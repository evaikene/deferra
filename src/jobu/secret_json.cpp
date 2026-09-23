#include "secret_json.hpp"

#include "payload_template_priv.hpp"
#include "secret_repository_priv.hpp"
#include "text_validation_priv.hpp"
#include "utc_timestamp.hpp"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace jb::jobu {

namespace {

using jb::core::ByteBuffer;
using jb::core::Error;
using jb::core::ErrorCategory;
using jb::core::JsonValue;

template <typename T>
using ConversionResult = jb::core::Result<T, Error>;

auto invalid(bool request) -> Error
{
    return {.category = ErrorCategory::InvalidArgument,
            .code     = request ? "jobu.protocol.invalid_request" : "jobu.protocol.invalid_response",
            .message  = request ? "The JobU secret request is invalid" : "The JobU secret response is invalid"};
}

template <typename T>
auto reject(bool request) -> ConversionResult<T>
{
    return ConversionResult<T>::failure(invalid(request));
}

auto too_large() -> Error
{
    return {.category = ErrorCategory::ResourceExhausted,
            .code     = "jobu.secret.too_large",
            .message  = "Secret exceeds its raw byte limit"};
}

auto json(auto value) -> JsonValue
{
    return JsonValue{.data = std::move(value)};
}

auto member(JsonValue::Object const& object, std::string_view name) -> JsonValue const*
{
    auto found = object.find(name);
    return found == object.end() ? nullptr : &found->second;
}

auto only_members(JsonValue::Object const& object, std::initializer_list<std::string_view> allowed) -> bool
{
    for (auto const& [name, value] : object) {
        static_cast<void>(value);
        auto permitted = false;
        for (auto candidate : allowed) {
            if (name == candidate) {
                permitted = true;
                break;
            }
        }
        if (!permitted) {
            return false;
        }
    }
    return true;
}

auto checked_json(JsonValue value, bool request) -> ConversionResult<JsonValue>
{
    if (!jb::core::serialize_json(value)) {
        return reject<JsonValue>(request);
    }
    return ConversionResult<JsonValue>::success(std::move(value));
}

auto base64_value(unsigned char value) noexcept -> std::optional<std::uint8_t>
{
    if (value >= 'A' && value <= 'Z') {
        return static_cast<std::uint8_t>(value - 'A');
    }
    if (value >= 'a' && value <= 'z') {
        return static_cast<std::uint8_t>(value - 'a' + 26U);
    }
    if (value >= '0' && value <= '9') {
        return static_cast<std::uint8_t>(value - '0' + 52U);
    }
    if (value == '+') {
        return 62U;
    }
    if (value == '/') {
        return 63U;
    }
    return std::nullopt;
}

auto encode_base64(ByteBuffer const& bytes) -> std::string
{
    constexpr auto alphabet = std::string_view{"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"};
    auto           encoded  = std::string{};
    encoded.reserve(((bytes.size() + 2U) / 3U) * 4U);
    for (auto offset = std::size_t{0}; offset < bytes.size(); offset += 3U) {
        auto const first  = std::to_integer<std::uint8_t>(bytes[offset]);
        auto const second = offset + 1U < bytes.size() ? std::to_integer<std::uint8_t>(bytes[offset + 1U]) : 0U;
        auto const third  = offset + 2U < bytes.size() ? std::to_integer<std::uint8_t>(bytes[offset + 2U]) : 0U;
        encoded.push_back(alphabet[first >> 2U]);
        encoded.push_back(alphabet[((first & 3U) << 4U) | (second >> 4U)]);
        encoded.push_back(offset + 1U < bytes.size() ? alphabet[((second & 15U) << 2U) | (third >> 6U)] : '=');
        encoded.push_back(offset + 2U < bytes.size() ? alphabet[third & 63U] : '=');
    }
    return encoded;
}

auto decode_base64(std::string_view encoded) -> ConversionResult<ByteBuffer>
{
    constexpr auto maximum_chars = ((detail::kMaximumSecretValueBytes + 2U) / 3U) * 4U;
    if (encoded.size() > maximum_chars) {
        return ConversionResult<ByteBuffer>::failure(too_large());
    }
    if (encoded.size() % 4U != 0U) {
        return reject<ByteBuffer>(true);
    }

    auto const padding      = static_cast<std::size_t>(!encoded.empty() && encoded.back() == '=') +
                              static_cast<std::size_t>(encoded.size() >= 2U && encoded[encoded.size() - 2U] == '=');
    auto const decoded_size = ((encoded.size() / 4U) * 3U) - padding;
    if (decoded_size > detail::kMaximumSecretValueBytes) {
        return ConversionResult<ByteBuffer>::failure(too_large());
    }

    auto bytes = ByteBuffer{};
    bytes.reserve(decoded_size);

    // Padding belongs only to the last quantum. Unused trailing bits must be zero so the input is canonical.
    for (auto index = std::size_t{0}; index < encoded.size(); index += 4U) {
        auto const first  = base64_value(static_cast<unsigned char>(encoded[index]));
        auto const second = base64_value(static_cast<unsigned char>(encoded[index + 1U]));
        auto const final  = index + 4U == encoded.size();
        if (!first || !second) {
            return reject<ByteBuffer>(true);
        }
        if (encoded[index + 2U] == '=') {
            if (!final || encoded[index + 3U] != '=' || (*second & 15U) != 0U) {
                return reject<ByteBuffer>(true);
            }
            bytes.push_back(static_cast<std::byte>((*first << 2U) | (*second >> 4U)));
            continue;
        }
        auto const third = base64_value(static_cast<unsigned char>(encoded[index + 2U]));
        if (!third) {
            return reject<ByteBuffer>(true);
        }
        bytes.push_back(static_cast<std::byte>((*first << 2U) | (*second >> 4U)));
        bytes.push_back(static_cast<std::byte>(((*second & 15U) << 4U) | (*third >> 2U)));
        if (encoded[index + 3U] == '=') {
            if (!final || (*third & 3U) != 0U) {
                return reject<ByteBuffer>(true);
            }
            continue;
        }
        auto const fourth = base64_value(static_cast<unsigned char>(encoded[index + 3U]));
        if (!fourth) {
            return reject<ByteBuffer>(true);
        }
        bytes.push_back(static_cast<std::byte>(((*third & 3U) << 6U) | *fourth));
    }
    return ConversionResult<ByteBuffer>::success(std::move(bytes));
}

auto valid_page(SecretPage const& page) -> bool
{
    if (page.items.size() > 200U) {
        return false;
    }
    for (auto index = std::size_t{0}; index < page.items.size(); ++index) {
        if (!detail::is_valid_secret_name(page.items[index].name) ||
            (index > 0 && page.items[index - 1U].name >= page.items[index].name)) {
            return false;
        }
    }
    return !page.next_after_name || (!page.items.empty() && *page.next_after_name == page.items.back().name);
}

} // namespace

