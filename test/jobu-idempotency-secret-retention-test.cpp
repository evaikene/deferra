#include "idempotency_repository_priv.hpp"
#include "retention_repository_priv.hpp"
#include "secret_repository_priv.hpp"

#include "attribute_registry.hpp"
#include "database.hpp"
#include "domain_storage_priv.hpp"
#include "query.hpp"
#include "sqlite/sqlite_driver.hpp"
#include "sqlite/sqlite_schema.hpp"
#include "support/temporary_directory.hpp"
#include "transaction.hpp"
#include "value.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace jb::core;
using namespace jb::db;
using namespace jb::jobu;
using namespace jb::jobu::detail;
using namespace jb::test;
using namespace std::chrono_literals;

namespace {

auto id(std::uint8_t suffix) -> Uuid
{
    auto bytes = Uuid::Storage{};
    bytes[6]   = std::byte{0x70};
    bytes[8]   = std::byte{0x80};
    bytes[15]  = static_cast<std::byte>(suffix);
    return Uuid{bytes};
}

auto make_database(std::filesystem::path database_file) -> Database
{
    return Database{std::make_unique<jb::db::sqlite::Driver>(jb::db::sqlite::Options{
        .database_file = std::move(database_file),
        .busy_timeout  = 1000ms,
        .durability    = jb::db::sqlite::Durability::Normal,
    })};
}

struct RepositoryFixture {
    RepositoryFixture()
        : idempotency{database}
        , secrets{database}
        , retention{database, registry}
    {
        REQUIRE(database.open());
        REQUIRE(jb::jobu::sqlite::ensure_schema(database));
    }

