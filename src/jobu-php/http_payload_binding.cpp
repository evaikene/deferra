#include "http_payload_binding.hpp"
#include "value_binding.hpp"
#include "value_snapshot.hpp"

#include <jb/core/text_validation.hpp>
#include <phpcxx/arguments.hpp>
#include <phpcxx/conversion.hpp>
#include <phpcxx/exception.hpp>
#include <phpcxx/object_binding.hpp>
#include <phpcxx/string.hpp>

extern "C" {
#include <ext/json/php_json.h>
#include <ext/standard/base64.h>
}

#include <algorithm>
#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace jb::php {
namespace {

// Mirror the native admission contract without linking daemon-private validators.
// Secret-dependent sizes/bytes are necessarily checked again by the daemon.
constexpr std::size_t request_limit = std::size_t{256} * 1024U;
constexpr std::size_t header_limit  = 128U - 4U;
constexpr std::size_t header_bytes  = (std::size_t{64} * 1024U) - 183U; // Maximum executor metadata.

struct Text {
    std::string value;
    bool        secret                         = false;
    auto        operator<=>(Text const&) const = default;
};

struct Header {
    std::string name;
    Text        value;
    bool        sensitive                        = false;
    auto        operator<=>(Header const&) const = default;
};

struct Payload {
    std::string              url;
    std::string              method = "GET";
    std::vector<Header>      headers;
    std::optional<Text>      body;
    std::vector<std::string> statuses{"200-299"};
    auto                     operator<=>(Payload const&) const = default;
};

struct Configuration {
    explicit Configuration(Payload value) noexcept
        : value(std::move(value))
    {}

    auto trace(::php::GcVisitor& visitor) const noexcept -> void { visitor.add(owner.view()); }

    Payload                value;
    ::php::Value           owner;
    std::function<void()>  changed;
    ConfigurationEditCheck editable;
};

struct HeaderValue {
    explicit HeaderValue(Header value) noexcept
        : value(std::move(value))
    {}

    auto trace([[maybe_unused]] ::php::GcVisitor& visitor) const noexcept -> void {}

    Header value;
};

template <typename T>
auto compare(T const& left, T const& right) noexcept -> int
{
    auto order = left <=> right;
    if (std::is_lt(order)) {
        return -1;
    }
    return std::is_gt(order) ? 1 : 0;
}

struct Policy : ::php::ObjectBindingPolicy<Configuration> {
    static constexpr bool cloneable = true;

    static auto clone(Configuration const& source) noexcept -> ::php::Result<Configuration>
    {
        // Native values cross the clone boundary; owner and notification do not.
        return Configuration(source.value);
    }

