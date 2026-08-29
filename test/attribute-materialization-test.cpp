#include "attribute_codec_priv.hpp"
#include "attribute_registry.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

using namespace jb::core;
using namespace jb::jobu;
using namespace jb::jobu::detail;
using namespace std::chrono_literals;

static_assert(std::is_move_constructible_v<SerializedAttributeDocument>);
static_assert(std::is_move_assignable_v<SerializedAttributeDocument>);
static_assert(!std::is_copy_constructible_v<SerializedAttributeDocument>);
static_assert(!std::is_copy_assignable_v<SerializedAttributeDocument>);

namespace {

auto make_json(auto value) -> JsonValue
{
    JsonValue result;
    result.data = std::move(value);
    return result;
}

auto same_value(AttributeValue const& left, AttributeValue const& right) -> bool;

auto same_list(AttributeValue::List const& left, AttributeValue::List const& right) -> bool
{
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (!same_value(left[index], right[index])) {
            return false;
        }
    }
    return true;
}

auto same_map(AttributeValue::Map const& left, AttributeValue::Map const& right) -> bool
{
    if (left.size() != right.size()) {
        return false;
    }
    auto right_entry = right.begin();
    for (auto const& [name, value] : left) {
        if (name != right_entry->first || !same_value(value, right_entry->second)) {
            return false;
        }
        ++right_entry;
    }
    return true;
}

auto same_value(AttributeValue const& left, AttributeValue const& right) -> bool
{
    if (left.data.index() != right.data.index()) {
        return false;
    }
    switch (left.data.index()) {
        case 0:
            return std::get<bool>(left.data) == std::get<bool>(right.data);
        case 1:
            return std::get<std::int64_t>(left.data) == std::get<std::int64_t>(right.data);
        case 2:
            return std::get<double>(left.data) == std::get<double>(right.data);
        case 3:
            return std::get<std::string>(left.data) == std::get<std::string>(right.data);
        case 4:
            return std::get<Duration>(left.data) == std::get<Duration>(right.data);
        case 5:
            return std::get<ByteBuffer>(left.data) == std::get<ByteBuffer>(right.data);
        case 6:
            return same_list(std::get<AttributeValue::List>(left.data), std::get<AttributeValue::List>(right.data));
        case 7:
            return same_map(std::get<AttributeValue::Map>(left.data), std::get<AttributeValue::Map>(right.data));
        default:
            return false;
    }
}

auto same_set(AttributeSet const& left, AttributeSet const& right) -> bool
{
    if (left.size() != right.size()) {
        return false;
    }
    auto right_entry = right.begin();
    for (auto const& [name, value] : left) {
        if (name != right_entry->first || !same_value(value, right_entry->second)) {
            return false;
        }
        ++right_entry;
    }
    return true;
}

auto string_list(std::initializer_list<std::string_view> values) -> AttributeValue::List
{
    auto result = AttributeValue::List{};
    result.reserve(values.size());
    for (auto const value : values) {
        result.push_back({.data = std::string{value}});
    }
    return result;
}

auto test_error(std::string code) -> Result<void, Error>
{
    return Result<void, Error>::failure({
        .category = ErrorCategory::InvalidArgument,
        .code     = std::move(code),
        .message  = "Invalid test attribute",
    });
}

auto has_type(AttributeValue const& value, AttributeType type) -> bool
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
            return std::holds_alternative<Duration>(value.data);
        case AttributeType::Bytes:
            return std::holds_alternative<ByteBuffer>(value.data);
        case AttributeType::List:
            return std::holds_alternative<AttributeValue::List>(value.data);
        case AttributeType::Map:
            return std::holds_alternative<AttributeValue::Map>(value.data);
    }
    return false;
}

class CompleteTypeRegistry final : public AttributeRegistry {
public:
    [[nodiscard]] auto find(std::string_view name) const noexcept -> AttributeDefinition const* override
    {
        for (auto const& definition : _definitions) {
            if (definition.name == name) {
                return &definition;
            }
        }
        return nullptr;
    }

    [[nodiscard]] auto validate(std::string_view name, AttributeValue const& value, AttributeScope scope) const
        -> Result<void, Error> override
    {
        auto const* definition = find(name);
        if (definition == nullptr) {
            return test_error("jobu.attribute.unknown");
        }
        if (!definition->scopes.test(scope)) {
            return test_error("jobu.attribute.invalid_scope");
        }
        if (!has_type(value, definition->type)) {
            return test_error("jobu.attribute.invalid_type");
        }
        return Result<void, Error>::success();
    }

    [[nodiscard]] auto definitions() const -> std::span<AttributeDefinition const> override { return _definitions; }

private:
    static auto all_scopes() -> AttributeScopes
    {
        return {AttributeScope::DaemonDefault, AttributeScope::QueueDefault, AttributeScope::Job};
    }