    TemporaryDirectory        directory;
    std::filesystem::path     database_file{directory.path() / "jobu.sqlite"};
    Database                  database{make_database(database_file)};
    StandardAttributeRegistry registry;
    IdempotencyRepository     idempotency;
    SecretRepository          secrets;
    RetentionRepository       retention;
};

auto require_error(auto const& result, ErrorCategory category, std::string_view code) -> Error
{
    REQUIRE_FALSE(result);
    CHECK(result.error().category == category);
    CHECK(result.error().code == code);
    return result.error();
}

auto scalar_count(Database& database, std::string_view table) -> std::int64_t
{
    Query query{database};
    REQUIRE(query.exec("SELECT COUNT(*) AS row_count FROM " + std::string{table}));
    REQUIRE(query.next());
    auto const* value = query.value("row_count");
    auto const* count = value == nullptr ? nullptr : std::get_if<std::int64_t>(value);
    REQUIRE(count != nullptr);
    return *count;
}

auto stored_secret(Database& database, std::string_view name) -> ByteBuffer
{
    Query query{database};
    REQUIRE(query.prepare("SELECT value_blob FROM jobu_secrets WHERE name = :name"));
    REQUIRE(query.bind_value(":name", make_text(name)));
    REQUIRE(query.exec());
    REQUIRE(query.next());
    auto const* value = query.value("value_blob");
    auto const* bytes = value == nullptr ? nullptr : std::get_if<ByteBuffer>(value);
    REQUIRE(bytes != nullptr);
    return *bytes;
}

void insert_queue(Database& database, Uuid const& queue_id, std::string_view name, bool deleted = false)
{
    Query query{database};
    REQUIRE(query.prepare(
        "INSERT INTO jobu_queues(id, name, deleted_name, state, weight, concurrency_limit, recovery_policy, "
        "defaults_json, retention_seconds, runnable_wait_warning_ms, created_at_us, updated_at_us, deleted_at_us) "
        "VALUES(:id, :name, :deleted_name, :state, 1, 1, 'fail_interrupted', "
        "'{\"version\":1,\"values\":{}}', NULL, 10000, 0, :updated_at, :deleted_at)"));
    REQUIRE(query.bind_value(":id", uuid_to_storage(queue_id)));
    REQUIRE(query.bind_value(
        ":name",
        make_text(deleted ? std::string{name} + "-deleted#" + queue_id.to_string() : std::string{name})));
    REQUIRE(query.bind_value(":deleted_name", deleted ? make_text(name) : Value{Null{}}));
    REQUIRE(query.bind_value(":state", make_text(deleted ? "deleted" : "active")));
    REQUIRE(query.bind_value(":updated_at", std::int64_t{deleted ? 10 : 0}));
    REQUIRE(query.bind_value(":deleted_at", deleted ? Value{std::int64_t{10}} : Value{Null{}}));
    REQUIRE(query.exec());
}

void insert_job(Database& database, Uuid const& job_id, Uuid const& queue_id, bool deleted = false)
{
    Query query{database};
    REQUIRE(query.prepare(
        "INSERT INTO jobu_jobs(id, queue_id, revision, name, state, type, schedule_kind, scheduled_at_us, "
        "cron_expression, cron_timezone, priority, attributes_json, payload_json, created_at_us, updated_at_us, "
        "deleted_at_us) VALUES(:id, :queue_id, 1, NULL, :state, 'cli', 'once', 0, NULL, NULL, 0, "
        "'{\"version\":1,\"values\":{}}', '{\"command\":\"true\"}', 0, :updated_at, :deleted_at)"));
    REQUIRE(query.bind_value(":id", uuid_to_storage(job_id)));
    REQUIRE(query.bind_value(":queue_id", uuid_to_storage(queue_id)));
    REQUIRE(query.bind_value(":state", make_text(deleted ? "deleted" : "active")));
    REQUIRE(query.bind_value(":updated_at", std::int64_t{deleted ? 10 : 0}));
    REQUIRE(query.bind_value(":deleted_at", deleted ? Value{std::int64_t{10}} : Value{Null{}}));
    REQUIRE(query.exec());
}

void insert_terminal_run(Database&    database,
                         Uuid const&  run_id,
                         Uuid const&  job_id,
                         Uuid const&  queue_id,
                         std::int64_t completed_at)
{
    Query query{database};
    REQUIRE(query.prepare(
        "INSERT INTO jobu_runs(id, job_id, job_revision, queue_id, origin, schedule_owned, planned_at_us, "
        "runnable_at_us, started_at_us, completed_at_us, type, priority, attributes_json, payload_json, state, "
        "result_json) VALUES(:id, :job_id, 1, :queue_id, 'scheduled', 1, 0, 0, 1, :completed_at, 'cli', 0, "
        "'{\"version\":1,\"values\":{}}', '{\"command\":\"true\"}', 'succeeded', '{}')"));
    REQUIRE(query.bind_value(":id", uuid_to_storage(run_id)));
    REQUIRE(query.bind_value(":job_id", uuid_to_storage(job_id)));
    REQUIRE(query.bind_value(":queue_id", uuid_to_storage(queue_id)));
    REQUIRE(query.bind_value(":completed_at", completed_at));
    REQUIRE(query.exec());
}

void insert_attempt_output(Database& database, Uuid const& run_id)
{
    Query attempt{database};
    REQUIRE(attempt.prepare(
        "INSERT INTO jobu_attempts(run_id, attempt_number, due_at_us, started_at_us, completed_at_us, state, "
        "outcome, result_json) VALUES(:run_id, 1, 0, 1, 2, 'completed', 'succeeded', '{}')"));
    REQUIRE(attempt.bind_value(":run_id", uuid_to_storage(run_id)));
    REQUIRE(attempt.exec());

    Query output{database};
    REQUIRE(output.prepare(
        "INSERT INTO jobu_attempt_output(run_id, attempt_number, stdout_blob, stderr_blob, stdout_truncated, "
        "stderr_truncated, capture_lost) VALUES(:run_id, 1, X'0001', NULL, 0, 0, 0)"));
    REQUIRE(output.bind_value(":run_id", uuid_to_storage(run_id)));
    REQUIRE(output.exec());
}

auto record(std::string                 method,
            Uuid                        scope_id,
            std::string                 key,
            Uuid                        resource_id,
            UtcTimePoint                created_at,
            std::optional<UtcTimePoint> expires_at = std::nullopt) -> IdempotencyRecord
{
    return {
        .method       = std::move(method),
        .scope_id     = scope_id,
        .key          = std::move(key),
        .request_json = "{\"request\":true}",
        .result_json  = "{\"result\":true}",
        .resource_id  = resource_id,
        .created_at   = created_at,
        .expires_at   = expires_at,
    };
}

} // anonymous namespace