    static auto compare(Configuration const& a, Configuration const& b) noexcept -> int
    {
        return jb::php::compare(a.value, b.value);
    }
};

struct HeaderPolicy : ::php::ObjectBindingPolicy<HeaderValue> {
    static auto compare(HeaderValue const& a, HeaderValue const& b) noexcept -> int
    {
        return jb::php::compare(a.value, b.value);
    }
};

static_assert(Policy::cloneable, "HTTP payload clones must detach from their owner");

using Binding       = ::php::ObjectBinding<Configuration, Policy>;
using HeaderBinding = ::php::ObjectBinding<HeaderValue, HeaderPolicy>;

auto invalid(std::string message = "Invalid HTTP payload value") -> ::php::Error
{
    return {.kind = ::php::ErrorKind::Value, .message = std::move(message)};
}

auto ascii_lower(char value) -> char
{
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
}

auto equal_name(std::string_view a, std::string_view b) -> bool
{
    return std::ranges::equal(a, b, [](char x, char y) { return ascii_lower(x) == ascii_lower(y); });
}

auto read_string(::php::Arguments const& args, std::uint32_t index) -> ::php::Result<std::string>
{
    auto value = args.string(index);
    if (!value) {
        return value.error();
    }
    // Own the bytes before another argument can invoke PHP and mutate a reference.
    return std::string(value.value().bytes());
}

auto read_text(::php::ValueView input) -> ::php::Result<Text>
{
    if (input.dereference().is_string()) {
        return Text{.value = std::string(::php::as_string(input).value().bytes())};
    }
    auto name = secret_name_from_php(input);
    if (!name) {
        return name.error();
    }
    return Text{.value = std::move(name).value(), .secret = true};
}

auto valid_url(std::string_view url) -> bool
{
    // Match native final-wire URL validation: printable ASCII, no fragment or
    // userinfo, HTTP(S), nonempty host, and a structurally valid port suffix.
    if (url.empty() || url.size() > std::size_t{16} * 1024U) {
        return false;
    }
    auto hex = [](char c) { return (c >= '0' && c <= '9') || (ascii_lower(c) >= 'a' && ascii_lower(c) <= 'f'); };
    for (std::size_t i = 0; i < url.size(); ++i) {
        auto c = static_cast<unsigned char>(url[i]);
        if (c < 0x21U || c > 0x7eU || c == '#') {
            return false;
        }
        if (c == '%') {
            if (i + 2U >= url.size() || !hex(url[i + 1U]) || !hex(url[i + 2U])) {
                return false;
            }
            i += 2U;
        }
    }
    auto scheme = url.find("://");
    if (scheme == std::string_view::npos ||
        (!equal_name(url.substr(0, scheme), "http") && !equal_name(url.substr(0, scheme), "https"))) {
        return false;
    }
    auto rest      = url.substr(scheme + 3U);
    auto authority = rest.substr(0, rest.find_first_of("/?"));
    if (authority.empty() || authority.find('@') != std::string_view::npos) {
        return false;
    }
    auto port = [](std::string_view suffix) {
        return suffix.size() > 1U && suffix.front() == ':' &&
               std::ranges::all_of(suffix.substr(1), [](char c) { return c >= '0' && c <= '9'; });
    };
    if (authority.front() == '[') {
        auto close = authority.find(']');
        if (close == std::string_view::npos || close == 1U || authority.find('[', 1U) != std::string_view::npos ||
            authority.find(']', close + 1U) != std::string_view::npos) {
            return false;
        }
        auto suffix = authority.substr(close + 1U);
        return suffix.empty() || port(suffix);
    }
    if (authority.find_first_of("[]") != std::string_view::npos) {
        return false;
    }
    auto colon = authority.find(':');
    return colon == std::string_view::npos || (colon != 0U && port(authority.substr(colon)));
}

auto read_method(std::string_view input) -> ::php::Result<std::string>
{
    for (auto const* method : {"GET", "POST", "PUT", "PATCH", "DELETE", "HEAD"}) {
        if (equal_name(input, method)) {
            return std::string(method);
        }
    }
    return invalid("Supported HTTP methods are GET, POST, PUT, PATCH, DELETE and HEAD");
}

auto valid_header_name(std::string_view name) -> bool
{
    return !name.empty() && std::ranges::all_of(name, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               std::string_view("!#$%&'*+-.^_`|~").find(c) != std::string_view::npos;
    });
}

auto read_header(::php::Arguments const& args) -> ::php::Result<Header>
{
    auto name = read_string(args, 1);
    if (!name) {
        return name.error();
    }
    auto value = read_text(args.get(2));
    if (!value) {
        return value.error();
    }
    bool sensitive = false;
    if (args.size() > 2) {
        auto flag = args.boolean(3);
        if (!flag) {
            return flag.error();
        }
        sensitive = flag.value();
    }
    if (!valid_header_name(name.value()) || equal_name(std::string_view(name.value()).substr(0, 7), "X-JobU-")) {
        return invalid("Invalid or reserved HTTP header name");
    }
    for (auto const* reserved : {"Host",
                                 "Content-Length",
                                 "Transfer-Encoding",
                                 "Connection",
                                 "Proxy-Connection",
                                 "TE",
                                 "Trailer",
                                 "Upgrade",
                                 "Idempotency-Key"}) {
        if (equal_name(name.value(), reserved)) {
            return invalid("Reserved HTTP header name");
        }
    }
    if (!value.value().secret && (!core::is_valid_utf8(value.value().value) ||
                                  value.value().value.find_first_of("\r\n\0", 0, 3) != std::string::npos)) {
        return invalid("HTTP header values must be UTF-8 without CR, LF or NUL");
    }
    sensitive = sensitive || value.value().secret || equal_name(name.value(), "Authorization") ||
                equal_name(name.value(), "Cookie") || equal_name(name.value(), "Proxy-Authorization");
    return Header{.name = std::move(name).value(), .value = std::move(value).value(), .sensitive = sensitive};
}

