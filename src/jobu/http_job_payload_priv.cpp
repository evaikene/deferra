#include "http_job_payload_priv.hpp"

#include "attempt.hpp"
#include "http_validation_priv.hpp"
#include "payload_template_priv.hpp"
#include "text_validation_priv.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace jb::jobu::detail {

namespace {

template <typename T>
using DecodeResult = jb::core::Result<T, JobPayloadIssue>;

struct ParsedHeader {
    jb::net::detail::HttpHeaderFields fields;
    bool                              sensitive{};
};

struct ParsedHttpPayload {
    std::string                         url;
    std::string                         method{"GET"};
    std::vector<ParsedHeader>           headers;
    std::optional<jb::core::ByteBuffer> body;
    bool                                body_is_reference{};
    std::vector<HttpStatusRange>        expected_statuses;
};

constexpr std::size_t kMaximumExpectedStatusSelectors{64};

auto member(jb::core::JsonValue::Object const& object, std::string_view name) -> jb::core::JsonValue const*
{
    auto const iterator = object.find(name);
    return iterator == object.end() ? nullptr : &iterator->second;
}

constexpr auto ascii_lower(unsigned char value) noexcept -> unsigned char
{
    if (value >= static_cast<unsigned char>('A') && value <= static_cast<unsigned char>('Z')) {
        return static_cast<unsigned char>(value + ('a' - 'A'));
    }
    return value;
}

auto ascii_equal(std::string_view lhs, std::string_view rhs) noexcept -> bool
{
    return lhs.size() == rhs.size() && std::ranges::equal(lhs, rhs, [](char left, char right) {
               return ascii_lower(static_cast<unsigned char>(left)) == ascii_lower(static_cast<unsigned char>(right));
           });
}

auto ascii_starts_with(std::string_view value, std::string_view prefix) noexcept -> bool
{
    return value.size() >= prefix.size() && ascii_equal(value.substr(0, prefix.size()), prefix);
}

auto jobu_reserved_header(std::string_view name) noexcept -> bool
{
    return ascii_starts_with(name, "X-JobU-") || ascii_equal(name, "Idempotency-Key");
}

auto forced_sensitive_header(std::string_view name) noexcept -> bool
{
    return ascii_equal(name, "Authorization") || ascii_equal(name, "Cookie") ||
           ascii_equal(name, "Proxy-Authorization");
}

void append_maximum_metadata_headers(std::vector<jb::net::detail::HttpHeaderFields>& headers)
{
    constexpr auto uuid_text = std::string_view{"00000000-0000-0000-0000-000000000000"};

    // Views borrow static strings, including the maximum-width attempt number.
    static auto const maximum_attempt = std::to_string(std::numeric_limits<AttemptNumber>::max());
    headers.reserve(headers.size() + 4U);
    headers.push_back({.name = "X-JobU-Job-ID", .value = uuid_text});
    headers.push_back({.name = "X-JobU-Run-ID", .value = uuid_text});
    headers.push_back({
        .name  = "X-JobU-Attempt",
        .value = maximum_attempt,
    });
    headers.push_back({.name = "Idempotency-Key", .value = uuid_text});
}

auto decode_headers(jb::core::JsonValue const* value, PayloadTemplateReferences* references)
    -> DecodeResult<std::vector<ParsedHeader>>
{
    auto headers = std::vector<ParsedHeader>{};
    if (value == nullptr) {
        return DecodeResult<std::vector<ParsedHeader>>::success(std::move(headers));
    }
    if (!value->is_array()) {
        return DecodeResult<std::vector<ParsedHeader>>::failure(JobPayloadIssue::InvalidHeaders);
    }

    headers.reserve(value->as_array().size());
    for (auto const& entry : value->as_array()) {
        if (!entry.is_object()) {
            return DecodeResult<std::vector<ParsedHeader>>::failure(JobPayloadIssue::InvalidHeaders);
        }
        auto const& object           = entry.as_object();
        auto const* name             = member(object, "name");
        auto const* data             = member(object, "value");
        auto const* sensitive        = member(object, "sensitive");
        auto const  expected_members = std::size_t{2} + (sensitive == nullptr ? 0U : 1U);
        if (object.size() != expected_members || name == nullptr || !name->is_string() || data == nullptr ||
            (sensitive != nullptr && !sensitive->is_bool()) || jobu_reserved_header(name->as_string())) {
            return DecodeResult<std::vector<ParsedHeader>>::failure(JobPayloadIssue::InvalidHeaders);
        }

        auto text = parse_payload_text(*data,
                                       JobPayloadIssue::InvalidHeaders,
                                       references,
                                       "/headers/" + std::to_string(headers.size()) + "/value");
        if (!text) {
            return DecodeResult<std::vector<ParsedHeader>>::failure(text.error());
        }

        // Credential fields are sensitive even when the persisted flag is absent or false.
        headers.push_back({
            .fields    = {.name = name->as_string(), .value = *text},
            .sensitive = forced_sensitive_header(name->as_string()) || (sensitive != nullptr && sensitive->as_bool()),
        });
    }
    return DecodeResult<std::vector<ParsedHeader>>::success(std::move(headers));
}

constexpr auto base64_value(unsigned char value) noexcept -> std::optional<std::uint8_t>
{
    if (value >= static_cast<unsigned char>('A') && value <= static_cast<unsigned char>('Z')) {
        return static_cast<std::uint8_t>(value - static_cast<unsigned char>('A'));
    }
    if (value >= static_cast<unsigned char>('a') && value <= static_cast<unsigned char>('z')) {
        return static_cast<std::uint8_t>(value - static_cast<unsigned char>('a') + 26U);
    }
    if (value >= static_cast<unsigned char>('0') && value <= static_cast<unsigned char>('9')) {
        return static_cast<std::uint8_t>(value - static_cast<unsigned char>('0') + 52U);
    }
    if (value == static_cast<unsigned char>('+')) {
        return 62U;
    }
    if (value == static_cast<unsigned char>('/')) {
        return 63U;
    }
    return std::nullopt;
}

auto decode_base64(std::string_view encoded) -> DecodeResult<jb::core::ByteBuffer>
{
    auto decoded = jb::core::ByteBuffer{};
    if (encoded.empty()) {
        return DecodeResult<jb::core::ByteBuffer>::success(std::move(decoded));
    }
    if (encoded.size() % 4U != 0U || encoded.size() / 4U > std::numeric_limits<std::size_t>::max() / 3U) {
        return DecodeResult<jb::core::ByteBuffer>::failure(JobPayloadIssue::InvalidBody);
    }

    auto const padding = static_cast<std::size_t>(encoded.back() == '=') +
                         static_cast<std::size_t>(encoded.size() >= 2U && encoded[encoded.size() - 2U] == '=');
    decoded.reserve(((encoded.size() / 4U) * 3U) - padding);

    // Padding is valid only in the final quantum. Reject nonzero unused bits
    // so alternate encodings of the same bytes are not accepted as canonical.
    for (std::size_t index = 0; index < encoded.size(); index += 4U) {
        auto const first  = base64_value(static_cast<unsigned char>(encoded[index]));
        auto const second = base64_value(static_cast<unsigned char>(encoded[index + 1U]));
        auto const final  = index + 4U == encoded.size();
        if (!first || !second) {
            return DecodeResult<jb::core::ByteBuffer>::failure(JobPayloadIssue::InvalidBody);
        }

        auto const third_padding  = encoded[index + 2U] == '=';
        auto const fourth_padding = encoded[index + 3U] == '=';
        if (third_padding) {
            if (!final || !fourth_padding || ((*second & 0x0fU) != 0U)) {
                return DecodeResult<jb::core::ByteBuffer>::failure(JobPayloadIssue::InvalidBody);
            }
            decoded.push_back(static_cast<std::byte>((*first << 2U) | (*second >> 4U)));
            continue;
        }

        auto const third = base64_value(static_cast<unsigned char>(encoded[index + 2U]));
        if (!third) {
            return DecodeResult<jb::core::ByteBuffer>::failure(JobPayloadIssue::InvalidBody);
        }
        decoded.push_back(static_cast<std::byte>((*first << 2U) | (*second >> 4U)));
        decoded.push_back(static_cast<std::byte>(((*second & 0x0fU) << 4U) | (*third >> 2U)));

        if (fourth_padding) {
            if (!final || ((*third & 0x03U) != 0U)) {
                return DecodeResult<jb::core::ByteBuffer>::failure(JobPayloadIssue::InvalidBody);
            }
            continue;
        }
        auto const fourth = base64_value(static_cast<unsigned char>(encoded[index + 3U]));
        if (!fourth) {
            return DecodeResult<jb::core::ByteBuffer>::failure(JobPayloadIssue::InvalidBody);
        }
        decoded.push_back(static_cast<std::byte>(((*third & 0x03U) << 6U) | *fourth));
    }
    return DecodeResult<jb::core::ByteBuffer>::success(std::move(decoded));
}

auto decode_body(jb::core::JsonValue const* value) -> DecodeResult<std::optional<jb::core::ByteBuffer>>
{
    if (value == nullptr) {
        return DecodeResult<std::optional<jb::core::ByteBuffer>>::success(std::nullopt);
    }
    if (!value->is_object()) {
        return DecodeResult<std::optional<jb::core::ByteBuffer>>::failure(JobPayloadIssue::InvalidBody);
    }
    auto const& object   = value->as_object();
    auto const* encoding = member(object, "encoding");
    auto const* data     = member(object, "data");
    if (object.size() != 2U || encoding == nullptr || !encoding->is_string() || data == nullptr || !data->is_string()) {
        return DecodeResult<std::optional<jb::core::ByteBuffer>>::failure(JobPayloadIssue::InvalidBody);
    }

    if (encoding->as_string() == "utf8") {
        if (!is_valid_utf8(data->as_string())) {
            return DecodeResult<std::optional<jb::core::ByteBuffer>>::failure(JobPayloadIssue::InvalidBody);
        }
        auto const bytes = jb::core::as_bytes(data->as_string());
        return DecodeResult<std::optional<jb::core::ByteBuffer>>::success(
            jb::core::ByteBuffer{bytes.begin(), bytes.end()});
    }
    if (encoding->as_string() == "base64") {
        auto decoded = decode_base64(data->as_string());
        if (!decoded) {
            return DecodeResult<std::optional<jb::core::ByteBuffer>>::failure(decoded.error());
        }
        return DecodeResult<std::optional<jb::core::ByteBuffer>>::success(std::move(decoded).value());
    }
    return DecodeResult<std::optional<jb::core::ByteBuffer>>::failure(JobPayloadIssue::InvalidBody);
}

auto parse_status(std::string_view text) noexcept -> std::optional<std::uint16_t>
{
    if (text.size() != 3U) {
        return std::nullopt;
    }
    auto value = std::uint16_t{0};
    for (auto const character : text) {
        if (character < '0' || character > '9') {
            return std::nullopt;
        }
        value = static_cast<std::uint16_t>((value * 10U) + static_cast<unsigned char>(character - '0'));
    }
    if (value < 100U || value > 599U) {
        return std::nullopt;
    }
    return value;
}

auto parse_status_selector_impl(std::string_view text) noexcept -> std::optional<HttpStatusRange>
{
    if (text.size() == 3U) {
        auto status = parse_status(text);
        if (!status) {
            return std::nullopt;
        }
        return HttpStatusRange{.first = *status, .last = *status};
    }
    if (text.size() != 7U || text[3] != '-') {
        return std::nullopt;
    }
    auto first = parse_status(text.substr(0, 3U));
    auto last  = parse_status(text.substr(4U));
    if (!first || !last || *first > *last) {
        return std::nullopt;
    }
    return HttpStatusRange{.first = *first, .last = *last};
}

auto decode_expected_statuses(jb::core::JsonValue const* value) -> DecodeResult<std::vector<HttpStatusRange>>
{
    auto ranges = std::vector<HttpStatusRange>{};
    if (value == nullptr) {
        ranges.push_back({.first = 200U, .last = 299U});
        return DecodeResult<std::vector<HttpStatusRange>>::success(std::move(ranges));
    }
    if (!value->is_array() || value->as_array().empty() || value->as_array().size() > kMaximumExpectedStatusSelectors) {
        return DecodeResult<std::vector<HttpStatusRange>>::failure(JobPayloadIssue::InvalidExpectedStatuses);
    }

    ranges.reserve(value->as_array().size());
    for (auto const& selector : value->as_array()) {
        if (!selector.is_string()) {
            return DecodeResult<std::vector<HttpStatusRange>>::failure(JobPayloadIssue::InvalidExpectedStatuses);
        }
        auto parsed = parse_http_status_selector(selector.as_string());
        if (!parsed) {
            return DecodeResult<std::vector<HttpStatusRange>>::failure(JobPayloadIssue::InvalidExpectedStatuses);
        }
        ranges.push_back(*parsed);
    }

    // Collapse overlaps and adjacent ranges into one bounded membership set;
    // duplicate selectors therefore have no effect on matching.
    std::ranges::sort(ranges, {}, &HttpStatusRange::first);
    auto normalized = std::vector<HttpStatusRange>{};
    normalized.reserve(ranges.size());
    for (auto const range : ranges) {
        if (normalized.empty() || range.first > normalized.back().last + 1U) {
            normalized.push_back(range);
            continue;
        }
        normalized.back().last = std::max(normalized.back().last, range.last);
    }
    return DecodeResult<std::vector<HttpStatusRange>>::success(std::move(normalized));
}

auto generic_request_issue(jb::core::Error const& error) noexcept -> JobPayloadIssue
{
    if (error.detail.starts_with("method.")) {
        return JobPayloadIssue::InvalidMethod;
    }
    if (error.detail.starts_with("url.")) {
        return JobPayloadIssue::InvalidUrl;
    }
    if (error.detail.starts_with("headers.")) {
        return JobPayloadIssue::InvalidHeaders;
    }
    if (error.detail.starts_with("body.")) {
        return JobPayloadIssue::InvalidBody;
    }
    return JobPayloadIssue::InvalidHttpRequest;
}

} // anonymous namespace