TEST_CASE("Idempotency repository round-trips scoped records and bounded cleanup", "[jobu][idempotency][sqlite]")
{
    RepositoryFixture fixture;
    auto const        queue_id = id(1);
    auto const        job_id   = id(2);

    REQUIRE(fixture.idempotency.insert(record("queue.create", Uuid{}, "shared-key", queue_id, UtcTimePoint{1s})));
    REQUIRE(fixture.idempotency.insert(
        record("job.create", queue_id, "shared-key", job_id, UtcTimePoint{2s}, UtcTimePoint{5s})));

    auto queue_record = fixture.idempotency.find("queue.create", Uuid{}, "shared-key");
    REQUIRE(queue_record);
    REQUIRE(queue_record->has_value());
    CHECK((*queue_record)->resource_id == queue_id);
    CHECK((*queue_record)->request_json == "{\"request\":true}");
    CHECK_FALSE((*queue_record)->expires_at);

    auto job_record = fixture.idempotency.find("job.create", queue_id, "shared-key");
    REQUIRE(job_record);
    REQUIRE(job_record->has_value());
    CHECK((*job_record)->resource_id == job_id);
    CHECK((*job_record)->expires_at == UtcTimePoint{5s});
    REQUIRE(fixture.idempotency.find("job.create", id(99), "shared-key"));
    CHECK_FALSE(fixture.idempotency.find("job.create", id(99), "shared-key")->has_value());

    require_error(fixture.idempotency.insert(record("queue.create", Uuid{}, "shared-key", id(3), UtcTimePoint{3s})),
                  ErrorCategory::Conflict,
                  "jobu.idempotency.conflict");
    auto expired = fixture.idempotency.erase_expired(UtcTimePoint{10s}, 1);
    REQUIRE(expired);
    CHECK(*expired == 1);
    REQUIRE(fixture.idempotency.find("job.create", queue_id, "shared-key"));
    CHECK_FALSE(fixture.idempotency.find("job.create", queue_id, "shared-key")->has_value());
    CHECK(*fixture.idempotency.erase_for_resource(queue_id) == 1);
    CHECK(scalar_count(fixture.database, "jobu_idempotency") == 0);

    require_error(fixture.idempotency.erase_expired(UtcTimePoint{10s}, 0),
                  ErrorCategory::InvalidArgument,
                  "jobu.storage.invalid_limit");
    require_error(fixture.idempotency.erase_expired(UtcTimePoint{10s}, 1001),
                  ErrorCategory::InvalidArgument,
                  "jobu.storage.invalid_limit");
}

