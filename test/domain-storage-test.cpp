#include "domain_storage_priv.hpp"

#include "attribute_codec_priv.hpp"
#include "attribute_registry.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

using namespace jb::core;
using namespace jb::db;
using namespace jb::jobu;
using namespace jb::jobu::detail;
using namespace jb::rpc;
using namespace std::chrono_literals;

namespace {

auto record(std::string name, Value value) -> Record
{
    return Record{{Field{std::move(name), std::move(value)}}};
}

auto json(auto value) -> JsonValue
{
    JsonValue result;
    result.data = std::move(value);
    return result;
}

auto same_attribute_value(AttributeValue const& left, AttributeValue const& right) -> bool;

auto same_attribute_list(AttributeValue::List const& left, AttributeValue::List const& right) -> bool
{
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (!same_attribute_value(left[index], right[index])) {
            return false;
        }
    }
    return true;
}

auto same_attribute_map(AttributeValue::Map const& left, AttributeValue::Map const& right) -> bool
{
    if (left.size() != right.size()) {
        return false;
    }
    auto right_entry = right.begin();
    for (auto const& [name, value] : left) {
        if (name != right_entry->first || !same_attribute_value(value, right_entry->second)) {
            return false;
        }
        ++right_entry;
    }
    return true;
}

auto same_attribute_value(AttributeValue const& left, AttributeValue const& right) -> bool
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
            return same_attribute_list(std::get<AttributeValue::List>(left.data),
                                       std::get<AttributeValue::List>(right.data));
        case 7:
            return same_attribute_map(std::get<AttributeValue::Map>(left.data),
                                      std::get<AttributeValue::Map>(right.data));
        default:
            return false;
    }
}

auto same_attribute_set(AttributeSet const& left, AttributeSet const& right) -> bool
{
    if (left.size() != right.size()) {
        return false;
    }
    auto right_entry = right.begin();
    for (auto const& [name, value] : left) {
        if (name != right_entry->first || !same_attribute_value(value, right_entry->second)) {
            return false;
        }
        ++right_entry;
    }
    return true;
}

auto codec_error(std::string code) -> Result<void, Error>
{
    return Result<void, Error>::failure({
        .category = ErrorCategory::InvalidArgument,
        .code     = std::move(code),
        .message  = "Invalid codec test attribute",
    });
}

auto codec_type_matches(AttributeValue const& value, AttributeType type) -> bool
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

class CodecAttributeRegistry final : public AttributeRegistry {
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
            return codec_error("jobu.attribute.unknown");
        }
        if (!definition->scopes.test(scope)) {
            return codec_error("jobu.attribute.invalid_scope");
        }
        if (!codec_type_matches(value, definition->type)) {
            return codec_error("jobu.attribute.invalid_type");
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
                .description      = "Boolean codec value",
            }, {
                .name             = "test.bytes",
                .type             = AttributeType::Bytes,
                .scopes           = all_scopes(),
                .built_in_default = {.data = ByteBuffer{}},
                .description      = "Byte codec value",
            }, {
                .name             = "test.duration",
                .type             = AttributeType::Duration,
                .scopes           = all_scopes(),
                .built_in_default = {.data = Duration::zero()},
                .description      = "Duration codec value",
            }, {
                .name             = "test.integer",
                .type             = AttributeType::Integer,
                .scopes           = all_scopes(),
                .built_in_default = {.data = std::int64_t{0}},
                .description      = "Integer codec value",
            }, {
                .name             = "test.list",
                .type             = AttributeType::List,
                .scopes           = all_scopes(),
                .built_in_default = {.data = AttributeValue::List{}},
                .description      = "List codec value",
            }, {
                .name             = "test.map",
                .type             = AttributeType::Map,
                .scopes           = all_scopes(),
                .built_in_default = {.data = AttributeValue::Map{}},
                .description      = "Map codec value",
            }, {
                .name             = "test.number",
                .type             = AttributeType::Number,
                .scopes           = all_scopes(),
                .built_in_default = {.data = 0.0},
                .description      = "Number codec value",
            }, {
                .name             = "test.string",
                .type             = AttributeType::String,
                .scopes           = all_scopes(),
                .built_in_default = {.data = std::string{}},
                .description      = "String codec value",
            }, }
    };
};