    std::array<AttributeDefinition, 8> _definitions{
        {
         {
                .name             = "test.boolean",
                .type             = AttributeType::Boolean,
                .scopes           = all_scopes(),
                .built_in_default = {.data = false},
                .description      = "Boolean test value",
            }, {
                .name             = "test.bytes",
                .type             = AttributeType::Bytes,
                .scopes           = AttributeScopes{AttributeScope::Job},
                .built_in_default = {.data = ByteBuffer{}},
                .description      = "Byte test value",
            }, {
                .name             = "test.duration",
                .type             = AttributeType::Duration,
                .scopes           = all_scopes(),
                .built_in_default = {.data = Duration::zero()},
                .description      = "Duration test value",
            }, {
                .name             = "test.integer",
                .type             = AttributeType::Integer,
                .scopes           = all_scopes(),
                .built_in_default = {.data = std::int64_t{0}},
                .description      = "Integer test value",
            }, {
                .name             = "test.list",
                .type             = AttributeType::List,
                .scopes           = all_scopes(),
                .built_in_default = {.data = AttributeValue::List{}},
                .description      = "List test value",
            }, {
                .name             = "test.map",
                .type             = AttributeType::Map,
                .scopes           = all_scopes(),
                .built_in_default = {.data = AttributeValue::Map{}},
                .description      = "Map test value",
            }, {
                .name             = "test.number",
                .type             = AttributeType::Number,
                .scopes           = all_scopes(),
                .built_in_default = {.data = 0.0},
                .description      = "Number test value",
            }, {
                .name             = "test.string",
                .type             = AttributeType::String,
                .scopes           = all_scopes(),
                .built_in_default = {.data = std::string{}},
                .description      = "String test value",
            }, }
    };
};

auto nested_list(std::size_t levels, AttributeValue leaf) -> AttributeValue
{
    for (std::size_t level = 0; level < levels; ++level) {
        leaf = AttributeValue{.data = AttributeValue::List{std::move(leaf)}};
    }
    return leaf;
}

auto typed_json(std::string type, JsonValue value) -> JsonValue
{
    return make_json(JsonValue::Object{
        {"type",  make_json(std::move(type))},
        {"value", std::move(value)          },
    });
}

auto attribute_document(JsonValue::Object values) -> JsonValue
{
    return make_json(JsonValue::Object{
        {"values",  make_json(std::move(values))},
        {"version", make_json(std::uint64_t{1}) },
    });
}

} // anonymous namespace

TEST_CASE("Standard attributes expose the specified definitions and defaults", "[jobu][attribute][materialization]")
{
    StandardAttributeRegistry                  registry;
    constexpr std::array<std::string_view, 19> expected_names{
        "http.follow_redirects",
        "http.idempotency_key",
        "http.max_redirects",
        "http.retry_errors",
        "http.retry_statuses",
        "http.tls_verify",
        "job.timeout",
        "output.capture",
        "output.http_body_limit",
        "output.http_headers_limit",
        "output.stderr_limit",
        "output.stdout_limit",
        "retry.initial_delay",
        "retry.jitter",
        "retry.max_attempts",
        "retry.max_delay",
        "retry.mode",
        "retry.multiplier",
        "retry.strategy",
    };
    auto const definitions = registry.definitions();
    REQUIRE(definitions.size() == expected_names.size());
    for (std::size_t index = 0; index < definitions.size(); ++index) {
        CHECK(definitions[index].name == expected_names[index]);
    }
    REQUIRE(registry.find("retry.jitter") != nullptr);
    REQUIRE(registry.find("retry.multiplier") != nullptr);
    CHECK(registry.find("retry.jitter")->type == AttributeType::Number);
    CHECK(registry.find("retry.multiplier")->type == AttributeType::Number);

    auto materialized = materialize_attributes(registry, {}, {}, {});
    REQUIRE(materialized);
    REQUIRE(materialized->size() == expected_names.size());
    CHECK_FALSE(std::get<bool>(materialized->at("http.follow_redirects").data));
    CHECK_FALSE(std::get<bool>(materialized->at("http.idempotency_key").data));
    CHECK(std::get<std::int64_t>(materialized->at("http.max_redirects").data) == 5);
    CHECK(same_list(std::get<AttributeValue::List>(materialized->at("http.retry_errors").data),
                    string_list({"resolve", "connect", "tls_handshake", "timeout", "send", "receive"})));
    CHECK(same_list(std::get<AttributeValue::List>(materialized->at("http.retry_statuses").data),
                    string_list({"408", "429", "500-599"})));
    CHECK(std::get<bool>(materialized->at("http.tls_verify").data));
    CHECK(std::get<Duration>(materialized->at("job.timeout").data) == 120s);
    CHECK(std::get<std::int64_t>(materialized->at("retry.max_attempts").data) == 1);
    CHECK(std::get<std::string>(materialized->at("retry.strategy").data) == "fixed");
    CHECK(std::get<Duration>(materialized->at("retry.initial_delay").data) == 0s);
    CHECK(std::get<double>(materialized->at("retry.jitter").data) == 0.0);
    CHECK(std::get<Duration>(materialized->at("retry.max_delay").data) == 24h);
    CHECK(std::get<std::string>(materialized->at("retry.mode").data) == "reschedule");
    CHECK(std::get<double>(materialized->at("retry.multiplier").data) == 2.0);
    CHECK(std::get<std::string>(materialized->at("output.capture").data) == "on_error");
    CHECK(std::get<std::int64_t>(materialized->at("output.http_body_limit").data) == std::int64_t{1024} * 1024);
    CHECK(std::get<std::int64_t>(materialized->at("output.http_headers_limit").data) == std::int64_t{64} * 1024);
    CHECK(std::get<std::int64_t>(materialized->at("output.stdout_limit").data) == std::int64_t{1024} * 1024);
    CHECK(std::get<std::int64_t>(materialized->at("output.stderr_limit").data) == std::int64_t{1024} * 1024);
    CHECK(registry.validate_materialized(*materialized));

    for (auto const& definition : registry.definitions()) {
        CHECK(definition.scopes.test(AttributeScope::DaemonDefault));
        CHECK(definition.scopes.test(AttributeScope::QueueDefault));
        CHECK(definition.scopes.test(AttributeScope::Job));
        CHECK(registry.validate(definition.name, definition.built_in_default, AttributeScope::DaemonDefault));
        CHECK(registry.validate(definition.name, definition.built_in_default, AttributeScope::QueueDefault));
        CHECK(registry.validate(definition.name, definition.built_in_default, AttributeScope::Job));
    }
}