TEST_CASE("Secret repository preserves private bytes metadata and atomic references", "[jobu][secret][sqlite]")
{
    RepositoryFixture fixture;
    auto const        queue_id = id(10);
    auto const        job_id   = id(11);
    insert_queue(fixture.database, queue_id, "secrets");
    insert_job(fixture.database, job_id, queue_id);

    auto first_value = ByteBuffer{std::byte{0x00}, std::byte{0x7f}, std::byte{0xff}};
    auto first       = fixture.secrets.set("alpha.token", first_value, UtcTimePoint{10s});
    REQUIRE(first);
    CHECK(first->created_at == UtcTimePoint{10s});
    CHECK(first->updated_at == UtcTimePoint{10s});
    CHECK(stored_secret(fixture.database, "alpha.token") == first_value);

    auto second_value = ByteBuffer{std::byte{0x42}, std::byte{0x00}};
    auto updated      = fixture.secrets.set("alpha.token", second_value, UtcTimePoint{20s});
    REQUIRE(updated);
    CHECK(updated->created_at == UtcTimePoint{10s});
    CHECK(updated->updated_at == UtcTimePoint{20s});
    CHECK(stored_secret(fixture.database, "alpha.token") == second_value);
    REQUIRE(fixture.secrets.set("beta", ByteBuffer{std::byte{0x01}}, UtcTimePoint{11s}));

    auto metadata = fixture.secrets.list_metadata(1);
    REQUIRE(metadata);
    REQUIRE(metadata->size() == 1);
    CHECK(metadata->front().name == "alpha.token");
    metadata = fixture.secrets.list_metadata(2, "alpha.token");
    REQUIRE(metadata);
    REQUIRE(metadata->size() == 1);
    CHECK(metadata->front().name == "beta");

    auto begun = Transaction::begin(fixture.database);
    REQUIRE(begun);
    auto transaction = std::move(begun).value();
    auto references  = std::vector<SecretReference>{
        {.secret_name = "alpha.token", .field_path = "payload.token"},
        {.secret_name = "beta",        .field_path = "payload.other"},
    };
    REQUIRE(fixture.secrets.replace_references_for_job(job_id, references));
    REQUIRE(transaction.commit());
    CHECK(*fixture.secrets.reference_count("alpha.token") == 1);
    CHECK(*fixture.secrets.reference_count("beta") == 1);
    require_error(fixture.secrets.erase("alpha.token"), ErrorCategory::Conflict, "jobu.secret.in_use");

    {
        auto rollback_begin = Transaction::begin(fixture.database);
        REQUIRE(rollback_begin);
        auto rollback = std::move(rollback_begin).value();
        auto missing  = std::vector<SecretReference>{
            {.secret_name = "missing", .field_path = "payload.missing"}
        };
        require_error(fixture.secrets.replace_references_for_job(job_id, missing),
                      ErrorCategory::Conflict,
                      "db.constraint.foreign_key");
    }
    CHECK(*fixture.secrets.reference_count("alpha.token") == 1);

    auto clear_begin = Transaction::begin(fixture.database);
    REQUIRE(clear_begin);
    auto clear_transaction = std::move(clear_begin).value();
    REQUIRE(fixture.secrets.replace_references_for_job(job_id, {}));
    REQUIRE(clear_transaction.commit());
    REQUIRE(fixture.secrets.erase("alpha.token"));
    require_error(fixture.secrets.erase("alpha.token"), ErrorCategory::NotFound, "jobu.secret.not_found");
    require_error(fixture.secrets.set("Bad.Name", {}, UtcTimePoint{1s}),
                  ErrorCategory::InvalidArgument,
                  "jobu.secret.invalid_name");
    require_error(fixture.secrets.list_metadata(0), ErrorCategory::InvalidArgument, "jobu.storage.invalid_limit");
}