auto typed_json(std::string type, JsonValue value) -> JsonValue
{
    return json(JsonValue::Object{
        {"type",  json(std::move(type))},
        {"value", std::move(value)     },
    });
}

auto attribute_document(JsonValue::Object values, std::uint64_t version = 1) -> JsonValue
{
    return json(JsonValue::Object{
        {"values",  json(std::move(values))},
        {"version", json(version)          },
    });
}

auto nested_typed_list(std::size_t levels) -> JsonValue
{
    auto value = typed_json("integer", json(std::int64_t{1}));
    for (std::size_t level = 0; level < levels; ++level) {
        value = typed_json("list", json(JsonValue::Array{std::move(value)}));
    }
    return value;
}

} // anonymous namespace

TEST_CASE("Domain storage preserves UUID bytes and rejects malformed fields", "[jobu][storage]")
{
    auto const uuid   = *Uuid::parse("00112233-4455-6677-8899-aabbccddeeff");
    auto const stored = uuid_to_storage(uuid);
    REQUIRE(std::holds_alternative<ByteBuffer>(stored));
    CHECK(std::get<ByteBuffer>(stored).size() == 16);

    auto const decoded = read_uuid(record("id", stored), "id");
    REQUIRE(decoded);
    CHECK(*decoded == uuid);

    for (auto const& invalid : {
             record("id", ByteBuffer(15)),
             record("id", std::string{"not-a-blob"}),
             Record{},
         }) {
        auto const result = read_uuid(invalid, "id");
        REQUIRE_FALSE(result);
        CHECK(result.error().code == "jobu.storage.invalid_uuid");
        CHECK(result.error().detail.find("field=id") != std::string::npos);
    }
}

TEST_CASE("Domain storage preserves signed Unix microseconds", "[jobu][storage]")
{
    using Microseconds = std::chrono::microseconds;

    auto const time   = UtcTimePoint{-1us};
    auto const stored = timestamp_to_storage(time);
    REQUIRE(stored);
    CHECK(std::get<std::int64_t>(*stored) == -1);

    auto const decoded = read_timestamp(record("updated_at_us", *stored), "updated_at_us");
    REQUIRE(decoded);
    CHECK(*decoded == time);

    auto const absent = read_optional_timestamp(record("deleted_at_us", Null{}), "deleted_at_us");
    REQUIRE(absent);
    CHECK_FALSE(absent->has_value());

    auto const minimum = std::chrono::ceil<Microseconds>(UtcTimePoint::min().time_since_epoch()).count();
    auto const maximum = std::chrono::floor<Microseconds>(UtcTimePoint::max().time_since_epoch()).count();
    for (auto const boundary : {minimum, maximum}) {
        auto const decoded_boundary = read_timestamp(record("created_at_us", boundary), "created_at_us");
        REQUIRE(decoded_boundary);
        CHECK(std::chrono::duration_cast<Microseconds>(decoded_boundary->time_since_epoch()).count() == boundary);
    }

    auto const wrong_type = read_timestamp(record("created_at_us", std::string{"invalid"}), "created_at_us");
    REQUIRE_FALSE(wrong_type);
    CHECK(wrong_type.error().code == "jobu.storage.invalid_time");

    if (minimum > std::numeric_limits<std::int64_t>::min()) {
        auto below_minimum_value = minimum;
        --below_minimum_value;
        auto const below_minimum = read_timestamp(record("created_at_us", below_minimum_value), "created_at_us");
        REQUIRE_FALSE(below_minimum);
        CHECK(below_minimum.error().code == "jobu.storage.invalid_time");
    }
    if (maximum < std::numeric_limits<std::int64_t>::max()) {
        auto above_maximum_value = maximum;
        ++above_maximum_value;
        auto const above_maximum = read_timestamp(record("created_at_us", above_maximum_value), "created_at_us");
        REQUIRE_FALSE(above_maximum);
        CHECK(above_maximum.error().code == "jobu.storage.invalid_time");
    }
}