auto set_secret_request_to_json(SetSecretRequest const& request) -> ConversionResult<JsonValue>
{
    if (request.value.size() > detail::kMaximumSecretValueBytes) {
        return ConversionResult<JsonValue>::failure(too_large());
    }
    return checked_json(json(JsonValue::Object{
                            {"name",  json(request.name)},
                            {"value",
                             json(JsonValue::Object{
                                 {"encoding", json(std::string{"base64"})},
                                 {"data", json(encode_base64(request.value))},
                             })                         },
    }),
                        true);
}

auto set_secret_request_from_json(JsonValue const& value) -> ConversionResult<SetSecretRequest>
{
    if (!value.is_object() || !only_members(value.as_object(), {"name", "value"})) {
        return reject<SetSecretRequest>(true);
    }
    auto const* name    = member(value.as_object(), "name");
    auto const* encoded = member(value.as_object(), "value");
    if (name == nullptr || !name->is_string() || encoded == nullptr || !encoded->is_object() ||
        !only_members(encoded->as_object(), {"encoding", "data"})) {
        return reject<SetSecretRequest>(true);
    }
    auto const* encoding = member(encoded->as_object(), "encoding");
    auto const* data     = member(encoded->as_object(), "data");
    if (!encoding || !encoding->is_string() || !data || !data->is_string()) {
        return reject<SetSecretRequest>(true);
    }

    auto request = SetSecretRequest{.name = name->as_string()};
    if (encoding->as_string() == "utf8") {
        auto const& text = data->as_string();
        if (!detail::is_valid_utf8(text)) {
            return reject<SetSecretRequest>(true);
        }
        if (text.size() > detail::kMaximumSecretValueBytes) {
            return ConversionResult<SetSecretRequest>::failure(too_large());
        }
        auto bytes    = jb::core::as_bytes(text);
        request.value = ByteBuffer{bytes.begin(), bytes.end()};
    }
    else if (encoding->as_string() == "base64") {
        auto decoded = decode_base64(data->as_string());
        if (!decoded) {
            return ConversionResult<SetSecretRequest>::failure(std::move(decoded).error());
        }
        request.value = std::move(decoded).value();
    }
    else {
        return reject<SetSecretRequest>(true);
    }
    return ConversionResult<SetSecretRequest>::success(std::move(request));
}

auto secret_list_request_to_json(SecretListRequest const& request) -> ConversionResult<JsonValue>
{
    auto object = JsonValue::Object{
        {"limit", json(static_cast<std::uint64_t>(request.limit))}
    };
    if (request.after_name) {
        object.emplace("after_name", json(*request.after_name));
    }
    return checked_json(json(std::move(object)), true);
}