TEST_CASE("Standard attributes enforce every field constraint", "[jobu][attribute][materialization]")
{
    StandardAttributeRegistry registry;
    auto                      validate = [&registry](std::string_view name, AttributeValue const& value) {
        return registry.validate(name, value, AttributeScope::Job);
    };

    CHECK(validate("http.follow_redirects", {.data = true}));
    CHECK(validate("http.idempotency_key", {.data = true}));
    CHECK(validate("http.tls_verify", {.data = false}));
    CHECK_FALSE(validate("http.follow_redirects", {.data = std::int64_t{1}}));

    CHECK(validate("http.max_redirects", {.data = std::int64_t{0}}));
    CHECK(validate("http.max_redirects", {.data = std::int64_t{20}}));
    CHECK_FALSE(validate("http.max_redirects", {.data = std::int64_t{-1}}));
    CHECK_FALSE(validate("http.max_redirects", {.data = std::int64_t{21}}));

    CHECK(validate("http.retry_errors", {.data = string_list({})}));
    CHECK(validate("http.retry_errors",
                   {.data = string_list({"resolve",
                                         "connect",
                                         "tls_handshake",
                                         "tls_verification",
                                         "timeout",
                                         "send",
                                         "receive",
                                         "redirect",
                                         "protocol"})}));
    CHECK_FALSE(validate("http.retry_errors", {.data = string_list({"connect", "connect"})}));
    CHECK_FALSE(validate("http.retry_errors", {.data = string_list({"cancelled"})}));
    CHECK_FALSE(
        validate("http.retry_errors",
                 {
                     .data = AttributeValue::List{{.data = std::string{"connect"}}, {.data = std::int64_t{1}}}
    }));

    CHECK(validate("http.retry_statuses", {.data = string_list({})}));
    CHECK(validate("http.retry_statuses", {.data = string_list({"100", "599", "200-299", "200-204"})}));
    CHECK_FALSE(validate("http.retry_statuses", {.data = string_list({"429", "429"})}));
    CHECK_FALSE(validate("http.retry_statuses", {.data = string_list({"99", "600", "299-200"})}));
    CHECK_FALSE(validate("http.retry_statuses",
                         {
                             .data = AttributeValue::List{{.data = std::string{"429"}}, {.data = false}}
    }));

    auto maximum_statuses = AttributeValue::List{};
    for (std::int64_t status = 100; status < 164; ++status) {
        maximum_statuses.push_back({.data = std::to_string(status)});
    }
    CHECK(validate("http.retry_statuses", {.data = maximum_statuses}));
    maximum_statuses.push_back({.data = std::string{"164"}});
    CHECK_FALSE(validate("http.retry_statuses", {.data = maximum_statuses}));

    CHECK(validate("job.timeout", {.data = 1ms}));
    CHECK(validate("job.timeout", {.data = std::chrono::days{30}}));
    CHECK_FALSE(validate("job.timeout", {.data = 1ms - 1ns}));
    CHECK_FALSE(validate("job.timeout", {.data = std::chrono::days{30} + 1ns}));

    CHECK(validate("retry.max_attempts", {.data = std::int64_t{1}}));
    CHECK(validate("retry.max_attempts", {.data = std::int64_t{100}}));
    CHECK_FALSE(validate("retry.max_attempts", {.data = std::int64_t{0}}));
    CHECK_FALSE(validate("retry.max_attempts", {.data = std::int64_t{101}}));

    CHECK(validate("retry.strategy", {.data = std::string{"fixed"}}));
    CHECK(validate("retry.strategy", {.data = std::string{"exponential"}}));
    CHECK_FALSE(validate("retry.strategy", {.data = std::string{"linear"}}));

    CHECK(validate("retry.initial_delay", {.data = 0s}));
    CHECK(validate("retry.initial_delay", {.data = 24h}));
    CHECK_FALSE(validate("retry.initial_delay", {.data = -1ns}));
    CHECK_FALSE(validate("retry.initial_delay", {.data = 24h + 1ns}));

    CHECK(validate("retry.jitter", {.data = 0.0}));
    CHECK(validate("retry.jitter", {.data = 1.0}));
    CHECK_FALSE(validate("retry.jitter", {.data = -0.01}));
    CHECK_FALSE(validate("retry.jitter", {.data = 1.01}));
    CHECK_FALSE(validate("retry.jitter", {.data = std::numeric_limits<double>::quiet_NaN()}));
    CHECK_FALSE(validate("retry.jitter", {.data = std::numeric_limits<double>::infinity()}));
    CHECK_FALSE(validate("retry.jitter", {.data = std::int64_t{0}}));

    CHECK(validate("retry.max_delay", {.data = 0s}));
    CHECK(validate("retry.max_delay", {.data = std::chrono::days{30}}));
    CHECK_FALSE(validate("retry.max_delay", {.data = -1ns}));
    CHECK_FALSE(validate("retry.max_delay", {.data = std::chrono::days{30} + 1ns}));

    CHECK(validate("retry.mode", {.data = std::string{"blocking"}}));
    CHECK(validate("retry.mode", {.data = std::string{"reschedule"}}));
    CHECK_FALSE(validate("retry.mode", {.data = std::string{"immediate"}}));

    CHECK(validate("retry.multiplier", {.data = 1.0}));
    CHECK(validate("retry.multiplier", {.data = 100.0}));
    CHECK_FALSE(validate("retry.multiplier", {.data = 0.99}));
    CHECK_FALSE(validate("retry.multiplier", {.data = 100.01}));
    CHECK_FALSE(validate("retry.multiplier", {.data = std::numeric_limits<double>::quiet_NaN()}));
    CHECK_FALSE(validate("retry.multiplier", {.data = std::numeric_limits<double>::infinity()}));
    CHECK_FALSE(validate("retry.multiplier", {.data = std::int64_t{2}}));

    for (auto const* capture : {"none", "on_error", "always"}) {
        CHECK(validate("output.capture", {.data = std::string{capture}}));
    }
    CHECK_FALSE(validate("output.capture", {.data = std::string{"errors"}}));

    for (auto const* name : {"output.stdout_limit", "output.stderr_limit"}) {
        CHECK(validate(name, {.data = std::int64_t{0}}));
        CHECK(validate(name, {.data = std::int64_t{64} * 1024 * 1024}));
        CHECK_FALSE(validate(name, {.data = std::int64_t{-1}}));
        CHECK_FALSE(validate(name, {.data = (std::int64_t{64} * 1024 * 1024) + 1}));
    }

    CHECK(validate("output.http_body_limit", {.data = std::int64_t{0}}));
    CHECK(validate("output.http_body_limit", {.data = std::int64_t{64} * 1024 * 1024}));
    CHECK_FALSE(validate("output.http_body_limit", {.data = std::int64_t{-1}}));
    CHECK_FALSE(validate("output.http_body_limit", {.data = (std::int64_t{64} * 1024 * 1024) + 1}));

    CHECK(validate("output.http_headers_limit", {.data = std::int64_t{0}}));
    CHECK(validate("output.http_headers_limit", {.data = std::int64_t{4} * 1024 * 1024}));
    CHECK_FALSE(validate("output.http_headers_limit", {.data = std::int64_t{-1}}));
    CHECK_FALSE(validate("output.http_headers_limit", {.data = (std::int64_t{4} * 1024 * 1024) + 1}));

    CHECK(validate("missing.value", {.data = true}).error().code == "jobu.attribute.unknown");
    CHECK(validate("job.timeout", {.data = std::int64_t{1}}).error().code == "jobu.attribute.invalid_type");
}