auto check_headers(std::vector<Header> const& headers, Header const* extra = nullptr) -> ::php::Result<void>
{
    if (headers.size() + (extra ? 1U : 0U) > header_limit) {
        return invalid("HTTP headers exceed the count limit");
    }
    std::size_t bytes = 0;
    auto        fits  = [&bytes](Header const& header) {
        auto size = header.name.size() + (header.value.secret ? 0U : header.value.value.size());
        if (size > header_bytes - bytes) {
            return false;
        }
        bytes += size;
        return true;
    };
    for (auto const& header : headers) {
        if (!fits(header)) {
            return invalid("HTTP headers exceed the byte limit");
        }
    }
    if (extra && !fits(*extra)) {
        return invalid("HTTP headers exceed the byte limit");
    }
    return {};
}

auto valid_status(std::string_view text) -> bool
{
    return text.size() == 3U && text[0] >= '1' && text[0] <= '5' && text[1] >= '0' && text[1] <= '9' &&
           text[2] >= '0' && text[2] <= '9';
}

auto read_statuses(::php::ValueView input) -> ::php::Result<std::vector<std::string>>
{
    auto array = ::php::as_array(input);
    if (!array) {
        return array.error();
    }
    if (!zend_array_is_list(const_cast<zend_array*>(array.value().get())) || array.value().empty() ||
        array.value().size() > 64U) {
        return invalid("Expected statuses must be a nonempty list of at most 64 selectors");
    }
    std::vector<std::string> result;
    for (auto entry : array.value()) {
        auto value = ::php::as_string(entry.value);
        if (!value) {
            return value.error();
        }
        auto text = value.value().bytes();
        if (!valid_status(text) && !(text.size() == 7U && text[3] == '-' && valid_status(text.substr(0, 3)) &&
                                     valid_status(text.substr(4)) && text.substr(0, 3) <= text.substr(4))) {
            return invalid("Invalid HTTP status selector");
        }
        result.emplace_back(text);
    }
    return result;
}

auto encode_json(::php::ValueView input) -> ::php::Result<Text>
{
    auto* function = static_cast<zend_function*>(zend_hash_str_find_ptr(EG(function_table), "json_encode", 11));
    if (!function || function->type != ZEND_INTERNAL_FUNCTION) {
        return invalid("PHP JSON encoder is unavailable");
    }
    // Use PHP's public entry point to preserve JsonException, json_last_error,
    // JsonSerializable, and callback-exception semantics across supported ABIs.
    auto argument = ::php::Value::retain(input.dereference());
    zval params[3]{};
    ZVAL_COPY_VALUE(&params[0], argument.get());
    ZVAL_LONG(&params[1], PHP_JSON_THROW_ON_ERROR);
    ZVAL_LONG(&params[2], 512);
    zval slot{};
    zend_call_known_function(function, nullptr, nullptr, &slot, 3, params, nullptr);
    auto result = ::php::Value::adopt(slot);
    if (::php::has_exception()) {
        return ::php::pending_exception();
    }
    return read_text(result.view());
}

auto text_json(Text const& text) -> core::JsonValue
{
    if (text.secret) {
        return {.data = core::JsonValue::Object{{"secret", {.data = text.value}}}};
    }
    return {.data = text.value};
}