TEST_CASE("Domain storage checks booleans and bounded integers", "[jobu][storage]")
{
    CHECK(std::get<std::int64_t>(boolean_to_storage(false)) == 0);
    CHECK(std::get<std::int64_t>(boolean_to_storage(true)) == 1);
    CHECK_FALSE(*read_boolean(record("flag", std::int64_t{0}), "flag"));
    CHECK(*read_boolean(record("flag", std::int64_t{1}), "flag"));
    CHECK(read_boolean(record("flag", std::int64_t{2}), "flag").error().code == "jobu.storage.invalid_boolean");

    REQUIRE(revision_to_storage(1));
    REQUIRE(attempt_number_to_storage(static_cast<AttemptNumber>(std::numeric_limits<std::int64_t>::max())));
    CHECK_FALSE(revision_to_storage(0));
    CHECK_FALSE(revision_to_storage(std::numeric_limits<JobRevision>::max()));
    CHECK_FALSE(attempt_number_to_storage(0));

    CHECK(*read_revision(record("revision", std::int64_t{7}), "revision") == 7);
    CHECK(*read_attempt_number(record("attempt_number", std::int64_t{3}), "attempt_number") == 3);
    CHECK_FALSE(read_revision(record("revision", std::int64_t{-1}), "revision"));
    CHECK_FALSE(read_attempt_number(record("attempt_number", std::int64_t{0}), "attempt_number"));

    CHECK(std::get<std::int64_t>(int32_to_storage(std::numeric_limits<std::int32_t>::min())) ==
          std::numeric_limits<std::int32_t>::min());
    CHECK(*read_int32(record("priority", std::int64_t{42}), "priority") == 42);
    CHECK_FALSE(read_int32(record("priority", std::numeric_limits<std::int64_t>::max()), "priority"));
}

TEST_CASE("Domain storage bounds nonnegative durations to signed storage range", "[jobu][storage]")
{
    auto absent = optional_nonnegative_seconds_to_storage(std::nullopt);
    REQUIRE(absent);
    CHECK(std::holds_alternative<Null>(*absent));

    auto zero_seconds = optional_nonnegative_seconds_to_storage(0s);
    REQUIRE(zero_seconds);
    CHECK(std::get<std::int64_t>(*zero_seconds) == 0);
    CHECK_FALSE(optional_nonnegative_seconds_to_storage(-1s));

    auto maximum_seconds = optional_nonnegative_seconds_to_storage(std::chrono::seconds::max());
    if constexpr (std::in_range<std::int64_t>(std::chrono::seconds::max().count())) {
        REQUIRE(maximum_seconds);
        CHECK(std::get<std::int64_t>(*maximum_seconds) == std::chrono::seconds::max().count());
    }
    else {
        CHECK_FALSE(maximum_seconds);
    }

    auto zero_milliseconds = nonnegative_milliseconds_to_storage(0ms);
    REQUIRE(zero_milliseconds);
    CHECK(std::get<std::int64_t>(*zero_milliseconds) == 0);
    CHECK_FALSE(nonnegative_milliseconds_to_storage(-1ms));

    auto maximum_milliseconds = nonnegative_milliseconds_to_storage(std::chrono::milliseconds::max());
    if constexpr (std::in_range<std::int64_t>(std::chrono::milliseconds::max().count())) {
        REQUIRE(maximum_milliseconds);
        CHECK(std::get<std::int64_t>(*maximum_milliseconds) == std::chrono::milliseconds::max().count());
    }
    else {
        CHECK_FALSE(maximum_milliseconds);
    }
}

TEST_CASE("Domain storage handles required and optional text", "[jobu][storage]")
{
    CHECK(*read_text(record("name", std::string{"queue"}), "name") == "queue");
    CHECK(*read_optional_text(record("name", std::string{"job"}), "name") == std::optional<std::string>{"job"});
    CHECK_FALSE(read_optional_text(record("name", Null{}), "name")->has_value());
    CHECK(read_text(record("name", std::int64_t{1}), "name").error().code == "jobu.storage.invalid_text");
}

TEST_CASE("Domain storage preserves nullable binary values", "[jobu][storage]")
{
    auto const bytes = ByteBuffer{std::byte{0x00}, std::byte{0xff}, std::byte{0x00}};
    CHECK(*read_optional_blob(record("output", bytes), "output") == std::optional<ByteBuffer>{bytes});
    CHECK_FALSE(read_optional_blob(record("output", Null{}), "output")->has_value());
    CHECK(read_optional_blob(record("output", std::string{"not a blob"}), "output").error().code ==
          "jobu.storage.invalid_blob");
}