TEST_CASE("Phase 4 retry number attributes round trip through public JSON", "[jobu][attribute][json]")
{
    StandardAttributeRegistry registry;
    AttributeSet              values{
        {"retry.jitter",     {.data = 0.25}},
        {"retry.multiplier", {.data = 3.5} },
    };

    auto encoded = attribute_set_to_json(values, registry, AttributeScope::Job);
    REQUIRE(encoded);
    CHECK(encoded->as_object().at("retry.jitter").as_double() == 0.25);
    CHECK(encoded->as_object().at("retry.multiplier").as_double() == 3.5);

    auto decoded = attribute_set_from_json(*encoded, registry, AttributeScope::Job);
    REQUIRE(decoded);
    CHECK(same_set(*decoded, values));
}

TEST_CASE("Phase 5 HTTP attributes round trip through public management JSON", "[jobu][attribute][json][http]")
{
    StandardAttributeRegistry registry;
    AttributeSet              values{
        {"http.follow_redirects",     {.data = true}                                  },
        {"http.idempotency_key",      {.data = true}                                  },
        {"http.max_redirects",        {.data = std::int64_t{7}}                       },
        {"http.retry_errors",         {.data = string_list({"connect", "protocol"})}  },
        {"http.retry_statuses",       {.data = string_list({"408", "429", "500-599"})}},
        {"http.tls_verify",           {.data = false}                                 },
        {"output.http_body_limit",    {.data = std::int64_t{2048}}                    },
        {"output.http_headers_limit", {.data = std::int64_t{1024}}                    },
    };

    auto encoded = attribute_set_to_json(values, registry, AttributeScope::Job);
    REQUIRE(encoded);
    auto const& object = encoded->as_object();
    CHECK(object.at("http.follow_redirects").as_bool());
    CHECK(object.at("http.max_redirects").as_int() == 7);
    REQUIRE(object.at("http.retry_errors").as_array().size() == 2U);
    CHECK(object.at("http.retry_errors").as_array()[0].as_string() == "connect");
    CHECK(object.at("http.retry_statuses").as_array()[2].as_string() == "500-599");

    auto decoded = attribute_set_from_json(*encoded, registry, AttributeScope::Job);
    REQUIRE(decoded);
    CHECK(same_set(*decoded, values));
}