auto export_payload(Payload const& payload) -> ::php::Result<core::JsonValue>
{
    core::JsonValue::Array headers;
    for (auto const& header : payload.headers) {
        headers.push_back({
            .data = core::JsonValue::Object{{"name", {.data = header.name}},
                                            {"value", text_json(header.value)},
                                            {"sensitive", {.data = header.sensitive}}}
        });
    }
    core::JsonValue::Array statuses;
    for (auto const& status : payload.statuses) {
        statuses.push_back({.data = status});
    }
    core::JsonValue::Object result{
        {"url",               {.data = payload.url}        },
        {"method",            {.data = payload.method}     },
        {"headers",           {.data = std::move(headers)} },
        {"expected_statuses", {.data = std::move(statuses)}}
    };
    if (payload.body) {
        auto const& body = *payload.body;
        if (body.secret) {
            result.emplace("body", text_json(body));
        }
        else {
            bool        utf8 = core::is_valid_utf8(body.value);
            std::string data = body.value;
            if (!utf8) {
                auto encoded = ::php::String::adopt(
                    php_base64_encode(reinterpret_cast<unsigned char const*>(data.data()), data.size()));
                data = encoded.view().bytes();
            }
            result.emplace(
                "body",
                core::JsonValue{
                    .data = core::JsonValue::Object{{"encoding", {.data = std::string(utf8 ? "utf8" : "base64")}},
                                                    {"data", {.data = std::move(data)}}}
            });
        }
    }
    core::JsonValue value{.data = std::move(result)};
    auto            encoded = core::serialize_json(value);
    if (!encoded || encoded->size() > request_limit) {
        return invalid("HTTP payload exceeds the size limit");
    }
    return value;
}

auto native(::php::ValueView input) -> ::php::Result<Configuration*>
{
    auto value = input.dereference();
    if (!value.is_object()) {
        return ::php::Error{.kind = ::php::ErrorKind::Type, .message = "Expected HttpPayload"};
    }
    return Binding::get(Z_OBJ_P(value.get()));
}

auto loaded_text(core::JsonValue const& value) -> ::php::Result<Text>
{
    if (value.is_string()) {
        return Text{.value = value.as_string()};
    }
    auto const* secret = snapshot::field(value, "secret");
    if (!secret || !secret->is_string()) {
        return invalid("Invalid payload text in server reply");
    }
    return Text{.value = secret->as_string(), .secret = true};
}

auto php_text(Text const& text) -> ::php::Result<::php::Value>
{
    if (text.secret) {
        return secret_name_to_php(text.value);
    }
    zval slot{};
    ZVAL_STRINGL(&slot, text.value.data(), text.value.size());
    return ::php::Value::adopt(slot);
}

auto php_header(Header const& header) -> ::php::Result<::php::Value>
{
    auto* entry = static_cast<zend_class_entry*>(zend_hash_str_find_ptr(EG(class_table), "jobu\\httpheader", 15));
    zval  slot{};
    object_init_ex(&slot, entry);
    auto result  = ::php::Value::adopt(slot);
    auto created = HeaderBinding::construct(Z_OBJ_P(result.get()), Header(header));
    if (!created) {
        return created.error();
    }
    return result;
}

template <typename T>
auto update(T& field, T value) -> bool
{
    if (field == value) {
        return false;
    }
    field = std::move(value);
    return true;
}

enum class Edit : std::uint8_t {
    Url,
    Method,
    AddHeader,
    ClearHeader,
    Body,
    ClearBody,
    JsonBody,
    Statuses
};