TEST_CASE("Domain storage uses stable lower-case enum text", "[jobu][storage]")
{
    CHECK(storage_text(QueueState::Suspending) == "suspending");
    CHECK(storage_text(RecoveryPolicy::RetryInterrupted) == "retry_interrupted");
    CHECK(storage_text(JobState::Suspended) == "suspended");
    CHECK(storage_text(JobType::Http) == "http");
    CHECK(storage_text(RunOrigin::Submitted) == "submitted");
    CHECK(storage_text(RunState::RetryWait) == "retry_wait");
    CHECK(storage_text(AttemptState::Completed) == "completed");
    CHECK(storage_text(AttemptOutcome::Interrupted) == "interrupted");

    CHECK(*read_queue_state(record("state", std::string{"deleted"}), "state") == QueueState::Deleted);
    CHECK(*read_recovery_policy(record("policy", std::string{"fail_interrupted"}), "policy") ==
          RecoveryPolicy::FailInterrupted);
    CHECK(*read_job_state(record("state", std::string{"active"}), "state") == JobState::Active);
    CHECK(*read_job_type(record("type", std::string{"cli"}), "type") == JobType::Cli);
    CHECK(*read_run_origin(record("origin", std::string{"manual"}), "origin") == RunOrigin::Manual);
    CHECK(*read_run_state(record("state", std::string{"cancelled"}), "state") == RunState::Cancelled);
    CHECK(*read_attempt_state(record("state", std::string{"running"}), "state") == AttemptState::Running);
    CHECK(*read_attempt_outcome(record("outcome", std::string{"failed"}), "outcome") == AttemptOutcome::Failed);

    auto const invalid = read_run_state(record("state", std::string{"unknown-secret-text"}), "state");
    REQUIRE_FALSE(invalid);
    CHECK(invalid.error().code == "jobu.storage.invalid_enum");
    CHECK(invalid.error().detail.find("unknown-secret-text") == std::string::npos);
}

TEST_CASE("Domain storage round-trips bounded deterministic JSON", "[jobu][storage]")
{
    auto const object = json(JsonValue::Object{
        {"b", json(std::uint64_t{2})    },
        {"a", json(std::string{"first"})},
    });
    auto const stored = json_to_storage(object, true, 64);
    REQUIRE(stored);
    CHECK(std::get<std::string>(*stored) == R"({"a":"first","b":2})");

    auto const decoded = read_json(record("payload_json", *stored), "payload_json", true, 64);
    REQUIRE(decoded);
    CHECK(*decoded == object);

    auto const absent = read_optional_json(record("result_json", Null{}), "result_json", true, 64);
    REQUIRE(absent);
    CHECK_FALSE(absent->has_value());

    CHECK(json_to_storage(json(JsonValue::Array{}), true, 64).error().code == "jobu.storage.invalid_json");
    CHECK(json_to_storage(object, true, 4).error().code == "jobu.storage.invalid_json");
    CHECK(read_json(record("payload_json", std::string{"{"}), "payload_json", true, 64).error().code ==
          "jobu.storage.invalid_json");
    CHECK(read_json(record("payload_json", std::string{"[]"}), "payload_json", true, 64).error().code ==
          "jobu.storage.invalid_json");
    CHECK(read_json(record("payload_json", std::string{R"({"duplicate":1,"duplicate":2})"}), "payload_json", true, 64)
              .error()
              .code == "jobu.storage.invalid_json");
}