TEST_CASE("Attribute materialization applies deterministic precedence and cross-field validation",
          "[jobu][attribute][materialization]")
{
    StandardAttributeRegistry registry;
    AttributeSet              daemon{
        {"job.timeout",        {.data = 90s}            },
        {"retry.max_attempts", {.data = std::int64_t{2}}}
    };
    AttributeSet queue{
        {"job.timeout",        {.data = 60s}            },
        {"retry.max_attempts", {.data = std::int64_t{3}}}
    };
    AttributeSet job{
        {"job.timeout", {.data = 30s}}
    };

    auto materialized = materialize_attributes(registry, daemon, queue, job);
    REQUIRE(materialized);
    CHECK(std::get<Duration>(materialized->at("job.timeout").data) == 30s);
    CHECK(std::get<std::int64_t>(materialized->at("retry.max_attempts").data) == 3);

    queue.insert_or_assign("retry.max_attempts", AttributeValue{.data = std::int64_t{9}});
    CHECK(std::get<std::int64_t>(materialized->at("retry.max_attempts").data) == 3);

    auto invalid = materialize_attributes(registry,
                                          {
    },
                                          {{"retry.initial_delay", {.data = 2h}}},
                                          {{"retry.max_delay", {.data = 1h}}});
    REQUIRE_FALSE(invalid);
    CHECK(invalid.error().code == "jobu.attribute.invalid_value");

    auto incomplete = *materialized;
    incomplete.erase("retry.mode");
    CHECK(registry.validate_materialized(incomplete).error().code == "jobu.attribute.invalid_value");

    auto partial_follow = attribute_set_to_json(
        {
            {"http.follow_redirects", {.data = true}}
    },
        registry,
        AttributeScope::QueueDefault);
    CHECK(partial_follow);
    auto partial_maximum = attribute_set_from_json(make_json(JsonValue::Object{
                                                       {"http.max_redirects", make_json(std::int64_t{0})}
    }),
                                                   registry,
                                                   AttributeScope::Job);
    CHECK(partial_maximum);

    auto invalid_redirects = materialize_attributes(registry,
                                                    {
                                                        {"http.follow_redirects", {.data = true}}
    },
                                                    {{"http.max_redirects", {.data = std::int64_t{0}}}},
                                                    {});
    REQUIRE_FALSE(invalid_redirects);
    CHECK(invalid_redirects.error().code == "jobu.attribute.invalid_value");

    auto disabled_redirects = materialize_attributes(registry,
                                                     {
                                                         {"http.follow_redirects", {.data = false}}
    },
                                                     {{"http.max_redirects", {.data = std::int64_t{0}}}},
                                                     {});
    REQUIRE(disabled_redirects);
}