TEST_CASE("Retention repository purges bounded history and guards durable references", "[jobu][retention][sqlite]")
{
    RepositoryFixture fixture;
    auto const        first_queue   = id(20);
    auto const        first_job     = id(21);
    auto const        first_run     = id(22);
    auto const        second_queue  = id(30);
    auto const        second_job    = id(31);
    auto const        second_run    = id(32);
    auto const        guarded_queue = id(40);

    insert_queue(fixture.database, first_queue, "first", true);
    insert_job(fixture.database, first_job, first_queue, true);
    insert_terminal_run(fixture.database, first_run, first_job, first_queue, 5);
    insert_attempt_output(fixture.database, first_run);
    REQUIRE(fixture.idempotency.insert(
        record("job.create", first_queue, "first-key", first_job, UtcTimePoint{1s}, UtcTimePoint{2s})));

    insert_queue(fixture.database, second_queue, "second", true);
    insert_job(fixture.database, second_job, second_queue, true);
    insert_terminal_run(fixture.database, second_run, second_job, second_queue, 6);
    REQUIRE(fixture.idempotency.insert(
        record("job.create", second_queue, "second-key", second_job, UtcTimePoint{2s}, UtcTimePoint{3s})));

    insert_queue(fixture.database, guarded_queue, "guarded", true);
    REQUIRE(fixture.idempotency.insert(
        record("job.create", guarded_queue, "guard-key", id(41), UtcTimePoint{3s}, std::nullopt)));

    auto first = fixture.retention.purge_batch(UtcTimePoint{10s}, 1);
    REQUIRE(first);
    CHECK(first->runs == 1);
    CHECK(first->idempotency_records == 1);
    CHECK(first->jobs == 1);
    CHECK(first->queues == 1);
    CHECK(scalar_count(fixture.database, "jobu_attempts") == 0);
    CHECK(scalar_count(fixture.database, "jobu_attempt_output") == 0);
    CHECK(scalar_count(fixture.database, "jobu_runs") == 1);
    CHECK(scalar_count(fixture.database, "jobu_jobs") == 1);
    CHECK(scalar_count(fixture.database, "jobu_queues") == 2);

    auto second = fixture.retention.purge_batch(UtcTimePoint{10s}, 1000);
    REQUIRE(second);
    CHECK(second->runs == 1);
    CHECK(second->idempotency_records == 1);
    CHECK(second->jobs == 1);
    CHECK(second->queues == 1);
    CHECK(scalar_count(fixture.database, "jobu_runs") == 0);
    CHECK(scalar_count(fixture.database, "jobu_jobs") == 0);
    CHECK(scalar_count(fixture.database, "jobu_queues") == 1);
    CHECK(scalar_count(fixture.database, "jobu_idempotency") == 1);

    require_error(fixture.retention.purge_batch(UtcTimePoint{10s}, 0),
                  ErrorCategory::InvalidArgument,
                  "jobu.storage.invalid_limit");
    require_error(fixture.retention.purge_batch(UtcTimePoint{10s}, 1001),
                  ErrorCategory::InvalidArgument,
                  "jobu.storage.invalid_limit");
}

TEST_CASE("Retention repository rolls the entire purge batch back on failure", "[jobu][retention][rollback]")
{
    RepositoryFixture fixture;
    auto const        queue_id = id(50);
    auto const        job_id   = id(51);
    auto const        run_id   = id(52);
    insert_queue(fixture.database, queue_id, "rollback", true);
    insert_job(fixture.database, job_id, queue_id, true);
    insert_terminal_run(fixture.database, run_id, job_id, queue_id, 5);
    insert_attempt_output(fixture.database, run_id);
    REQUIRE(fixture.idempotency.insert(
        record("job.create", queue_id, "rollback-key", job_id, UtcTimePoint{1s}, UtcTimePoint{2s})));
    {
        Query query{fixture.database};
        REQUIRE(query.exec("CREATE TRIGGER fail_retention_job_delete BEFORE DELETE ON jobu_jobs "
                           "BEGIN SELECT RAISE(ABORT, 'injected failure'); END"));
    }

    require_error(fixture.retention.purge_batch(UtcTimePoint{10s}, 10), ErrorCategory::Conflict, "db.constraint");
    CHECK(scalar_count(fixture.database, "jobu_runs") == 1);
    CHECK(scalar_count(fixture.database, "jobu_attempts") == 1);
    CHECK(scalar_count(fixture.database, "jobu_attempt_output") == 1);
    CHECK(scalar_count(fixture.database, "jobu_jobs") == 1);
    CHECK(scalar_count(fixture.database, "jobu_queues") == 1);
    CHECK(scalar_count(fixture.database, "jobu_idempotency") == 1);
}