auto parse_http_status_selector(std::string_view selector) noexcept -> std::optional<HttpStatusRange>
{
    return parse_status_selector_impl(selector);
}

HttpStatusSet::HttpStatusSet(std::vector<HttpStatusRange> ranges) noexcept
    : _ranges{std::move(ranges)}
{}

auto HttpStatusSet::contains(std::uint16_t status) const noexcept -> bool
{
    return std::ranges::any_of(_ranges, [status](HttpStatusRange const& range) {
        return status >= range.first && status <= range.last;
    });
}

namespace {

auto parse_http_payload(jb::core::JsonValue const& payload,
                        PayloadTemplateReferences* references,
                        bool                       distinguish_size_failure = false) -> DecodeResult<ParsedHttpPayload>
{
    if (!payload.is_object()) {
        return DecodeResult<ParsedHttpPayload>::failure(JobPayloadIssue::NotObject);
    }
    auto const& object = payload.as_object();
    auto const* url    = member(object, "url");
    if (url == nullptr || !url->is_string() || url->as_string().empty()) {
        return DecodeResult<ParsedHttpPayload>::failure(JobPayloadIssue::MissingUrl);
    }

    auto request = ParsedHttpPayload{.url = url->as_string()};
    if (auto const* method = member(object, "method"); method != nullptr) {
        if (!method->is_string()) {
            return DecodeResult<ParsedHttpPayload>::failure(JobPayloadIssue::InvalidMethod);
        }
        request.method = method->as_string();
    }

    auto headers = decode_headers(member(object, "headers"), references);
    if (!headers) {
        return DecodeResult<ParsedHttpPayload>::failure(headers.error());
    }
    request.headers = std::move(headers).value();

    auto const* body_value = member(object, "body");
    // The body object has two disjoint grammars. Any secret member selects the closed reference grammar.
    if (references != nullptr && body_value != nullptr && body_value->is_object() &&
        body_value->as_object().contains("secret")) {
        auto const issue = references->add(*body_value, "/body");
        if (issue != JobPayloadIssue::None) {
            return DecodeResult<ParsedHttpPayload>::failure(issue);
        }
        request.body_is_reference = true;
    }
    else {
        auto body = decode_body(body_value);
        if (!body) {
            return DecodeResult<ParsedHttpPayload>::failure(body.error());
        }
        request.body = std::move(body).value();
    }

    auto statuses = decode_expected_statuses(member(object, "expected_statuses"));
    if (!statuses) {
        return DecodeResult<ParsedHttpPayload>::failure(statuses.error());
    }

    // Reserve metadata and check all known bytes. Unresolved header values remain absent views,
    // and a referenced body still counts as present for the HEAD restriction.
    auto fields = std::vector<jb::net::detail::HttpHeaderFields>{};
    fields.reserve(request.headers.size() + 4U);
    for (auto const& header : request.headers) {
        fields.push_back(header.fields);
    }
    append_maximum_metadata_headers(fields);
    auto validation =
        jb::net::detail::validate_http_request_fields(request.method,
                                                      request.url,
                                                      fields,
                                                      request.body.has_value() || request.body_is_reference);
    if (!validation) {
        // Preparation distinguishes expansion limits without changing existing decoder error identities.
        if (distinguish_size_failure && validation.error().detail == "headers.too_large") {
            return DecodeResult<ParsedHttpPayload>::failure(JobPayloadIssue::PreparedRequestTooLarge);
        }
        return DecodeResult<ParsedHttpPayload>::failure(generic_request_issue(validation.error()));
    }
    request.expected_statuses = std::move(statuses).value();
    return DecodeResult<ParsedHttpPayload>::success(std::move(request));
}

} // namespace