TEST_CASE("Materialized Phase 4 snapshots acquire all Phase 5 HTTP defaults",
          "[jobu][attribute][materialization][compatibility]")
{
    constexpr std::string_view phase4_snapshot = R"({
        "values": {
            "job.timeout": {"type": "duration_ns", "value": 120000000000},
            "output.capture": {"type": "string", "value": "on_error"},
            "output.stderr_limit": {"type": "integer", "value": 1048576},
            "output.stdout_limit": {"type": "integer", "value": 1048576},
            "retry.initial_delay": {"type": "duration_ns", "value": 0},
            "retry.jitter": {"type": "number", "value": 0.0},
            "retry.max_attempts": {"type": "integer", "value": 1},
            "retry.max_delay": {"type": "duration_ns", "value": 86400000000000},
            "retry.mode": {"type": "string", "value": "reschedule"},
            "retry.multiplier": {"type": "number", "value": 2.0},
            "retry.strategy": {"type": "string", "value": "fixed"}
        },
        "version": 1
    })";

    StandardAttributeRegistry registry;
    auto                      document = parse_json(phase4_snapshot);
    REQUIRE(document);

    auto decoded =
        decode_attribute_document(registry, *document, AttributeScope::Job, AttributeDocumentMode::Materialized);
    REQUIRE(decoded);
    REQUIRE(decoded->size() == 19U);
    CHECK_FALSE(std::get<bool>(decoded->at("http.follow_redirects").data));
    CHECK_FALSE(std::get<bool>(decoded->at("http.idempotency_key").data));
    CHECK(std::get<std::int64_t>(decoded->at("http.max_redirects").data) == 5);
    CHECK(same_list(std::get<AttributeValue::List>(decoded->at("http.retry_errors").data),
                    string_list({"resolve", "connect", "tls_handshake", "timeout", "send", "receive"})));
    CHECK(same_list(std::get<AttributeValue::List>(decoded->at("http.retry_statuses").data),
                    string_list({"408", "429", "500-599"})));
    CHECK(std::get<bool>(decoded->at("http.tls_verify").data));
    CHECK(std::get<std::int64_t>(decoded->at("output.http_body_limit").data) == std::int64_t{1024} * 1024);
    CHECK(std::get<std::int64_t>(decoded->at("output.http_headers_limit").data) == std::int64_t{64} * 1024);

    auto partial = decode_attribute_document(registry, *document, AttributeScope::Job, AttributeDocumentMode::Partial);
    REQUIRE(partial);
    CHECK(partial->size() == 11U);
    CHECK_FALSE(partial->contains("http.follow_redirects"));
    CHECK_FALSE(partial->contains("output.http_body_limit"));
}

TEST_CASE("Public attribute JSON is registry directed and round trips natural values", "[jobu][attribute][json]")
{
    CompleteTypeRegistry registry;
    AttributeSet         values{
        {"test.boolean",  {.data = true}                                                             },
        {"test.bytes",    {.data = ByteBuffer{std::byte{0x00}, std::byte{0xaf}, std::byte{0xff}}}    },
        {"test.duration", {.data = 1500ms}                                                           },
        {"test.integer",  {.data = std::int64_t{42}}                                                 },
        {"test.list",     {.data = AttributeValue::List{{.data = false}, {.data = std::int64_t{-7}}}}},
        {"test.map",      {.data = AttributeValue::Map{{"name", {.data = std::string{"value"}}}}}    },
        {"test.number",   {.data = 1.5}                                                              },
        {"test.string",   {.data = std::string{"text"}}                                              },
    };

    auto encoded = attribute_set_to_json(values, registry, AttributeScope::Job);
    REQUIRE(encoded);
    CHECK(encoded->as_object().at("test.bytes").as_string() == "00afff");
    CHECK(encoded->as_object().at("test.duration").as_int() == 1500);

    auto text = serialize_json(*encoded);
    REQUIRE(text);
    auto parsed = parse_json(*text);
    REQUIRE(parsed);
    auto decoded = attribute_set_from_json(*parsed, registry, AttributeScope::Job);
    REQUIRE(decoded);
    CHECK(same_set(*decoded, values));

    auto wrong_scope = attribute_set_to_json(
        {
            {"test.bytes", {.data = ByteBuffer{}}}
    },
        registry,
        AttributeScope::QueueDefault);
    REQUIRE_FALSE(wrong_scope);
    CHECK(wrong_scope.error().code == "jobu.attribute.invalid_scope");
}