auto edit(::php::Arguments const& args, Edit action, Payload& target, ConfigurationEditCheck const& editable)
    -> ::php::Result<bool>
{
    // Conversion may invoke PHP. Never snapshot the payload before conversion
    // or replace unrelated fields afterward; nested edits are independent.
    if (action == Edit::Url || action == Edit::Method || action == Edit::ClearHeader) {
        auto text = read_string(args, 1);
        if (!text) {
            return text.error();
        }
        auto allowed = check_configuration_edit(editable);
        if (!allowed) {
            return allowed.error();
        }
        if (action == Edit::Url) {
            if (!valid_url(text.value())) {
                return invalid("Invalid HTTP URL");
            }
            return update(target.url, std::move(text).value());
        }
        if (action == Edit::ClearHeader) {
            if (!valid_header_name(text.value())) {
                return invalid("Invalid HTTP header name");
            }
            return std::erase_if(target.headers, [&](Header const& h) { return equal_name(h.name, text.value()); }) !=
                   0;
        }
        auto method = read_method(std::move(text).value());
        if (!method) {
            return method.error();
        }
        if (method.value() == "HEAD" && target.body) {
            return invalid("HEAD cannot have a request body");
        }
        return update(target.method, std::move(method).value());
    }
    if (action == Edit::AddHeader) {
        auto header = read_header(args);
        if (!header) {
            return header.error();
        }
        auto allowed = check_configuration_edit(editable);
        if (!allowed) {
            return allowed.error();
        }
        auto checked = check_headers(target.headers, &header.value());
        if (!checked) {
            return checked.error();
        }
        target.headers.push_back(std::move(header).value());
        return true;
    }
    if (action == Edit::Statuses) {
        auto statuses = read_statuses(args.get(1));
        if (!statuses) {
            return statuses.error();
        }
        return update(target.statuses, std::move(statuses).value());
    }
    if (action == Edit::ClearBody) {
        return update(target.body, std::optional<Text>{});
    }

    auto body = action == Edit::JsonBody ? encode_json(args.get(1)) : read_text(args.get(1));
    if (!body) {
        return body.error();
    }
    auto allowed = check_configuration_edit(editable);
    if (!allowed) {
        return allowed.error();
    }
    // JsonSerializable can change method or headers. Check current state only
    // after it returns, and commit body plus an optional Content-Type together.
    if (target.method == "HEAD" || body.value().value.size() > request_limit) {
        return invalid("HTTP body is not allowed or exceeds the size limit");
    }
    bool   add_type = action == Edit::JsonBody && std::ranges::none_of(target.headers, [](Header const& h) {
                        return equal_name(h.name, "Content-Type");
                      });
    Header content_type{.name = "Content-Type", .value = {.value = "application/json"}};
    if (add_type) {
        auto checked = check_headers(target.headers, &content_type);
        if (!checked) {
            return checked.error();
        }
        target.headers.push_back(std::move(content_type));
    }
    bool changed = update(target.body, std::optional<Text>(std::move(body).value()));
    return changed || add_type;
}

auto construct(zend_execute_data* frame) noexcept -> void
{
    ::php::Arguments args(frame);
    auto             count = args.count(1, 2);
    if (!count) {
        ::php::raise(count.error());
        return;
    }
    auto url = read_string(args, 1);
    if (!url) {
        ::php::raise(url.error());
        return;
    }
    auto method = args.size() > 1 ? read_string(args, 2) : ::php::Result<std::string>(std::string("GET"));
    if (!method) {
        ::php::raise(method.error());
        return;
    }
    auto canonical = read_method(std::move(method).value());
    if (!canonical) {
        ::php::raise(canonical.error());
        return;
    }
    if (!valid_url(url.value())) {
        ::php::raise(invalid("Invalid HTTP URL"));
        return;
    }
    auto result = Binding::construct(Z_OBJ(frame->This),
                                     Payload{.url = std::move(url).value(), .method = std::move(canonical).value()});
    if (!result) {
        ::php::raise(result.error());
    }
}

auto mutate(zend_execute_data* frame, Edit action) noexcept -> void
{
    ::php::Arguments args(frame);
    std::uint32_t    minimum = 1;
    std::uint32_t    maximum = 1;
    if (action == Edit::ClearBody) {
        minimum = maximum = 0;
    }
    else if (action == Edit::AddHeader) {
        minimum = 2;
        maximum = 3;
    }
    auto count = args.count(minimum, maximum);
    if (!count) {
        ::php::raise(count.error());
        return;
    }
    auto target = Binding::get(Z_OBJ(frame->This));
    if (!target) {
        ::php::raise(target.error());
        return;
    }
    auto allowed = check_configuration_edit(target.value()->editable);
    if (!allowed) {
        ::php::raise(allowed.error());
        return;
    }
    auto changed = edit(args, action, target.value()->value, target.value()->editable);
    if (!changed) {
        ::php::raise(changed.error());
        return;
    }
    if (changed.value() && target.value()->changed) {
        target.value()->changed();
    }
}