TEST_CASE("Attribute persistence uses explicit typed tags for every value alternative", "[jobu][storage][attribute]")
{
    CodecAttributeRegistry registry;
    AttributeSet           values{
        {"test.boolean",  {.data = true}                                                         },
        {"test.bytes",    {.data = ByteBuffer{std::byte{0x00}, std::byte{0xaf}, std::byte{0xff}}}},
        {"test.duration", {.data = 1500ms}                                                       },
        {"test.integer",  {.data = std::int64_t{-42}}                                            },
        {"test.list",
         {.data =
              AttributeValue::List{
                  {.data = 2s},
                  {.data = ByteBuffer{std::byte{0x10}}},
              }}                                                                                 },
        {"test.map",
         {.data =
              AttributeValue::Map{
                  {"flag", {.data = false}},
                  {"nested", {.data = AttributeValue::List{{.data = std::string{"value"}}}}},
              }}                                                                                 },
        {"test.number",   {.data = 1.5}                                                          },
        {"test.string",   {.data = std::string{"text"}}                                          },
    };

    auto encoded = encode_attribute_document(registry, values, AttributeScope::Job, AttributeDocumentMode::Partial);
    REQUIRE(encoded);
    auto const& encoded_values = encoded->as_object().at("values").as_object();
    CHECK(encoded_values.at("test.boolean").as_object().at("type").as_string() == "boolean");
    CHECK(encoded_values.at("test.integer").as_object().at("type").as_string() == "integer");
    CHECK(encoded_values.at("test.number").as_object().at("type").as_string() == "number");
    CHECK(encoded_values.at("test.string").as_object().at("type").as_string() == "string");
    CHECK(encoded_values.at("test.duration").as_object().at("type").as_string() == "duration_ns");
    CHECK(encoded_values.at("test.bytes").as_object().at("type").as_string() == "bytes_hex");
    CHECK(encoded_values.at("test.list").as_object().at("type").as_string() == "list");
    CHECK(encoded_values.at("test.map").as_object().at("type").as_string() == "map");
    CHECK(encoded_values.at("test.bytes").as_object().at("value").as_string() == "00afff");
    CHECK(encoded_values.at("test.duration").as_object().at("value").as_int() == 1500000000);
    auto const& encoded_list = encoded_values.at("test.list").as_object().at("value").as_array();
    CHECK(encoded_list.at(0).as_object().at("type").as_string() == "duration_ns");
    CHECK(encoded_list.at(1).as_object().at("type").as_string() == "bytes_hex");
    auto const& encoded_map = encoded_values.at("test.map").as_object().at("value").as_object();
    CHECK(encoded_map.at("nested").as_object().at("type").as_string() == "list");

    auto text = serialize_json(*encoded);
    REQUIRE(text);
    auto parsed = parse_json(*text);
    REQUIRE(parsed);
    auto decoded = decode_attribute_document(registry, *parsed, AttributeScope::Job, AttributeDocumentMode::Partial);
    REQUIRE(decoded);
    CHECK(same_attribute_set(*decoded, values));
}

TEST_CASE("Attribute persistence keeps partial documents partial and extends materialized documents",
          "[jobu][storage][attribute]")
{
    StandardAttributeRegistry registry;
    AttributeSet              partial{
        {"job.timeout", {.data = 5s}}
    };

    auto encoded_partial =
        encode_attribute_document(registry, partial, AttributeScope::QueueDefault, AttributeDocumentMode::Partial);
    REQUIRE(encoded_partial);
    auto partial_text = serialize_json(*encoded_partial);
    REQUIRE(partial_text);
    CHECK(*partial_text == R"({"values":{"job.timeout":{"type":"duration_ns","value":5000000000}},"version":1})");
    auto decoded_partial = decode_attribute_document(registry,
                                                     *encoded_partial,
                                                     AttributeScope::QueueDefault,
                                                     AttributeDocumentMode::Partial);
    REQUIRE(decoded_partial);
    REQUIRE(decoded_partial->size() == 1U);
    CHECK(std::get<Duration>(decoded_partial->at("job.timeout").data) == 5s);

    auto complete = materialize_attributes(registry, {}, {}, {});
    REQUIRE(complete);
    auto encoded_complete =
        encode_attribute_document(registry, *complete, AttributeScope::Job, AttributeDocumentMode::Materialized);
    REQUIRE(encoded_complete);
    auto& values = std::get<JsonValue::Object>(encoded_complete->data).at("values");
    std::get<JsonValue::Object>(values.data).erase("retry.mode");

    auto decoded_older = decode_attribute_document(registry,
                                                   *encoded_complete,
                                                   AttributeScope::Job,
                                                   AttributeDocumentMode::Materialized);
    REQUIRE(decoded_older);
    REQUIRE(decoded_older->size() == registry.definitions().size());
    CHECK(std::get<std::string>(decoded_older->at("retry.mode").data) == "reschedule");

    auto decoded_as_partial =
        decode_attribute_document(registry, *encoded_complete, AttributeScope::Job, AttributeDocumentMode::Partial);
    REQUIRE(decoded_as_partial);
    CHECK_FALSE(decoded_as_partial->contains("retry.mode"));

    complete->erase("retry.mode");
    auto incomplete =
        encode_attribute_document(registry, *complete, AttributeScope::Job, AttributeDocumentMode::Materialized);
    REQUIRE_FALSE(incomplete);
    CHECK(incomplete.error().code == "jobu.attribute.invalid_value");
}