TEST_CASE("Public attribute JSON rejects lossy, malformed, and unsupported recursive values", "[jobu][attribute][json]")
{
    CompleteTypeRegistry registry;

    auto lossy = attribute_set_to_json(
        {
            {"test.duration", {.data = 1ms + 1ns}}
    },
        registry,
        AttributeScope::Job);
    REQUIRE_FALSE(lossy);
    CHECK(lossy.error().code == "jobu.attribute.invalid_value");

    auto overflow =
        attribute_set_from_json(make_json(JsonValue::Object{
                                    {"test.duration", make_json(std::numeric_limits<std::int64_t>::max())}
    }),
                                registry,
                                AttributeScope::Job);
    REQUIRE_FALSE(overflow);
    CHECK(overflow.error().code == "jobu.attribute.invalid_value");

    auto uppercase = attribute_set_from_json(make_json(JsonValue::Object{
                                                 {"test.bytes", make_json(std::string{"AA"})}
    }),
                                             registry,
                                             AttributeScope::Job);
    REQUIRE_FALSE(uppercase);
    CHECK(uppercase.error().code == "jobu.attribute.invalid_value");

    auto nested_duration = attribute_set_to_json(
        {
            {"test.list", {.data = AttributeValue::List{{.data = 1s}}}}
    },
        registry,
        AttributeScope::Job);
    REQUIRE_FALSE(nested_duration);
    CHECK(nested_duration.error().code == "jobu.attribute.invalid_value");

    auto nested_bytes = attribute_set_to_json(
        {
            {"test.map", {.data = AttributeValue::Map{{"data", {.data = ByteBuffer{}}}}}}
    },
        registry,
        AttributeScope::Job);
    REQUIRE_FALSE(nested_bytes);
    CHECK(nested_bytes.error().code == "jobu.attribute.invalid_value");

    auto nested_null = attribute_set_from_json(make_json(JsonValue::Object{
                                                   {"test.list", make_json(JsonValue::Array{make_json(JsonNull{})})}
    }),
                                               registry,
                                               AttributeScope::Job);
    REQUIRE_FALSE(nested_null);
    CHECK(nested_null.error().code == "jobu.attribute.invalid_value");

    auto too_deep = attribute_set_to_json(
        {
            {"test.list", nested_list(65U, {.data = std::int64_t{1}})}
    },
        registry,
        AttributeScope::Job);
    REQUIRE_FALSE(too_deep);
    CHECK(too_deep.error().code == "jobu.attribute.invalid_value");
}

TEST_CASE("Attribute persistence validates UTF-8 while traversing typed values", "[jobu][storage][attribute]")
{
    CompleteTypeRegistry registry;
    auto const           invalid_utf8 = std::string{"\xc3\x28", 2U};

    SECTION("encoding rejects invalid string values")
    {
        auto encoded = encode_attribute_document(registry,
                                                 {
                                                     {"test.string", {.data = invalid_utf8}}
        },
                                                 AttributeScope::Job,
                                                 AttributeDocumentMode::Partial);
        REQUIRE_FALSE(encoded);
        CHECK(encoded.error().category == ErrorCategory::InvalidArgument);
        CHECK(encoded.error().code == "jobu.attribute.invalid_value");
        CHECK(encoded.error().message == "JobU attribute text is not valid UTF-8");
    }

    SECTION("encoding rejects invalid nested map keys")
    {
        auto encoded =
            encode_attribute_document(registry,
                                      {
                                          {"test.map", {.data = AttributeValue::Map{{invalid_utf8, {.data = true}}}}}
        },
                                      AttributeScope::Job,
                                      AttributeDocumentMode::Partial);
        REQUIRE_FALSE(encoded);
        CHECK(encoded.error().category == ErrorCategory::InvalidArgument);
        CHECK(encoded.error().code == "jobu.attribute.invalid_value");
        CHECK(encoded.error().message == "JobU attribute text is not valid UTF-8");
    }

    auto decode = [&registry](JsonValue const& document) {
        return decode_attribute_document(registry, document, AttributeScope::Job, AttributeDocumentMode::Partial);
    };

    SECTION("decoding rejects invalid string values")
    {
        auto decoded = decode(attribute_document({
            {"test.string", typed_json("string", make_json(invalid_utf8))}
        }));
        REQUIRE_FALSE(decoded);
        CHECK(decoded.error().category == ErrorCategory::Internal);
        CHECK(decoded.error().code == "jobu.attribute.invalid_document");
        CHECK(decoded.error().detail == "reason=invalid_utf8");
    }

    SECTION("decoding rejects invalid nested map keys")
    {
        auto decoded = decode(attribute_document({
            {"test.map", typed_json("map", make_json(JsonValue::Object{{invalid_utf8, make_json(true)}}))}
        }));
        REQUIRE_FALSE(decoded);
        CHECK(decoded.error().category == ErrorCategory::Internal);
        CHECK(decoded.error().code == "jobu.attribute.invalid_document");
        CHECK(decoded.error().detail == "reason=invalid_utf8");
    }

    SECTION("decoding rejects invalid type tags and top-level names")
    {
        auto invalid_tag = decode(attribute_document({
            {"test.string", typed_json(invalid_utf8, make_json(std::string{"text"}))}
        }));
        REQUIRE_FALSE(invalid_tag);
        CHECK(invalid_tag.error().detail == "reason=invalid_utf8");

        auto invalid_name = decode(attribute_document({
            {invalid_utf8, typed_json("string", make_json(std::string{"text"}))}
        }));
        REQUIRE_FALSE(invalid_name);
        CHECK(invalid_name.error().detail == "reason=invalid_utf8");
    }

    SECTION("valid multibyte text round trips")
    {
        AttributeSet values{
            {"test.map",    {.data = AttributeValue::Map{{"ключ", {.data = std::string{"значение"}}}}}},
            {"test.string", {.data = std::string{"Tere, maailm! 🌍"}}                                 },
        };
        auto encoded = encode_attribute_document(registry, values, AttributeScope::Job, AttributeDocumentMode::Partial);
        REQUIRE(encoded);
        auto decoded = decode(*encoded);
        REQUIRE(decoded);
        CHECK(same_set(*decoded, values));
    }
}