auto secret_list_request_from_json(JsonValue const& value) -> ConversionResult<SecretListRequest>
{
    if (!value.is_object() || !only_members(value.as_object(), {"limit", "after_name"})) {
        return reject<SecretListRequest>(true);
    }
    auto request = SecretListRequest{};
    if (auto const* limit = member(value.as_object(), "limit")) {
        auto decoded = std::uint64_t{};
        if (limit->is_uint()) {
            decoded = limit->as_uint();
        }
        else if (limit->is_int() && limit->as_int() >= 0) {
            decoded = static_cast<std::uint64_t>(limit->as_int());
        }
        else {
            return reject<SecretListRequest>(true);
        }
        if (decoded > std::numeric_limits<std::size_t>::max()) {
            return reject<SecretListRequest>(true);
        }
        request.limit = static_cast<std::size_t>(decoded);
    }
    if (auto const* after = member(value.as_object(), "after_name")) {
        if (!after->is_string()) {
            return reject<SecretListRequest>(true);
        }
        request.after_name = after->as_string();
    }
    return ConversionResult<SecretListRequest>::success(std::move(request));
}

auto secret_delete_request_to_json(std::string_view name) -> ConversionResult<JsonValue>
{
    return checked_json(json(JsonValue::Object{
                            {"name", json(std::string{name})}
    }),
                        true);
}

auto secret_delete_request_from_json(JsonValue const& value) -> ConversionResult<std::string>
{
    if (!value.is_object() || !only_members(value.as_object(), {"name"})) {
        return reject<std::string>(true);
    }
    auto const* name = member(value.as_object(), "name");
    if (!name || !name->is_string()) {
        return reject<std::string>(true);
    }
    return ConversionResult<std::string>::success(name->as_string());
}

auto secret_metadata_to_json(SecretMetadata const& metadata) -> ConversionResult<JsonValue>
{
    if (!detail::is_valid_secret_name(metadata.name)) {
        return reject<JsonValue>(false);
    }
    auto created = format_utc_timestamp(metadata.created_at);
    auto updated = format_utc_timestamp(metadata.updated_at);
    if (!created || !updated) {
        return reject<JsonValue>(false);
    }
    return checked_json(json(JsonValue::Object{
                            {"name",       json(metadata.name)             },
                            {"created_at", json(std::move(created).value())},
                            {"updated_at", json(std::move(updated).value())},
    }),
                        false);
}

auto secret_metadata_from_json(JsonValue const& value) -> ConversionResult<SecretMetadata>
{
    if (!value.is_object()) {
        return reject<SecretMetadata>(false);
    }
    auto const* name    = member(value.as_object(), "name");
    auto const* created = member(value.as_object(), "created_at");
    auto const* updated = member(value.as_object(), "updated_at");
    if (!name || !name->is_string() || !detail::is_valid_secret_name(name->as_string()) || !created ||
        !created->is_string() || !updated || !updated->is_string()) {
        return reject<SecretMetadata>(false);
    }
    auto created_at = parse_utc_timestamp(created->as_string());
    auto updated_at = parse_utc_timestamp(updated->as_string());
    if (!created_at || !updated_at) {
        return reject<SecretMetadata>(false);
    }
    return ConversionResult<SecretMetadata>::success(
        {.name = name->as_string(), .created_at = *created_at, .updated_at = *updated_at});
}

auto secret_page_to_json(SecretPage const& page) -> ConversionResult<JsonValue>
{
    if (!valid_page(page)) {
        return reject<JsonValue>(false);
    }
    auto items = JsonValue::Array{};
    items.reserve(page.items.size());
    for (auto const& metadata : page.items) {
        auto encoded = secret_metadata_to_json(metadata);
        if (!encoded) {
            return reject<JsonValue>(false);
        }
        items.push_back(std::move(encoded).value());
    }
    auto next = json(jb::core::JsonNull{});
    if (page.next_after_name) {
        next = json(*page.next_after_name);
    }
    return checked_json(json(JsonValue::Object{
                            {"items",           json(std::move(items))},
                            {"next_after_name", std::move(next)       },
    }),
                        false);
}

auto secret_page_from_json(JsonValue const& value) -> ConversionResult<SecretPage>
{
    if (!value.is_object()) {
        return reject<SecretPage>(false);
    }
    auto const* items = member(value.as_object(), "items");
    auto const* next  = member(value.as_object(), "next_after_name");
    if (!items || !items->is_array() || !next || items->as_array().size() > 200U) {
        return reject<SecretPage>(false);
    }

    auto page = SecretPage{};
    page.items.reserve(items->as_array().size());
    for (auto const& item : items->as_array()) {
        auto metadata = secret_metadata_from_json(item);
        if (!metadata) {
            return reject<SecretPage>(false);
        }
        page.items.push_back(std::move(metadata).value());
    }
    if (next->is_string()) {
        page.next_after_name = next->as_string();
    }
    else if (!next->is_null()) {
        return reject<SecretPage>(false);
    }
    if (!valid_page(page)) {
        return reject<SecretPage>(false);
    }
    return ConversionResult<SecretPage>::success(std::move(page));
}

} // namespace jb::jobu