enum class Field : std::uint8_t {
    Url,
    Method,
    Headers,
    Body,
    Statuses,
    HeaderName,
    HeaderValue,
    Sensitive
};

auto read(zend_execute_data* frame, zval* output, Field field) noexcept -> void
{
    auto count = ::php::Arguments(frame).count(0, 0);
    if (!count) {
        ::php::raise(count.error());
        return;
    }
    if (field == Field::HeaderName || field == Field::HeaderValue || field == Field::Sensitive) {
        auto header = HeaderBinding::get(Z_OBJ(frame->This));
        if (!header) {
            ::php::raise(header.error());
            return;
        }
        auto const& value = header.value()->value;
        if (field == Field::Sensitive) {
            ZVAL_BOOL(output, value.sensitive);
        }
        else {
            auto text = php_text(field == Field::HeaderName ? Text{.value = value.name} : value.value);
            if (!text) {
                ::php::raise(text.error());
                return;
            }
            *output = text.value().release();
        }
        return;
    }
    auto target = Binding::get(Z_OBJ(frame->This));
    if (!target) {
        ::php::raise(target.error());
        return;
    }
    auto const& value = target.value()->value;
    if (field == Field::Url || field == Field::Method) {
        auto const& text = field == Field::Url ? value.url : value.method;
        ZVAL_STRINGL(output, text.data(), text.size());
        return;
    }
    if (field == Field::Body) {
        auto text = value.body ? php_text(*value.body) : ::php::Result<::php::Value>(::php::Value::null());
        if (!text) {
            ::php::raise(text.error());
            return;
        }
        *output = text.value().release();
        return;
    }
    zval slot{};
    array_init(&slot);
    auto owner = ::php::Value::adopt(slot);
    if (field == Field::Headers) {
        for (auto const& header : value.headers) {
            auto item = php_header(header);
            if (!item) {
                ::php::raise(item.error());
                return;
            }
            auto released = item.value().release();
            zend_hash_next_index_insert(Z_ARR_P(owner.get()), &released);
        }
    }
    else {
        for (auto const& status : value.statuses) {
            zval item{};
            ZVAL_STRINGL(&item, status.data(), status.size());
            zend_hash_next_index_insert(Z_ARR_P(owner.get()), &item);
        }
    }
    *output = owner.release();
}

} // namespace

auto bind_http_payload_classes(zend_class_entry* payload, zend_class_entry* header) -> bool
{
    payload->ce_flags |= ZEND_ACC_NO_DYNAMIC_PROPERTIES;
    header->ce_flags  |= ZEND_ACC_NO_DYNAMIC_PROPERTIES;
    return static_cast<bool>(Binding::bind(payload)) && static_cast<bool>(HeaderBinding::bind(header));
}

auto equal_http_payload(::php::ValueView left, ::php::ValueView right) -> bool
{
    return native(left).value()->value == native(right).value()->value;
}