TEST_CASE("Serialized attribute documents own the canonical encoded bytes", "[jobu][storage][attribute]")
{
    CompleteTypeRegistry registry;
    AttributeSet         partial{
        {"test.map",    {.data = AttributeValue::Map{{"nested", {.data = std::string{"value"}}}}}},
        {"test.string", {.data = std::string{"text"}}                                            },
    };

    auto partial_json =
        encode_attribute_document(registry, partial, AttributeScope::QueueDefault, AttributeDocumentMode::Partial);
    REQUIRE(partial_json);
    auto partial_text = serialize_json(*partial_json);
    REQUIRE(partial_text);
    auto serialized_partial = encode_and_serialize_attribute_document(registry,
                                                                      partial,
                                                                      AttributeScope::QueueDefault,
                                                                      AttributeDocumentMode::Partial);
    REQUIRE(serialized_partial);
    CHECK(serialized_partial->serialized() == *partial_text);

    auto materialized = materialize_attributes(registry, {}, {}, {});
    REQUIRE(materialized);
    auto materialized_json =
        encode_attribute_document(registry, *materialized, AttributeScope::Job, AttributeDocumentMode::Materialized);
    REQUIRE(materialized_json);
    auto materialized_text = serialize_json(*materialized_json);
    REQUIRE(materialized_text);
    auto serialized_materialized = encode_and_serialize_attribute_document(registry,
                                                                           *materialized,
                                                                           AttributeScope::Job,
                                                                           AttributeDocumentMode::Materialized);
    REQUIRE(serialized_materialized);
    CHECK(serialized_materialized->serialized() == *materialized_text);
}

TEST_CASE("Attribute persistence retains numeric and nesting validation", "[jobu][storage][attribute]")
{
    CompleteTypeRegistry registry;

    auto non_finite = encode_attribute_document(registry,
                                                {
                                                    {"test.number", {.data = std::numeric_limits<double>::infinity()}}
    },
                                                AttributeScope::Job,
                                                AttributeDocumentMode::Partial);
    REQUIRE_FALSE(non_finite);
    CHECK(non_finite.error().code == "jobu.attribute.invalid_value");

    auto too_deep = encode_attribute_document(registry,
                                              {
                                                  {"test.list", nested_list(65U, {.data = std::int64_t{1}})}
    },
                                              AttributeScope::Job,
                                              AttributeDocumentMode::Partial);
    REQUIRE_FALSE(too_deep);
    CHECK(too_deep.error().code == "jobu.attribute.invalid_value");

    auto decoded_number = decode_attribute_document(
        registry,
        attribute_document({
            {"test.number", typed_json("number", make_json(std::numeric_limits<double>::infinity()))}
    }),
        AttributeScope::Job,
        AttributeDocumentMode::Partial);
    REQUIRE_FALSE(decoded_number);
    CHECK(decoded_number.error().code == "jobu.attribute.invalid_document");
    CHECK(decoded_number.error().detail == "reason=number_shape");
}