auto prepared_http_payload_issue(jb::core::JsonValue const& payload) -> JobPayloadIssue
{
    auto parsed = parse_http_payload(payload, nullptr, true);
    return parsed ? JobPayloadIssue::None : parsed.error();
}

auto decode_http_job_payload(jb::core::JsonValue const& payload) -> DecodeResult<HttpJobPayload>
{
    auto parsed = parse_http_payload(payload, nullptr);
    if (!parsed) {
        return DecodeResult<HttpJobPayload>::failure(parsed.error());
    }

    // Only the concrete path constructs execution-ready owning headers and body bytes.
    auto headers = std::vector<jb::net::HttpHeader>{};
    headers.reserve(parsed->headers.size());
    for (auto const& header : parsed->headers) {
        headers.push_back({.name      = std::string{header.fields.name},
                           .value     = std::string{*header.fields.value},
                           .sensitive = header.sensitive});
    }
    return DecodeResult<HttpJobPayload>::success({
        .url               = std::move(parsed->url),
        .method            = std::move(parsed->method),
        .headers           = std::move(headers),
        .body              = std::move(parsed->body),
        .expected_statuses = HttpStatusSet{std::move(parsed->expected_statuses)},
    });
}

auto validate_http_payload_template(jb::core::JsonValue const& payload, PayloadTemplateReferences& references)
    -> JobPayloadIssue
{
    auto parsed = parse_http_payload(payload, &references);
    return parsed ? JobPayloadIssue::None : parsed.error();
}

} // namespace jb::jobu::detail