auto http_payload_to_php(core::JsonValue const& value) -> ::php::Result<::php::Value>
{
    Payload     payload;
    auto const* url = snapshot::field(value, "url");
    if (!url || !url->is_string()) {
        return invalid("Invalid HTTP URL in server reply");
    }
    payload.url = url->as_string();
    if (auto const* method = snapshot::field(value, "method")) {
        if (!method->is_string()) {
            return invalid("Invalid HTTP method in server reply");
        }
        // Historical templates retain their exact method spelling. Admission
        // validation applies when explicitly replacing a payload, not reading it.
        payload.method = method->as_string();
    }
    if (auto const* headers = snapshot::field(value, "headers")) {
        if (!headers->is_array()) {
            return invalid("Invalid HTTP headers in server reply");
        }
        for (auto const& item : headers->as_array()) {
            auto const* name      = snapshot::field(item, "name");
            auto const* text      = snapshot::field(item, "value");
            auto const* sensitive = snapshot::field(item, "sensitive");
            if (!name || !name->is_string() || !text || (sensitive && !sensitive->is_bool())) {
                return invalid("Invalid HTTP header in server reply");
            }
            auto parsed = loaded_text(*text);
            if (!parsed) {
                return parsed.error();
            }
            bool protected_header = (sensitive != nullptr && sensitive->as_bool()) || parsed.value().secret ||
                                    equal_name(name->as_string(), "Authorization") ||
                                    equal_name(name->as_string(), "Cookie") ||
                                    equal_name(name->as_string(), "Proxy-Authorization");
            payload.headers.push_back(
                {.name = name->as_string(), .value = std::move(parsed).value(), .sensitive = protected_header});
        }
    }
    if (auto const* body = snapshot::field(value, "body")) {
        if (snapshot::field(*body, "secret")) {
            auto text = loaded_text(*body);
            if (!text) {
                return text.error();
            }
            payload.body = std::move(text).value();
        }
        else {
            auto const* encoding = snapshot::field(*body, "encoding");
            auto const* data     = snapshot::field(*body, "data");
            if (!encoding || !encoding->is_string() || !data || !data->is_string()) {
                return invalid("Invalid HTTP body in server reply");
            }
            std::string bytes = data->as_string();
            if (encoding->as_string() == "base64") {
                auto* decoded =
                    php_base64_decode_ex(reinterpret_cast<unsigned char const*>(bytes.data()), bytes.size(), true);
                if (!decoded) {
                    return invalid("Invalid HTTP body encoding in server reply");
                }
                auto owner = ::php::String::adopt(decoded);
                bytes      = owner.view().bytes();
            }
            else if (encoding->as_string() != "utf8") {
                return invalid("Invalid HTTP body encoding in server reply");
            }
            payload.body = Text{.value = std::move(bytes)};
        }
    }
    if (auto const* statuses = snapshot::field(value, "expected_statuses")) {
        auto converted = snapshot::to_php(*statuses);
        if (!converted) {
            return converted.error();
        }
        auto parsed = read_statuses(converted.value().view());
        if (!parsed) {
            return parsed.error();
        }
        payload.statuses = std::move(parsed).value();
    }

    zval slot{};
    object_init_ex(&slot, snapshot::entry("jobu\\httppayload"));
    auto owner       = ::php::Value::adopt(slot);
    auto constructed = Binding::construct(Z_OBJ_P(owner.get()), std::move(payload));
    if (!constructed) {
        return constructed.error();
    }
    return owner;
}

auto http_payload_from_php(::php::ValueView value) -> ::php::Result<core::JsonValue>
{
    auto source = native(value);
    if (!source) {
        return source.error();
    }
    return export_payload(source.value()->value);
}

auto replace_http_payload(zend_object* target, ::php::ValueView source) -> ::php::Result<void>
{
    auto destination = Binding::get(target);
    if (!destination) {
        return destination.error();
    }
    auto supplied = native(source);
    if (!supplied) {
        return supplied.error();
    }
    auto checked = export_payload(supplied.value()->value);
    if (!checked) {
        return checked.error();
    }
    auto allowed = check_configuration_edit(destination.value()->editable);
    if (!allowed) {
        return allowed.error();
    }
    if (update(destination.value()->value, supplied.value()->value) && destination.value()->changed) {
        destination.value()->changed();
    }
    return {};
}

auto prepare_http_payload_restore(zend_object* target, ::php::ValueView source) -> ::php::Result<ConfigurationRestore>
{
    auto destination = Binding::get(target);
    if (!destination) {
        return destination.error();
    }
    auto supplied = native(source);
    if (!supplied) {
        return supplied.error();
    }
    auto checked = export_payload(supplied.value()->value);
    if (!checked) {
        return checked.error();
    }
    return ConfigurationRestore([target = destination.value(), value = supplied.value()->value]() mutable {
        target->value = std::move(value);
    });
}

