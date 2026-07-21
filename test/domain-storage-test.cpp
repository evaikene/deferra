#include "domain_storage_priv.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
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

    auto const invalid =
        read_timestamp(record("created_at_us", std::numeric_limits<std::int64_t>::min()), "created_at_us");
    REQUIRE_FALSE(invalid);
    CHECK(invalid.error().code == "jobu.storage.invalid_time");
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

TEST_CASE("Domain storage handles required and optional text", "[jobu][storage]")
{
    CHECK(*read_text(record("name", std::string{"queue"}), "name") == "queue");
    CHECK(*read_optional_text(record("name", std::string{"job"}), "name") == std::optional<std::string>{"job"});
    CHECK_FALSE(read_optional_text(record("name", Null{}), "name")->has_value());
    CHECK(read_text(record("name", std::int64_t{1}), "name").error().code == "jobu.storage.invalid_text");
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
}