TEST_CASE("Attribute persistence rejects malformed typed documents with a stable internal error",
          "[jobu][storage][attribute]")
{
    CodecAttributeRegistry registry;
    auto                   decode = [&registry](JsonValue document) {
        return decode_attribute_document(registry, document, AttributeScope::Job, AttributeDocumentMode::Partial);
    };

    auto unknown_type = decode(attribute_document({
        {"test.integer", typed_json("unknown", json(std::int64_t{1}))}
    }));
    REQUIRE_FALSE(unknown_type);
    CHECK(unknown_type.error().category == ErrorCategory::Internal);
    CHECK(unknown_type.error().code == "jobu.attribute.invalid_document");
    CHECK(unknown_type.error().detail == "reason=unknown_type_tag");

    for (auto document : {
             attribute_document(
                 {{"test.integer",
                   typed_json("integer", json(std::uint64_t{std::numeric_limits<std::uint64_t>::max()}))}    }
                 ),
             attribute_document(
                 {{"test.duration",
                   typed_json("duration_ns", json(std::uint64_t{std::numeric_limits<std::uint64_t>::max()}))}}
                 ),
             attribute_document({{"test.bytes", typed_json("bytes_hex", json(std::string{"AA"}))}                                             }
                 ),
             attribute_document({{"unknown.value", typed_json("integer", json(std::int64_t{1}))}                                              }
                 ),
             attribute_document({                                                                                                            },
                 2),
    }) {
        auto result = decode(std::move(document));
        REQUIRE_FALSE(result);
        CHECK(result.error().category == ErrorCategory::Internal);
        CHECK(result.error().code == "jobu.attribute.invalid_document");
    }

    auto extra_member = typed_json("integer", json(std::int64_t{1}));
    std::get<JsonValue::Object>(extra_member.data).emplace("extra", json(true));
    auto extra_result = decode(attribute_document({
        {"test.integer", std::move(extra_member)}
    }));
    REQUIRE_FALSE(extra_result);
    CHECK(extra_result.error().code == "jobu.attribute.invalid_document");

    auto too_deep = decode(attribute_document({
        {"test.list", nested_typed_list(65U)}
    }));
    REQUIRE_FALSE(too_deep);
    CHECK(too_deep.error().code == "jobu.attribute.invalid_document");
}

TEST_CASE("Materialized attribute decoding revalidates standard cross-field rules", "[jobu][storage][attribute]")
{
    StandardAttributeRegistry registry;
    AttributeSet              invalid{
        {"retry.initial_delay", {.data = 2h}},
        {"retry.max_delay",     {.data = 1h}},
    };
    auto partial = encode_attribute_document(registry, invalid, AttributeScope::Job, AttributeDocumentMode::Partial);
    REQUIRE(partial);

    auto decoded =
        decode_attribute_document(registry, *partial, AttributeScope::Job, AttributeDocumentMode::Materialized);
    REQUIRE_FALSE(decoded);
    CHECK(decoded.error().category == ErrorCategory::Internal);
    CHECK(decoded.error().code == "jobu.attribute.invalid_document");
}

TEST_CASE("Attribute persistence limits manually constructed recursive trees", "[jobu][storage][attribute]")
{
    CodecAttributeRegistry registry;
    auto                   value = AttributeValue{.data = std::int64_t{1}};
    for (std::size_t level = 0; level < 65U; ++level) {
        value = AttributeValue{.data = AttributeValue::List{std::move(value)}};
    }

    auto encoded = encode_attribute_document(registry,
                                             {
                                                 {"test.list", std::move(value)}
    },
                                             AttributeScope::Job,
                                             AttributeDocumentMode::Partial);
    REQUIRE_FALSE(encoded);
    CHECK(encoded.error().code == "jobu.attribute.invalid_value");
}
