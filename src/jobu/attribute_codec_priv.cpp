#include "attribute_codec_priv.hpp"

#include "attribute_registry.hpp"

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace jb::jobu::detail {

namespace {

template <typename T>
using Result = jb::core::Result<T, jb::core::Error>;

constexpr std::uint64_t kAttributeDocumentVersion{1};
constexpr std::size_t   kMaxAttributeDepth{64};

auto make_json(auto value) -> jb::rpc::JsonValue
{
    jb::rpc::JsonValue result;
    result.data = std::move(value);
    return result;
}

auto attribute_input_error(std::string message) -> jb::core::Error
{
    return {
        .category = jb::core::ErrorCategory::InvalidArgument,
        .code     = "jobu.attribute.invalid_value",
        .message  = std::move(message),
    };
}

auto invalid_document(std::string_view reason) -> jb::core::Error
{
    return {
        .category = jb::core::ErrorCategory::Internal,
        .code     = "jobu.attribute.invalid_document",
        .message  = "Persisted JobU attribute document is invalid",
        .detail   = reason.empty() ? std::string{} : "reason=" + std::string{reason},
    };
}

template <typename T>
auto document_failure(std::string_view reason) -> Result<T>
{
    return Result<T>::failure(invalid_document(reason));
}

auto find_member(jb::rpc::JsonValue::Object const& object, std::string_view name) -> jb::rpc::JsonValue const*
{
    auto const iterator = object.find(name);
    return iterator == object.end() ? nullptr : &iterator->second;
}

auto signed_integer(jb::rpc::JsonValue const& value) -> Result<std::int64_t>
{
    if (value.is_int()) {
        return Result<std::int64_t>::success(value.as_int());
    }
    if (value.is_uint() && value.as_uint() <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return Result<std::int64_t>::success(static_cast<std::int64_t>(value.as_uint()));
    }
    return document_failure<std::int64_t>("integer_out_of_range");
}

auto duration_to_nanoseconds(jb::core::Duration value) -> Result<std::int64_t>
{
    using Nanoseconds    = std::chrono::nanoseconds;
    auto const converted = std::chrono::duration_cast<Nanoseconds>(value);
    if (std::chrono::duration_cast<jb::core::Duration>(converted) != value) {
        return Result<std::int64_t>::failure(
            attribute_input_error("JobU duration must be exactly representable in signed nanoseconds"));
    }
    return Result<std::int64_t>::success(converted.count());
}

auto nanoseconds_to_duration(jb::rpc::JsonValue const& value) -> Result<jb::core::Duration>
{
    auto count = signed_integer(value);
    if (!count) {
        return Result<jb::core::Duration>::failure(std::move(count).error());
    }

    using Nanoseconds  = std::chrono::nanoseconds;
    auto const minimum = std::chrono::ceil<Nanoseconds>(jb::core::Duration::min()).count();
    auto const maximum = std::chrono::floor<Nanoseconds>(jb::core::Duration::max()).count();
    if (*count < minimum || *count > maximum) {
        return document_failure<jb::core::Duration>("duration_out_of_range");
    }
    auto const converted = std::chrono::duration_cast<jb::core::Duration>(Nanoseconds{*count});
    if (std::chrono::duration_cast<Nanoseconds>(converted).count() != *count) {
        return document_failure<jb::core::Duration>("duration_out_of_range");
    }
    return Result<jb::core::Duration>::success(converted);
}

auto bytes_to_hex(jb::core::ByteBuffer const& bytes) -> std::string
{
    constexpr std::string_view digits{"0123456789abcdef"};
    auto                       result = std::string{};
    result.reserve(bytes.size() * 2U);
    for (auto const byte : bytes) {
        auto const value = std::to_integer<unsigned int>(byte);
        result.push_back(digits[value >> 4U]);
        result.push_back(digits[value & 0x0fU]);
    }
    return result;
}

auto hex_value(char character) noexcept -> int
{
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    return -1;
}

auto hex_to_bytes(jb::rpc::JsonValue const& value) -> Result<jb::core::ByteBuffer>
{
    if (!value.is_string() || value.as_string().size() % 2U != 0U) {
        return document_failure<jb::core::ByteBuffer>("invalid_hex");
    }
    auto const& text   = value.as_string();
    auto        result = jb::core::ByteBuffer{};
    result.reserve(text.size() / 2U);
    for (std::size_t index = 0; index < text.size(); index += 2U) {
        auto const high = hex_value(text[index]);
        auto const low  = hex_value(text[index + 1U]);
        if (high < 0 || low < 0) {
            return document_failure<jb::core::ByteBuffer>("invalid_hex");
        }
        result.push_back(static_cast<std::byte>((high << 4) | low));
    }
    return Result<jb::core::ByteBuffer>::success(std::move(result));
}

auto typed_value_to_json(AttributeValue const& value, std::size_t depth) -> Result<jb::rpc::JsonValue>
{
    auto tag           = std::string{};
    auto encoded_value = jb::rpc::JsonValue{};

    if (auto const* boolean = std::get_if<bool>(&value.data)) {
        tag           = "boolean";
        encoded_value = make_json(*boolean);
    }
    else if (auto const* integer = std::get_if<std::int64_t>(&value.data)) {
        tag           = "integer";
        encoded_value = make_json(*integer);
    }
    else if (auto const* number = std::get_if<double>(&value.data)) {
        if (!std::isfinite(*number)) {
            return Result<jb::rpc::JsonValue>::failure(attribute_input_error("JobU number attribute must be finite"));
        }
        tag           = "number";
        encoded_value = make_json(*number);
    }
    else if (auto const* text = std::get_if<std::string>(&value.data)) {
        tag           = "string";
        encoded_value = make_json(*text);
    }
    else if (auto const* duration = std::get_if<jb::core::Duration>(&value.data)) {
        auto nanoseconds = duration_to_nanoseconds(*duration);
        if (!nanoseconds) {
            return Result<jb::rpc::JsonValue>::failure(std::move(nanoseconds).error());
        }
        tag           = "duration_ns";
        encoded_value = make_json(*nanoseconds);
    }
    else if (auto const* bytes = std::get_if<jb::core::ByteBuffer>(&value.data)) {
        tag           = "bytes_hex";
        encoded_value = make_json(bytes_to_hex(*bytes));
    }
    else {
        if (depth >= kMaxAttributeDepth) {
            return Result<jb::rpc::JsonValue>::failure(attribute_input_error("JobU attribute nesting is too deep"));
        }
        if (auto const* list = std::get_if<AttributeValue::List>(&value.data)) {
            auto array = jb::rpc::JsonValue::Array{};
            array.reserve(list->size());
            for (auto const& entry : *list) {
                auto encoded = typed_value_to_json(entry, depth + 1U);
                if (!encoded) {
                    return Result<jb::rpc::JsonValue>::failure(std::move(encoded).error());
                }
                array.push_back(std::move(encoded).value());
            }
            tag           = "list";
            encoded_value = make_json(std::move(array));
        }
        else {
            auto object = jb::rpc::JsonValue::Object{};
            for (auto const& [name, entry] : std::get<AttributeValue::Map>(value.data)) {
                auto encoded = typed_value_to_json(entry, depth + 1U);
                if (!encoded) {
                    return Result<jb::rpc::JsonValue>::failure(std::move(encoded).error());
                }
                object.emplace(name, std::move(encoded).value());
            }
            tag           = "map";
            encoded_value = make_json(std::move(object));
        }
    }

    return Result<jb::rpc::JsonValue>::success(make_json(jb::rpc::JsonValue::Object{
        {"type",  make_json(std::move(tag))},
        {"value", std::move(encoded_value) },
    }));
}

auto typed_value_from_json(jb::rpc::JsonValue const& value, std::size_t depth) -> Result<AttributeValue>
{
    if (!value.is_object() || value.as_object().size() != 2U) {
        return document_failure<AttributeValue>("typed_value_shape");
    }
    auto const* type    = find_member(value.as_object(), "type");
    auto const* payload = find_member(value.as_object(), "value");
    if (type == nullptr || !type->is_string() || payload == nullptr) {
        return document_failure<AttributeValue>("typed_value_shape");
    }

    auto const& tag = type->as_string();
    if (tag == "boolean") {
        if (payload->is_bool()) {
            return Result<AttributeValue>::success({.data = payload->as_bool()});
        }
        return document_failure<AttributeValue>("boolean_shape");
    }
    if (tag == "integer") {
        auto integer = signed_integer(*payload);
        if (!integer) {
            return Result<AttributeValue>::failure(std::move(integer).error());
        }
        return Result<AttributeValue>::success({.data = *integer});
    }
    if (tag == "number") {
        if (!payload->is_double() || !std::isfinite(payload->as_double())) {
            return document_failure<AttributeValue>("number_shape");
        }
        return Result<AttributeValue>::success({.data = payload->as_double()});
    }
    if (tag == "string") {
        if (!payload->is_string()) {
            return document_failure<AttributeValue>("string_shape");
        }
        return Result<AttributeValue>::success({.data = payload->as_string()});
    }
    if (tag == "duration_ns") {
        auto duration = nanoseconds_to_duration(*payload);
        if (!duration) {
            return Result<AttributeValue>::failure(std::move(duration).error());
        }
        return Result<AttributeValue>::success({.data = *duration});
    }
    if (tag == "bytes_hex") {
        auto bytes = hex_to_bytes(*payload);
        if (!bytes) {
            return Result<AttributeValue>::failure(std::move(bytes).error());
        }
        return Result<AttributeValue>::success({.data = std::move(bytes).value()});
    }
    if (tag != "list" && tag != "map") {
        return document_failure<AttributeValue>("unknown_type_tag");
    }
    if (depth >= kMaxAttributeDepth) {
        return document_failure<AttributeValue>("nesting_too_deep");
    }
    if (tag == "list") {
        if (!payload->is_array()) {
            return document_failure<AttributeValue>("list_shape");
        }
        auto result = AttributeValue::List{};
        result.reserve(payload->as_array().size());
        for (auto const& entry : payload->as_array()) {
            auto decoded = typed_value_from_json(entry, depth + 1U);
            if (!decoded) {
                return Result<AttributeValue>::failure(std::move(decoded).error());
            }
            result.push_back(std::move(decoded).value());
        }
        return Result<AttributeValue>::success({.data = std::move(result)});
    }

    if (!payload->is_object()) {
        return document_failure<AttributeValue>("map_shape");
    }
    auto result = AttributeValue::Map{};
    for (auto const& [name, entry] : payload->as_object()) {
        auto decoded = typed_value_from_json(entry, depth + 1U);
        if (!decoded) {
            return Result<AttributeValue>::failure(std::move(decoded).error());
        }
        result.emplace(name, std::move(decoded).value());
    }
    return Result<AttributeValue>::success({.data = std::move(result)});
}

auto validate_encoding(AttributeRegistry const& registry,
                       AttributeSet const&      values,
                       AttributeScope           scope,
                       AttributeDocumentMode    mode) -> Result<void>
{
    if (mode == AttributeDocumentMode::Materialized && scope != AttributeScope::Job) {
        return Result<void>::failure(attribute_input_error("Materialized JobU attributes require Job scope"));
    }
    for (auto const& [name, value] : values) {
        auto validated = registry.validate(name, value, scope);
        if (!validated) {
            return validated;
        }
    }
    if (mode == AttributeDocumentMode::Partial) {
        return Result<void>::success();
    }
    for (auto const& definition : registry.definitions()) {
        if (definition.scopes.test(AttributeScope::Job) && !values.contains(definition.name)) {
            return Result<void>::failure(attribute_input_error("Materialized JobU attributes are incomplete"));
        }
    }
    auto materialized = materialize_attributes(registry, {}, {}, values);
    if (!materialized) {
        return Result<void>::failure(std::move(materialized).error());
    }
    return Result<void>::success();
}

} // anonymous namespace

auto encode_attribute_document(AttributeRegistry const& registry,
                               AttributeSet const&      values,
                               AttributeScope           scope,
                               AttributeDocumentMode    mode) -> jb::core::Result<jb::rpc::JsonValue, jb::core::Error>
{
    auto validated = validate_encoding(registry, values, scope, mode);
    if (!validated) {
        return Result<jb::rpc::JsonValue>::failure(std::move(validated).error());
    }

    auto encoded_values = jb::rpc::JsonValue::Object{};
    for (auto const& [name, value] : values) {
        auto encoded = typed_value_to_json(value, 0);
        if (!encoded) {
            return Result<jb::rpc::JsonValue>::failure(std::move(encoded).error());
        }
        encoded_values.emplace(name, std::move(encoded).value());
    }
    auto result     = make_json(jb::rpc::JsonValue::Object{
        {"values",  make_json(std::move(encoded_values))},
        {"version", make_json(kAttributeDocumentVersion)},
    });
    auto serialized = jb::rpc::serialize_json(result);
    if (!serialized) {
        return Result<jb::rpc::JsonValue>::failure(attribute_input_error("JobU attribute text is not valid UTF-8"));
    }
    return Result<jb::rpc::JsonValue>::success(std::move(result));
}

auto decode_attribute_document(AttributeRegistry const&  registry,
                               jb::rpc::JsonValue const& document,
                               AttributeScope            scope,
                               AttributeDocumentMode     mode) -> jb::core::Result<AttributeSet, jb::core::Error>
{
    if (!document.is_object() || document.as_object().size() != 2U) {
        return document_failure<AttributeSet>("document_shape");
    }
    auto const* version = find_member(document.as_object(), "version");
    auto const* values  = find_member(document.as_object(), "values");
    if (version == nullptr || values == nullptr || !values->is_object() ||
        ((!version->is_uint() || version->as_uint() != kAttributeDocumentVersion) &&
         (!version->is_int() || version->as_int() != static_cast<std::int64_t>(kAttributeDocumentVersion)))) {
        return document_failure<AttributeSet>("document_shape");
    }
    if (mode == AttributeDocumentMode::Materialized && scope != AttributeScope::Job) {
        return document_failure<AttributeSet>("materialized_scope");
    }

    auto result = AttributeSet{};
    for (auto const& [name, encoded] : values->as_object()) {
        auto const* definition = registry.find(name);
        if (definition == nullptr || !definition->scopes.test(scope)) {
            return document_failure<AttributeSet>(definition == nullptr ? "unknown_attribute" : "invalid_scope");
        }
        auto decoded = typed_value_from_json(encoded, 0);
        if (!decoded) {
            return document_failure<AttributeSet>(decoded.error().code);
        }
        auto validated = registry.validate(name, *decoded, scope);
        if (!validated) {
            return document_failure<AttributeSet>(validated.error().code);
        }
        result.emplace(name, std::move(decoded).value());
    }

    auto serialized = jb::rpc::serialize_json(document);
    if (!serialized) {
        return document_failure<AttributeSet>(serialized.error().code);
    }
    if (mode == AttributeDocumentMode::Partial) {
        return Result<AttributeSet>::success(std::move(result));
    }

    auto materialized = materialize_attributes(registry, {}, {}, result);
    if (!materialized) {
        return document_failure<AttributeSet>(materialized.error().code);
    }
    return materialized;
}

} // namespace jb::jobu::detail