auto attach_http_payload(zend_object*           target,
                         ::php::ValueView       owner,
                         std::function<void()>  changed,
                         ConfigurationEditCheck editable) -> ::php::Result<void>
{
    auto config = Binding::get(target);
    if (!config) {
        return config.error();
    }
    if (!owner.dereference().is_object() || config.value()->owner.view().is_initialized()) {
        return invalid("Payload requires an object owner and may only be attached once");
    }
    config.value()->owner    = ::php::Value::retain(owner.dereference());
    config.value()->changed  = std::move(changed);
    config.value()->editable = std::move(editable);
    return {};
}

} // namespace jb::php

ZEND_METHOD(JobU_HttpPayload, __construct)
{
    (void)return_value;
    jb::php::construct(execute_data);
}

ZEND_METHOD(JobU_HttpHeader, __construct)
{
    (void)execute_data;
    (void)return_value;
    ::php::raise(
        {.kind = ::php::ErrorKind::Runtime, .message = "HttpHeader values come from HttpPayload::getHeaders()"});
}

ZEND_METHOD(JobU_HttpPayload, getUrl)
{
    jb::php::read(execute_data, return_value, jb::php::Field::Url);
}

ZEND_METHOD(JobU_HttpPayload, getMethod)
{
    jb::php::read(execute_data, return_value, jb::php::Field::Method);
}

ZEND_METHOD(JobU_HttpPayload, getHeaders)
{
    jb::php::read(execute_data, return_value, jb::php::Field::Headers);
}

ZEND_METHOD(JobU_HttpPayload, getBody)
{
    jb::php::read(execute_data, return_value, jb::php::Field::Body);
}

ZEND_METHOD(JobU_HttpPayload, getExpectedStatuses)
{
    jb::php::read(execute_data, return_value, jb::php::Field::Statuses);
}

ZEND_METHOD(JobU_HttpHeader, getName)
{
    jb::php::read(execute_data, return_value, jb::php::Field::HeaderName);
}

ZEND_METHOD(JobU_HttpHeader, getValue)
{
    jb::php::read(execute_data, return_value, jb::php::Field::HeaderValue);
}

ZEND_METHOD(JobU_HttpHeader, isSensitive)
{
    jb::php::read(execute_data, return_value, jb::php::Field::Sensitive);
}

ZEND_METHOD(JobU_HttpPayload, setUrl)
{
    (void)return_value;
    jb::php::mutate(execute_data, jb::php::Edit::Url);
}

ZEND_METHOD(JobU_HttpPayload, setMethod)
{
    (void)return_value;
    jb::php::mutate(execute_data, jb::php::Edit::Method);
}

ZEND_METHOD(JobU_HttpPayload, addHeader)
{
    (void)return_value;
    jb::php::mutate(execute_data, jb::php::Edit::AddHeader);
}

ZEND_METHOD(JobU_HttpPayload, clearHeader)
{
    (void)return_value;
    jb::php::mutate(execute_data, jb::php::Edit::ClearHeader);
}

ZEND_METHOD(JobU_HttpPayload, setBody)
{
    (void)return_value;
    jb::php::mutate(execute_data, jb::php::Edit::Body);
}

ZEND_METHOD(JobU_HttpPayload, clearBody)
{
    (void)return_value;
    jb::php::mutate(execute_data, jb::php::Edit::ClearBody);
}

ZEND_METHOD(JobU_HttpPayload, setJsonBody)
{
    (void)return_value;
    jb::php::mutate(execute_data, jb::php::Edit::JsonBody);
}

ZEND_METHOD(JobU_HttpPayload, setExpectedStatuses)
{
    (void)return_value;
    jb::php::mutate(execute_data, jb::php::Edit::Statuses);
}
