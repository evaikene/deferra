#include "attribute_registry.hpp"

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <string>
#include <utility>
#include <variant>

namespace jb::jobu {

namespace {

using AttributeResult = jb::core::Result<void, jb::core::Error>;
template <typename T>
using Result = jb::core::Result<T, jb::core::Error>;

constexpr std::size_t  kMaxAttributeDepth{64};
constexpr std::int64_t kDefaultOutputLimitBytes = std::int64_t{1024} * 1024;
constexpr std::int64_t kMaximumOutputLimitBytes = std::int64_t{64} * 1024 * 1024;

auto attribute_error(std::string code, std::string message) -> jb::core::Error
{
    return {
        .category = jb::core::ErrorCategory::InvalidArgument,
        .code     = std::move(code),
        .message  = std::move(message),
    };
}

auto invalid_value(std::string message = "JobU attribute value is invalid") -> jb::core::Error
{
    return attribute_error("jobu.attribute.invalid_value", std::move(message));
}

auto make_json(auto value) -> jb::core::JsonValue
{
    jb::core::JsonValue result;
    result.data = std::move(value);
    return result;
}

auto standard_scopes() -> AttributeScopes
{
    return {AttributeScope::DaemonDefault, AttributeScope::QueueDefault, AttributeScope::Job};
}

auto has_attribute_type(AttributeValue const& value, AttributeType type) -> bool
{
    switch (type) {
        case AttributeType::Boolean:
            return std::holds_alternative<bool>(value.data);
        case AttributeType::Integer:
            return std::holds_alternative<std::int64_t>(value.data);
        case AttributeType::Number:
            return std::holds_alternative<double>(value.data);
        case AttributeType::String:
            return std::holds_alternative<std::string>(value.data);
        case AttributeType::Duration:
            return std::holds_alternative<jb::core::Duration>(value.data);
        case AttributeType::Bytes:
            return std::holds_alternative<jb::core::ByteBuffer>(value.data);
        case AttributeType::List:
            return std::holds_alternative<AttributeValue::List>(value.data);
        case AttributeType::Map:
            return std::holds_alternative<AttributeValue::Map>(value.data);
    }
    return false;
}

auto validate_set(AttributeRegistry const& registry, AttributeSet const& values, AttributeScope scope)
    -> AttributeResult
{
    for (auto const& [name, value] : values) {
        auto validated = registry.validate(name, value, scope);
        if (!validated) {
            return validated;
        }
    }
    return AttributeResult::success();
}

auto validate_standard_cross_fields(AttributeRegistry const& registry, AttributeSet const& values) -> AttributeResult
{
    auto const* initial_definition = registry.find("retry.initial_delay");
    auto const* maximum_definition = registry.find("retry.max_delay");
    if (initial_definition == nullptr || maximum_definition == nullptr ||
        initial_definition->type != AttributeType::Duration || maximum_definition->type != AttributeType::Duration) {
        return AttributeResult::success();
    }

    auto const initial = values.find("retry.initial_delay");
    auto const maximum = values.find("retry.max_delay");
    if (initial == values.end() || maximum == values.end()) {
        return AttributeResult::success();
    }
    auto const* initial_duration = std::get_if<jb::core::Duration>(&initial->second.data);
    auto const* maximum_duration = std::get_if<jb::core::Duration>(&maximum->second.data);
    if (initial_duration == nullptr || maximum_duration == nullptr) {
        return AttributeResult::success();
    }
    if (*maximum_duration < *initial_duration) {
        return AttributeResult::failure(invalid_value("retry.max_delay must not be below retry.initial_delay"));
    }
    return AttributeResult::success();
}

auto validate_complete_set(AttributeRegistry const& registry, AttributeSet const& values) -> AttributeResult
{
    auto validated = validate_set(registry, values, AttributeScope::Job);
    if (!validated) {
        return validated;
    }
    for (auto const& definition : registry.definitions()) {
        if (definition.scopes.test(AttributeScope::Job) && !values.contains(definition.name)) {
            return AttributeResult::failure(invalid_value("Materialized JobU attributes are incomplete"));
        }
    }
    return validate_standard_cross_fields(registry, values);
}

auto is_one_of(std::string const& value, std::initializer_list<std::string_view> choices) -> bool
{
    for (auto const choice : choices) {
        if (value == choice) {
            return true;
        }
    }
    return false;
}

auto duration_to_milliseconds(jb::core::Duration value) -> Result<std::int64_t>
{
    using Milliseconds   = std::chrono::milliseconds;
    auto const converted = std::chrono::duration_cast<Milliseconds>(value);
    if (std::chrono::duration_cast<jb::core::Duration>(converted) != value) {
        return Result<std::int64_t>::failure(
            invalid_value("JobU public duration must be exactly representable in milliseconds"));
    }
    return Result<std::int64_t>::success(converted.count());
}

auto json_signed_integer(jb::core::JsonValue const& value) -> Result<std::int64_t>
{
    if (value.is_int()) {
        return Result<std::int64_t>::success(value.as_int());
    }
    if (value.is_uint() && value.as_uint() <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return Result<std::int64_t>::success(static_cast<std::int64_t>(value.as_uint()));
    }
    return Result<std::int64_t>::failure(invalid_value("JobU attribute requires a signed 64-bit integer"));
}

auto milliseconds_to_duration(jb::core::JsonValue const& value) -> Result<jb::core::Duration>
{
    auto count = json_signed_integer(value);
    if (!count) {
        return Result<jb::core::Duration>::failure(std::move(count).error());
    }

    using Milliseconds = std::chrono::milliseconds;
    auto const minimum = std::chrono::ceil<Milliseconds>(jb::core::Duration::min()).count();
    auto const maximum = std::chrono::floor<Milliseconds>(jb::core::Duration::max()).count();
    if (*count < minimum || *count > maximum) {
        return Result<jb::core::Duration>::failure(invalid_value("JobU public duration is out of range"));
    }
    auto const converted = std::chrono::duration_cast<jb::core::Duration>(Milliseconds{*count});
    if (std::chrono::duration_cast<Milliseconds>(converted).count() != *count) {
        return Result<jb::core::Duration>::failure(invalid_value("JobU public duration is out of range"));
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

auto hex_to_bytes(std::string const& text) -> Result<jb::core::ByteBuffer>
{
    if (text.size() % 2U != 0U) {
        return Result<jb::core::ByteBuffer>::failure(invalid_value("JobU byte attribute requires even-length hex"));
    }
    auto result = jb::core::ByteBuffer{};
    result.reserve(text.size() / 2U);
    for (std::size_t index = 0; index < text.size(); index += 2U) {
        auto const high = hex_value(text[index]);
        auto const low  = hex_value(text[index + 1U]);
        if (high < 0 || low < 0) {
            return Result<jb::core::ByteBuffer>::failure(
                invalid_value("JobU byte attribute requires lower-case hexadecimal"));
        }
        result.push_back(static_cast<std::byte>((high << 4) | low));
    }
    return Result<jb::core::ByteBuffer>::success(std::move(result));
}

auto natural_value_to_json(AttributeValue const& value, std::size_t depth) -> Result<jb::core::JsonValue>
{
    if (auto const* boolean = std::get_if<bool>(&value.data)) {
        return Result<jb::core::JsonValue>::success(make_json(*boolean));
    }
    if (auto const* integer = std::get_if<std::int64_t>(&value.data)) {
        return Result<jb::core::JsonValue>::success(make_json(*integer));
    }
    if (auto const* number = std::get_if<double>(&value.data)) {
        if (!std::isfinite(*number)) {
            return Result<jb::core::JsonValue>::failure(invalid_value("JobU number attribute must be finite"));
        }
        return Result<jb::core::JsonValue>::success(make_json(*number));
    }
    if (auto const* text = std::get_if<std::string>(&value.data)) {
        return Result<jb::core::JsonValue>::success(make_json(*text));
    }
    if (std::holds_alternative<jb::core::Duration>(value.data) ||
        std::holds_alternative<jb::core::ByteBuffer>(value.data)) {
        return Result<jb::core::JsonValue>::failure(
            invalid_value("Nested JobU durations and bytes have no public JSON element schema"));
    }
    if (depth >= kMaxAttributeDepth) {
        return Result<jb::core::JsonValue>::failure(invalid_value("JobU attribute nesting is too deep"));
    }
    if (auto const* list = std::get_if<AttributeValue::List>(&value.data)) {
        auto result = jb::core::JsonValue::Array{};
        result.reserve(list->size());
        for (auto const& entry : *list) {
            auto encoded = natural_value_to_json(entry, depth + 1U);
            if (!encoded) {
                return Result<jb::core::JsonValue>::failure(std::move(encoded).error());
            }
            result.push_back(std::move(encoded).value());
        }
        return Result<jb::core::JsonValue>::success(make_json(std::move(result)));
    }

    auto result = jb::core::JsonValue::Object{};
    for (auto const& [name, entry] : std::get<AttributeValue::Map>(value.data)) {
        auto encoded = natural_value_to_json(entry, depth + 1U);
        if (!encoded) {
            return Result<jb::core::JsonValue>::failure(std::move(encoded).error());
        }
        result.emplace(name, std::move(encoded).value());
    }
    return Result<jb::core::JsonValue>::success(make_json(std::move(result)));
}

auto natural_value_from_json(jb::core::JsonValue const& value, std::size_t depth) -> Result<AttributeValue>
{
    if (value.is_bool()) {
        return Result<AttributeValue>::success({.data = value.as_bool()});
    }
    if (value.is_int()) {
        return Result<AttributeValue>::success({.data = value.as_int()});
    }
    if (value.is_uint()) {
        if (value.as_uint() > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            return Result<AttributeValue>::failure(invalid_value("JobU nested integer is out of range"));
        }
        return Result<AttributeValue>::success({.data = static_cast<std::int64_t>(value.as_uint())});
    }
    if (value.is_double()) {
        if (!std::isfinite(value.as_double())) {
            return Result<AttributeValue>::failure(invalid_value("JobU nested number must be finite"));
        }
        return Result<AttributeValue>::success({.data = value.as_double()});
    }
    if (value.is_string()) {
        return Result<AttributeValue>::success({.data = value.as_string()});
    }
    if (!value.is_array() && !value.is_object()) {
        return Result<AttributeValue>::failure(invalid_value("JSON null is not a JobU attribute value"));
    }
    if (depth >= kMaxAttributeDepth) {
        return Result<AttributeValue>::failure(invalid_value("JobU attribute nesting is too deep"));
    }
    if (value.is_array()) {
        auto result = AttributeValue::List{};
        result.reserve(value.as_array().size());
        for (auto const& entry : value.as_array()) {
            auto decoded = natural_value_from_json(entry, depth + 1U);
            if (!decoded) {
                return Result<AttributeValue>::failure(std::move(decoded).error());
            }
            result.push_back(std::move(decoded).value());
        }
        return Result<AttributeValue>::success({.data = std::move(result)});
    }

    auto result = AttributeValue::Map{};
    for (auto const& [name, entry] : value.as_object()) {
        auto decoded = natural_value_from_json(entry, depth + 1U);
        if (!decoded) {
            return Result<AttributeValue>::failure(std::move(decoded).error());
        }
        result.emplace(name, std::move(decoded).value());
    }
    return Result<AttributeValue>::success({.data = std::move(result)});
}

auto definition_value_to_json(AttributeDefinition const& definition, AttributeValue const& value)
    -> Result<jb::core::JsonValue>
{
    if (definition.type == AttributeType::Duration) {
        auto milliseconds = duration_to_milliseconds(std::get<jb::core::Duration>(value.data));
        if (!milliseconds) {
            return Result<jb::core::JsonValue>::failure(std::move(milliseconds).error());
        }
        return Result<jb::core::JsonValue>::success(make_json(*milliseconds));
    }
    if (definition.type == AttributeType::Bytes) {
        return Result<jb::core::JsonValue>::success(
            make_json(bytes_to_hex(std::get<jb::core::ByteBuffer>(value.data))));
    }
    return natural_value_to_json(value, 0);
}

auto definition_value_from_json(AttributeDefinition const& definition, jb::core::JsonValue const& value)
    -> Result<AttributeValue>
{
    switch (definition.type) {
        case AttributeType::Boolean:
            if (value.is_bool()) {
                return Result<AttributeValue>::success({.data = value.as_bool()});
            }
            break;
        case AttributeType::Integer: {
            auto integer = json_signed_integer(value);
            if (integer) {
                return Result<AttributeValue>::success({.data = *integer});
            }
            return Result<AttributeValue>::failure(std::move(integer).error());
        }
        case AttributeType::Number:
            if (value.is_double()) {
                if (!std::isfinite(value.as_double())) {
                    return Result<AttributeValue>::failure(invalid_value("JobU number attribute must be finite"));
                }
                return Result<AttributeValue>::success({.data = value.as_double()});
            }
            if (value.is_int()) {
                return Result<AttributeValue>::success({.data = static_cast<double>(value.as_int())});
            }
            if (value.is_uint()) {
                return Result<AttributeValue>::success({.data = static_cast<double>(value.as_uint())});
            }
            break;
        case AttributeType::String:
            if (value.is_string()) {
                return Result<AttributeValue>::success({.data = value.as_string()});
            }
            break;
        case AttributeType::Duration: {
            auto duration = milliseconds_to_duration(value);
            if (duration) {
                return Result<AttributeValue>::success({.data = *duration});
            }
            return Result<AttributeValue>::failure(std::move(duration).error());
        }
        case AttributeType::Bytes:
            if (value.is_string()) {
                auto bytes = hex_to_bytes(value.as_string());
                if (bytes) {
                    return Result<AttributeValue>::success({.data = std::move(bytes).value()});
                }
                return Result<AttributeValue>::failure(std::move(bytes).error());
            }
            break;
        case AttributeType::List:
            if (value.is_array()) {
                return natural_value_from_json(value, 0);
            }
            break;
        case AttributeType::Map:
            if (value.is_object()) {
                return natural_value_from_json(value, 0);
            }
            break;
    }
    return Result<AttributeValue>::failure(
        attribute_error("jobu.attribute.invalid_type", "JobU attribute JSON type does not match its definition"));
}

} // anonymous namespace

StandardAttributeRegistry::StandardAttributeRegistry()
    : _definitions{
          {
           {
                  .name             = "job.timeout",
                  .type             = AttributeType::Duration,
                  .scopes           = standard_scopes(),
                  .built_in_default = {.data =
                                           std::chrono::duration_cast<jb::core::Duration>(std::chrono::seconds{120})},
                  .description      = "Maximum duration of one job execution",
              }, {
                  .name             = "output.capture",
                  .type             = AttributeType::String,
                  .scopes           = standard_scopes(),
                  .built_in_default = {.data = std::string{"on_error"}},
                  .description      = "Runner output capture policy",
              }, {
                  .name             = "output.stderr_limit",
                  .type             = AttributeType::Integer,
                  .scopes           = standard_scopes(),
                  .built_in_default = {.data = kDefaultOutputLimitBytes},
                  .description      = "Maximum captured standard error bytes",
              }, {
                  .name             = "output.stdout_limit",
                  .type             = AttributeType::Integer,
                  .scopes           = standard_scopes(),
                  .built_in_default = {.data = kDefaultOutputLimitBytes},
                  .description      = "Maximum captured standard output bytes",
              }, {
                  .name             = "retry.initial_delay",
                  .type             = AttributeType::Duration,
                  .scopes           = standard_scopes(),
                  .built_in_default = {.data = jb::core::Duration::zero()},
                  .description      = "Delay before the first retry",
              }, {
                  .name             = "retry.jitter",
                  .type             = AttributeType::Number,
                  .scopes           = standard_scopes(),
                  .built_in_default = {.data = 0.0},
                  .description      = "Symmetric retry delay jitter fraction",
              }, {
                  .name             = "retry.max_attempts",
                  .type             = AttributeType::Integer,
                  .scopes           = standard_scopes(),
                  .built_in_default = {.data = std::int64_t{1}},
                  .description      = "Maximum number of execution attempts",
              }, {
                  .name             = "retry.max_delay",
                  .type             = AttributeType::Duration,
                  .scopes           = standard_scopes(),
                  .built_in_default = {.data = std::chrono::duration_cast<jb::core::Duration>(std::chrono::hours{24})},
                  .description      = "Maximum delay between retries",
              }, {
                  .name             = "retry.mode",
                  .type             = AttributeType::String,
                  .scopes           = standard_scopes(),
                  .built_in_default = {.data = std::string{"reschedule"}},
                  .description      = "Whether retry delays block or reschedule queue capacity",
              }, {
                  .name             = "retry.multiplier",
                  .type             = AttributeType::Number,
                  .scopes           = standard_scopes(),
                  .built_in_default = {.data = 2.0},
                  .description      = "Exponential retry delay multiplier",
              }, {
                  .name             = "retry.strategy",
                  .type             = AttributeType::String,
                  .scopes           = standard_scopes(),
                  .built_in_default = {.data = std::string{"fixed"}},
                  .description      = "Retry delay calculation strategy",
              }, }
}
{}

auto StandardAttributeRegistry::find(std::string_view name) const noexcept -> AttributeDefinition const*
{
    for (auto const& definition : _definitions) {
        if (definition.name == name) {
            return &definition;
        }
    }
    return nullptr;
}

auto StandardAttributeRegistry::validate(std::string_view name, AttributeValue const& value, AttributeScope scope) const
    -> jb::core::Result<void, jb::core::Error>
{
    auto const* definition = find(name);
    if (definition == nullptr) {
        return AttributeResult::failure(attribute_error("jobu.attribute.unknown", "JobU attribute is not registered"));
    }
    if (!definition->scopes.test(scope)) {
        return AttributeResult::failure(
            attribute_error("jobu.attribute.invalid_scope", "JobU attribute is not accepted at this scope"));
    }
    if (!has_attribute_type(value, definition->type)) {
        return AttributeResult::failure(
            attribute_error("jobu.attribute.invalid_type", "JobU attribute type does not match its definition"));
    }

    using namespace std::chrono;
    if (name == "job.timeout") {
        auto const duration = std::get<jb::core::Duration>(value.data);
        if (duration < duration_cast<jb::core::Duration>(milliseconds{1}) ||
            duration > duration_cast<jb::core::Duration>(days{30})) {
            return AttributeResult::failure(invalid_value());
        }
    }
    else if (name == "retry.max_attempts") {
        auto const attempts = std::get<std::int64_t>(value.data);
        if (attempts < 1 || attempts > 100) {
            return AttributeResult::failure(invalid_value());
        }
    }
    else if (name == "retry.strategy") {
        if (!is_one_of(std::get<std::string>(value.data), {"fixed", "exponential"})) {
            return AttributeResult::failure(invalid_value());
        }
    }
    else if (name == "retry.initial_delay") {
        auto const duration = std::get<jb::core::Duration>(value.data);
        if (duration < jb::core::Duration::zero() || duration > duration_cast<jb::core::Duration>(hours{24})) {
            return AttributeResult::failure(invalid_value());
        }
    }
    else if (name == "retry.jitter") {
        auto const jitter = std::get<double>(value.data);
        if (!std::isfinite(jitter) || jitter < 0.0 || jitter > 1.0) {
            return AttributeResult::failure(invalid_value());
        }
    }
    else if (name == "retry.max_delay") {
        auto const duration = std::get<jb::core::Duration>(value.data);
        if (duration < jb::core::Duration::zero() || duration > duration_cast<jb::core::Duration>(days{30})) {
            return AttributeResult::failure(invalid_value());
        }
    }
    else if (name == "retry.mode") {
        if (!is_one_of(std::get<std::string>(value.data), {"blocking", "reschedule"})) {
            return AttributeResult::failure(invalid_value());
        }
    }
    else if (name == "retry.multiplier") {
        auto const multiplier = std::get<double>(value.data);
        if (!std::isfinite(multiplier) || multiplier < 1.0 || multiplier > 100.0) {
            return AttributeResult::failure(invalid_value());
        }
    }
    else if (name == "output.capture") {
        if (!is_one_of(std::get<std::string>(value.data), {"none", "on_error", "always"})) {
            return AttributeResult::failure(invalid_value());
        }
    }
    else if (name == "output.stdout_limit" || name == "output.stderr_limit") {
        auto const limit = std::get<std::int64_t>(value.data);
        if (limit < 0 || limit > kMaximumOutputLimitBytes) {
            return AttributeResult::failure(invalid_value());
        }
    }
    return AttributeResult::success();
}

auto StandardAttributeRegistry::definitions() const -> std::span<AttributeDefinition const>
{
    return _definitions;
}

auto StandardAttributeRegistry::validate_materialized(AttributeSet const& values) const
    -> jb::core::Result<void, jb::core::Error>
{
    return validate_complete_set(*this, values);
}

auto materialize_attributes(AttributeRegistry const& registry,
                            AttributeSet const&      daemon_defaults,
                            AttributeSet const&      queue_defaults,
                            AttributeSet const&      job_values) -> jb::core::Result<AttributeSet, jb::core::Error>
{
    for (auto const& [values, scope] : {
             std::pair{&daemon_defaults, AttributeScope::DaemonDefault},
             std::pair{&queue_defaults,  AttributeScope::QueueDefault },
             std::pair{&job_values,      AttributeScope::Job          },
    }) {
        auto validated = validate_set(registry, *values, scope);
        if (!validated) {
            return Result<AttributeSet>::failure(std::move(validated).error());
        }
    }

    auto result = AttributeSet{};
    for (auto const& definition : registry.definitions()) {
        if (!definition.scopes.test(AttributeScope::Job)) {
            continue;
        }
        auto validated = registry.validate(definition.name, definition.built_in_default, AttributeScope::Job);
        if (!validated) {
            return Result<AttributeSet>::failure(std::move(validated).error());
        }
        result.emplace(definition.name, definition.built_in_default);
    }
    for (auto const* layer : {&daemon_defaults, &queue_defaults, &job_values}) {
        for (auto const& [name, value] : *layer) {
            result.insert_or_assign(name, value);
        }
    }

    auto validated = validate_complete_set(registry, result);
    if (!validated) {
        return Result<AttributeSet>::failure(std::move(validated).error());
    }
    return Result<AttributeSet>::success(std::move(result));
}

auto attribute_set_to_json(AttributeSet const& values, AttributeRegistry const& registry, AttributeScope scope)
    -> jb::core::Result<jb::core::JsonValue, jb::core::Error>
{
    auto object = jb::core::JsonValue::Object{};
    for (auto const& [name, value] : values) {
        auto const* definition = registry.find(name);
        if (definition == nullptr) {
            return Result<jb::core::JsonValue>::failure(
                attribute_error("jobu.attribute.unknown", "JobU attribute is not registered"));
        }
        auto validated = registry.validate(name, value, scope);
        if (!validated) {
            return Result<jb::core::JsonValue>::failure(std::move(validated).error());
        }
        auto encoded = definition_value_to_json(*definition, value);
        if (!encoded) {
            return Result<jb::core::JsonValue>::failure(std::move(encoded).error());
        }
        object.emplace(name, std::move(encoded).value());
    }
    auto cross_fields = validate_standard_cross_fields(registry, values);
    if (!cross_fields) {
        return Result<jb::core::JsonValue>::failure(std::move(cross_fields).error());
    }

    auto result     = make_json(std::move(object));
    auto serialized = jb::core::serialize_json(result);
    if (!serialized) {
        return Result<jb::core::JsonValue>::failure(invalid_value("JobU attribute text is not valid UTF-8"));
    }
    return Result<jb::core::JsonValue>::success(std::move(result));
}

auto attribute_set_from_json(jb::core::JsonValue const& value, AttributeRegistry const& registry, AttributeScope scope)
    -> jb::core::Result<AttributeSet, jb::core::Error>
{
    if (!value.is_object()) {
        return Result<AttributeSet>::failure(invalid_value("JobU attributes must be a JSON object"));
    }

    auto result = AttributeSet{};
    for (auto const& [name, json_value] : value.as_object()) {
        auto const* definition = registry.find(name);
        if (definition == nullptr) {
            return Result<AttributeSet>::failure(
                attribute_error("jobu.attribute.unknown", "JobU attribute is not registered"));
        }
        if (!definition->scopes.test(scope)) {
            return Result<AttributeSet>::failure(
                attribute_error("jobu.attribute.invalid_scope", "JobU attribute is not accepted at this scope"));
        }
        auto decoded = definition_value_from_json(*definition, json_value);
        if (!decoded) {
            return Result<AttributeSet>::failure(std::move(decoded).error());
        }
        auto validated = registry.validate(name, *decoded, scope);
        if (!validated) {
            return Result<AttributeSet>::failure(std::move(validated).error());
        }
        result.emplace(name, std::move(decoded).value());
    }
    auto cross_fields = validate_standard_cross_fields(registry, result);
    if (!cross_fields) {
        return Result<AttributeSet>::failure(std::move(cross_fields).error());
    }

    auto encoded = attribute_set_to_json(result, registry, scope);
    if (!encoded) {
        return Result<AttributeSet>::failure(std::move(encoded).error());
    }
    return Result<AttributeSet>::success(std::move(result));
}

} // namespace jb::jobu
