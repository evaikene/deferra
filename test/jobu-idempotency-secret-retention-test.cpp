#include "idempotency_codec_priv.hpp"
#include "idempotency_repository_priv.hpp"
#include "retention_repository_priv.hpp"
#include "secret_repository_priv.hpp"

#include "attribute_registry.hpp"
#include "database.hpp"
#include "domain_storage_priv.hpp"
#include "query.hpp"
#include "sqlite/sqlite_driver.hpp"
#include "sqlite/sqlite_schema.hpp"
#include "storage_failure_priv.hpp"
#include "support/catch_utils.hpp" // IWYU pragma: keep for Catch::StringMaker specializations
#include "support/fault_database_driver.hpp"
#include "support/storage_fault_helpers.hpp"
#include "support/temporary_directory.hpp"
#include "transaction.hpp"
#include "value.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

using namespace jb::core;
using namespace jb::db;
using namespace jb::jobu;
using namespace jb::jobu::detail;
using namespace jb::test;
using namespace std::chrono_literals;

namespace {

auto id(std::uint16_t suffix) -> Uuid
{
    auto bytes = Uuid::Storage{};
    bytes[6]   = std::byte{0x70};
    bytes[8]   = std::byte{0x80};
    bytes[14]  = static_cast<std::byte>(suffix >> 8U);
    bytes[15]  = static_cast<std::byte>(suffix & 0xffU);
    return Uuid{bytes};
}

auto make_database(std::filesystem::path database_file, std::shared_ptr<DatabaseFaultState> faults) -> Database
{
    return Database{
        std::make_unique<FaultDatabaseDriver>(std::make_unique<jb::db::sqlite::Driver>(jb::db::sqlite::Options{
                                                  .database_file = std::move(database_file),
                                                  .busy_timeout  = 1000ms,
                                                  .durability    = jb::db::sqlite::Durability::Normal,
                                              }),
                                              std::move(faults))};
}

struct RepositoryFixture {
    RepositoryFixture()
        : idempotency{database}
        , secrets{database}
        , retention{database, registry}
    {
        REQUIRE(database.open());
        REQUIRE(jb::jobu::sqlite::ensure_schema(database));
        faults->classify = [sql = prepared_sql](std::string_view text) {
            sql->emplace_back(text);
            if (text.starts_with("SELECT id AS queue_id, retention_seconds")) {
                return std::string{"retention.policy"};
            }
            if (text.starts_with("SELECT id AS run_id, completed_at_us")) {
                return std::string{text.find(":after_completed") == std::string_view::npos ? "retention.first"
                                                                                           : "retention.continuation"};
            }
            return std::string{text.starts_with("DELETE FROM jobu_runs") ? "retention.delete" : "other"};
        };
    }

    TemporaryDirectory                        directory;
    std::filesystem::path                     database_file{directory.path() / "jobu.sqlite"};
    std::shared_ptr<DatabaseFaultState>       faults{std::make_shared<DatabaseFaultState>()};
    std::shared_ptr<std::vector<std::string>> prepared_sql{std::make_shared<std::vector<std::string>>()};
    Database                                  database{make_database(database_file, faults)};
    StandardAttributeRegistry                 registry;
    IdempotencyRepository                     idempotency;
    SecretRepository                          secrets;
    RetentionRepository                       retention;
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

void insert_queue(Database&                           database,
                  Uuid const&                         queue_id,
                  std::string_view                    name,
                  bool                                deleted   = false,
                  std::optional<std::chrono::seconds> retention = std::nullopt)
{
    Query query{database};
    REQUIRE(query.prepare(
        "INSERT INTO jobu_queues(id, name, deleted_name, state, weight, concurrency_limit, recovery_policy, "
        "defaults_json, retention_seconds, runnable_wait_warning_ms, created_at_us, updated_at_us, deleted_at_us) "
        "VALUES(:id, :name, :deleted_name, :state, 1, 1, 'fail_interrupted', "
        "'{\"version\":1,\"values\":{}}', :retention, 10000, 0, :updated_at, :deleted_at)"));
    REQUIRE(query.bind_value(":id", uuid_to_storage(queue_id)));
    REQUIRE(query.bind_value(":retention", retention ? Value{retention->count()} : Value{Null{}}));
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
        "'{\"version\":1,\"values\":{}}', '{\"command\":\"/true\"}', 0, :updated_at, :deleted_at)"));
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
    auto transaction = Transaction::begin(database);
    REQUIRE(transaction);

    Query query{database};
    REQUIRE(query.prepare(
        "INSERT INTO jobu_runs(id, job_id, job_revision, queue_id, origin, schedule_owned, planned_at_us, "
        "runnable_at_us, started_at_us, completed_at_us, type, priority, attributes_json, payload_json, state, "
        "result_json) VALUES(:id, :job_id, 1, :queue_id, 'scheduled', 1, 0, 0, 1, :completed_at, 'cli', 0, "
        "'{\"version\":1,\"values\":{}}', '{\"command\":\"/true\"}', 'succeeded', '{}')"));
    REQUIRE(query.bind_value(":id", uuid_to_storage(run_id)));
    REQUIRE(query.bind_value(":job_id", uuid_to_storage(job_id)));
    REQUIRE(query.bind_value(":queue_id", uuid_to_storage(queue_id)));
    REQUIRE(query.bind_value(":completed_at", completed_at));
    REQUIRE(query.exec());

    REQUIRE(query.prepare("INSERT INTO jobu_run_timing(run_id) VALUES(:id)"));
    REQUIRE(query.bind_value(":id", uuid_to_storage(run_id)));
    REQUIRE(query.exec());
    REQUIRE(query.finish());
    REQUIRE(transaction->commit());
}

void insert_attempt_output(Database& database, Uuid const& run_id, std::int64_t number = 1, std::size_t bytes = 2)
{
    Query attempt{database};
    REQUIRE(attempt.prepare(
        "INSERT INTO jobu_attempts(run_id, attempt_number, due_at_us, started_at_us, completed_at_us, state, "
        "outcome, result_json) VALUES(:run_id, :number, 0, 1, 2, 'completed', 'succeeded', '{}')"));
    REQUIRE(attempt.bind_value(":run_id", uuid_to_storage(run_id)));
    REQUIRE(attempt.bind_value(":number", number));
    REQUIRE(attempt.exec());

    Query output{database};
    REQUIRE(output.prepare(
        "INSERT INTO jobu_attempt_output(run_id, attempt_number, stdout_blob, stderr_blob, stdout_truncated, "
        "stderr_truncated, capture_lost) VALUES(:run_id, :number, :bytes, NULL, 0, 0, 0)"));
    REQUIRE(output.bind_value(":run_id", uuid_to_storage(run_id)));
    REQUIRE(output.bind_value(":number", number));
    REQUIRE(output.bind_value(":bytes", ByteBuffer(bytes, std::byte{0x01})));
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

auto cleanup_record(RepositoryFixture& fixture, std::string method, Uuid scope, std::string key, Uuid resource)
    -> IdempotencyRecord
{
    auto payload = parse_json(R"({"command":"/true"})");
    REQUIRE(payload);
    auto attributes = materialize_attributes(fixture.registry, {}, {}, {});
    REQUIRE(attributes);
    auto request = Result<std::string, Error>{};
    auto result  = Result<std::string, Error>{};
    if (method == "job.create") {
        auto input = CreateJobRequest{.queue = scope, .schedule = OnceSchedule{UtcTimePoint{}}, .payload = *payload};
        request    = encode_job_create_idempotency_request(input, scope, fixture.registry);
        auto job   = JobDefinition{.id         = resource,
                                   .queue_id   = scope,
                                   .schedule   = OnceSchedule{UtcTimePoint{}},
                                   .attributes = *attributes,
                                   .payload    = *payload};
        result     = encode_job_idempotency_result(job, fixture.registry);
    }
    else {
        request = encode_run_now_idempotency_request({.job_id = scope});
        Query query{fixture.database};
        REQUIRE(query.prepare("SELECT queue_id FROM jobu_runs WHERE id = :id"));
        REQUIRE(query.bind_value(":id", uuid_to_storage(resource)));
        REQUIRE(query.exec());
        REQUIRE(query.next());
        auto queue = read_uuid(query.record(), "queue_id");
        REQUIRE(queue);
        auto run = JobRun{.id             = resource,
                          .job_id         = scope,
                          .job_revision   = 1,
                          .queue_id       = *queue,
                          .origin         = RunOrigin::Manual,
                          .schedule_owned = false,
                          .attributes     = *attributes,
                          .payload        = *payload};
        result   = encode_run_now_idempotency_result(run, fixture.registry);
    }
    REQUIRE(request);
    REQUIRE(result);
    return {.method       = std::move(method),
            .scope_id     = scope,
            .key          = std::move(key),
            .request_json = *request,
            .result_json  = *result,
            .resource_id  = resource};
}

void execute(Database& database, std::string_view sql)
{
    Query query{database};
    REQUIRE(query.exec(sql));
}

auto has_run(Database& database, Uuid const& run_id) -> bool
{
    Query query{database};
    REQUIRE(query.prepare("SELECT id FROM jobu_runs WHERE id = :id"));
    REQUIRE(query.bind_value(":id", uuid_to_storage(run_id)));
    REQUIRE(query.exec());
    auto next = query.next();
    REQUIRE(next);
    return *next;
}

void check_run_only_counts(RetentionBatchResult const& result, std::size_t runs)
{
    CHECK(result.purged.runs == runs);
    CHECK(result.purged.idempotency_records == 0);
    CHECK(result.purged.jobs == 0);
    CHECK(result.purged.queues == 0);
}

auto visit(RepositoryFixture&   fixture,
           RetentionSweepCursor cursor    = {},
           UtcTimePoint         now       = UtcTimePoint{20s},
           std::chrono::seconds retention = 10s,
           std::size_t          limit     = 100) -> RetentionBatchResult
{
    auto result = fixture.retention.purge_next_batch(now, retention, limit, cursor);
    REQUIRE(result);
    return *result;
}

auto finish_sweep(RepositoryFixture& fixture, RetentionSweepCursor cursor = {}) -> RetentionBatchResult
{
    for (std::size_t visits = 0; visits < 1000; ++visits) {
        auto result = visit(fixture, cursor);
        if (result.sweep_complete) {
            return result;
        }
        cursor = result.next;
    }
    FAIL("Retention sweep did not finish");
    return {};
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
    auto page = fixture.idempotency.list_referencing(queue_id, 1);
    REQUIRE(page);
    REQUIRE(page->size() == 1);
    CHECK(page->front().method == "job.create");
    auto const after =
        IdempotencyKey{.method = page->front().method, .scope_id = page->front().scope_id, .key = page->front().key};
    auto continuation = fixture.idempotency.list_referencing(queue_id, 1, after);
    REQUIRE(continuation);
    REQUIRE(continuation->size() == 1);
    CHECK(continuation->front().method == "queue.create");

    auto mismatch        = page->front();
    mismatch.resource_id = id(99);
    CHECK(*fixture.idempotency.erase_matching(mismatch) == 0);
    CHECK(*fixture.idempotency.erase_matching(page->front()) == 1);
    CHECK(*fixture.idempotency.erase_matching(continuation->front()) == 1);
    CHECK(scalar_count(fixture.database, "jobu_idempotency") == 0);
    require_error(fixture.idempotency.list_referencing(queue_id, 0),
                  ErrorCategory::InvalidArgument,
                  "jobu.storage.invalid_limit");
    require_error(fixture.idempotency.list_referencing(queue_id, 1001),
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

TEST_CASE("Retention visits inherited finite unlimited and deleted queue policies", "[jobu][retention][sqlite]")
{
    RepositoryFixture fixture;
    insert_queue(fixture.database, id(1), "inherited");
    insert_queue(fixture.database, id(2), "finite", false, 5s);
    insert_queue(fixture.database, id(3), "unlimited", false, 0s);
    insert_queue(fixture.database, id(4), "deleted", true, 1s);
    for (std::uint16_t number = 1; number <= 4; ++number) {
        insert_job(fixture.database, id(number + 10), id(number), number == 4);
        insert_terminal_run(fixture.database, id(number + 20), id(number + 10), id(number), 14'000'000);
    }
    REQUIRE(fixture.idempotency.insert(cleanup_record(fixture, "job.create", id(4), "retained-key", id(14))));
    execute(fixture.database,
            "UPDATE jobu_runs SET origin = 'manual', schedule_owned = 0 "
            "WHERE id = X'00000000000070008000000000000018'");
    REQUIRE(fixture.idempotency.insert(cleanup_record(fixture, "job.run_now", id(14), "manual-key", id(24))));
    execute(fixture.database,
            "UPDATE jobu_jobs SET state = 'succeeded' "
            "WHERE id = X'0000000000007000800000000000000c'");

    auto first = visit(fixture);
    check_run_only_counts(first, 0);
    auto second = visit(fixture, first.next);
    check_run_only_counts(second, 1);
    auto third = visit(fixture, second.next);
    check_run_only_counts(third, 0);
    auto fourth = visit(fixture, third.next);
    CHECK(fourth.purged.runs == 1);
    CHECK(fourth.purged.idempotency_records == 1);
    auto end = finish_sweep(fixture, fourth.next);
    CHECK(end.sweep_complete);
    CHECK_FALSE(end.next.after_queue);
    CHECK(has_run(fixture.database, id(21)));
    CHECK_FALSE(has_run(fixture.database, id(22)));
    CHECK(has_run(fixture.database, id(23)));
    CHECK_FALSE(has_run(fixture.database, id(24)));
    CHECK(scalar_count(fixture.database, "jobu_run_timing") == 2);
    CHECK(scalar_count(fixture.database, "jobu_jobs") == 3);
    CHECK(scalar_count(fixture.database, "jobu_queues") == 3);
    CHECK(scalar_count(fixture.database, "jobu_idempotency") == 0);
}

TEST_CASE("Unlimited daemon retention still visits finite overrides and empty queues", "[jobu][retention]")
{
    RepositoryFixture fixture;
    insert_queue(fixture.database, id(1), "empty");
    insert_queue(fixture.database, id(2), "inherited");
    insert_queue(fixture.database, id(3), "finite", false, 1s);
    for (std::uint16_t number = 2; number <= 3; ++number) {
        insert_job(fixture.database, id(number + 10), id(number));
        insert_terminal_run(fixture.database, id(number + 20), id(number + 10), id(number), 1);
    }
    auto first = visit(fixture, {}, UtcTimePoint{20s}, 0s);
    CHECK(first.next.after_queue == id(1));
    check_run_only_counts(first, 0);
    auto second = visit(fixture, first.next, UtcTimePoint{20s}, 0s);
    CHECK(second.next.after_queue == id(2));
    check_run_only_counts(second, 0);
    auto third = visit(fixture, second.next, UtcTimePoint{20s}, 0s);
    check_run_only_counts(third, 1);
    CHECK(has_run(fixture.database, id(22)));
    CHECK_FALSE(has_run(fixture.database, id(23)));
}

TEST_CASE("Retention uses strict completion time for every terminal state", "[jobu][retention]")
{
    auto const*       state = GENERATE("succeeded", "failed", "interrupted", "cancelled");
    RepositoryFixture fixture;
    insert_queue(fixture.database, id(1), "history");
    insert_job(fixture.database, id(2), id(1));
    for (std::uint16_t number = 3; number <= 5; ++number) {
        insert_terminal_run(fixture.database, id(number), id(2), id(1), 9'999'996 + number);
    }
    execute(fixture.database, "UPDATE jobu_runs SET state = '" + std::string{state} + "'");
    auto result = visit(fixture);
    check_run_only_counts(result, 1);
    CHECK_FALSE(has_run(fixture.database, id(3)));
    CHECK(has_run(fixture.database, id(4)));
    CHECK(has_run(fixture.database, id(5)));
}

TEST_CASE("Retention preserves nonterminal runs with old attempts and output", "[jobu][retention]")
{
    auto const*       state = GENERATE("scheduled", "running", "retry_wait");
    RepositoryFixture fixture;
    insert_queue(fixture.database, id(1), "pending");
    insert_job(fixture.database, id(2), id(1));
    insert_terminal_run(fixture.database, id(3), id(2), id(1), 2);
    insert_attempt_output(fixture.database, id(3));
    execute(fixture.database,
            "UPDATE jobu_runs SET state = '" + std::string{state} + "', completed_at_us = NULL, result_json = NULL" +
                (std::string_view{state} == "scheduled" ? ", started_at_us = NULL" : ""));
    auto const before = storage_snapshot(fixture.database);
    check_run_only_counts(visit(fixture), 0);
    CHECK(storage_snapshot(fixture.database) == before);
}

TEST_CASE("Historical run queue governs retention after a definition moves", "[jobu][retention]")
{
    RepositoryFixture fixture;
    insert_queue(fixture.database, id(1), "old", true, 1s);
    insert_queue(fixture.database, id(2), "new", false, 0s);
    insert_job(fixture.database, id(3), id(2));
    insert_terminal_run(fixture.database, id(4), id(3), id(1), 1);
    insert_terminal_run(fixture.database, id(5), id(3), id(2), 1);
    auto first = visit(fixture);
    check_run_only_counts(first, 1);
    check_run_only_counts(visit(fixture, first.next), 0);
    CHECK_FALSE(has_run(fixture.database, id(4)));
    CHECK(has_run(fixture.database, id(5)));
    CHECK(scalar_count(fixture.database, "jobu_jobs") == 1);
    CHECK(scalar_count(fixture.database, "jobu_queues") == 2);
}

TEST_CASE("Retention advances full queues fairly and resets for later history", "[jobu][retention]")
{
    RepositoryFixture fixture;
    for (std::uint16_t number = 1; number <= 2; ++number) {
        insert_queue(fixture.database, id(number), "queue" + std::to_string(number));
        insert_job(fixture.database, id(number + 10), id(number));
    }
    for (std::uint16_t number = 20; number < 24; ++number) {
        insert_terminal_run(fixture.database, id(number), id(11), id(1), 1);
    }
    insert_terminal_run(fixture.database, id(30), id(12), id(2), 1);
    auto first = visit(fixture, {}, UtcTimePoint{20s}, 10s, 1);
    check_run_only_counts(first, 1);
    CHECK_FALSE(has_run(fixture.database, id(20)));
    auto second = visit(fixture, first.next, UtcTimePoint{20s}, 10s, 1);
    check_run_only_counts(second, 1);
    CHECK_FALSE(has_run(fixture.database, id(30)));
    CHECK(scalar_count(fixture.database, "jobu_runs") == 3);

    // A queue inserted behind the current cursor is picked up by the next sweep.
    insert_queue(fixture.database, id(0), "late-queue");
    auto end = finish_sweep(fixture, second.next);
    CHECK(end.sweep_complete);
    CHECK_FALSE(end.next.after_queue);
    auto next_sweep = visit(fixture, end.next);
    CHECK(next_sweep.next.after_queue == id(0));
    check_run_only_counts(visit(fixture, next_sweep.next), 3);
}

TEST_CASE("Retention reads policy edits at the next queue visit", "[jobu][retention]")
{
    RepositoryFixture fixture;
    insert_queue(fixture.database, id(1), "empty");
    insert_queue(fixture.database, id(2), "edited", false, 0s);
    insert_job(fixture.database, id(3), id(2));
    insert_terminal_run(fixture.database, id(4), id(3), id(2), 15'000'000);
    auto first = visit(fixture);
    execute(fixture.database, "UPDATE jobu_queues SET retention_seconds = 1");
    check_run_only_counts(visit(fixture, first.next), 1);
}

TEST_CASE("Retention cutoff handles clock precision rollback and durable range", "[jobu][retention]")
{
    RepositoryFixture fixture;
    insert_queue(fixture.database, id(1), "range");
    insert_job(fixture.database, id(2), id(1));
    auto const minimum = std::chrono::ceil<std::chrono::microseconds>(UtcTimePoint::min().time_since_epoch());
    auto const maximum = std::chrono::floor<std::chrono::microseconds>(UtcTimePoint::max().time_since_epoch());

    SECTION("huge inherited and queue durations saturate without overflow")
    {
        insert_terminal_run(fixture.database, id(3), id(2), id(1), minimum.count());
        check_run_only_counts(visit(fixture, {}, UtcTimePoint::max(), std::chrono::seconds::max()), 0);
        execute(fixture.database, "UPDATE jobu_queues SET retention_seconds = 9223372036854775807");
        check_run_only_counts(visit(fixture, {}, UtcTimePoint::max(), 1s), 0);
        CHECK(has_run(fixture.database, id(3)));
    }
    SECTION("minimum cannot underflow and maximum can retain equality")
    {
        insert_terminal_run(fixture.database, id(3), id(2), id(1), minimum.count());
        insert_terminal_run(fixture.database, id(4), id(2), id(1), maximum.count());
        check_run_only_counts(visit(fixture, {}, UtcTimePoint::min(), 1s), 0);
        check_run_only_counts(visit(fixture, {}, UtcTimePoint::max(), 1s), 1);
        CHECK(has_run(fixture.database, id(4)));
    }
    SECTION("pre-epoch cutoff is strict")
    {
        insert_terminal_run(fixture.database, id(3), id(2), id(1), -10'000'001);
        insert_terminal_run(fixture.database, id(4), id(2), id(1), -10'000'000);
        check_run_only_counts(visit(fixture, {}, UtcTimePoint{-9s}, 1s), 1);
        CHECK(has_run(fixture.database, id(4)));
    }
    SECTION("a cutoff between durable ticks includes the preceding tick")
    {
        insert_terminal_run(fixture.database, id(3), id(2), id(1), 10'000'000);
        auto const fractional = std::chrono::duration_cast<UtcClock::duration>(1ns);
        if (fractional != UtcClock::duration::zero()) {
            check_run_only_counts(visit(fixture, {}, UtcTimePoint{20s} + fractional), 1);
        }
    }
    SECTION("UTC rollback postpones expiry and forward correction permits it")
    {
        insert_terminal_run(fixture.database, id(3), id(2), id(1), 15'000'000);
        check_run_only_counts(visit(fixture), 0);
        check_run_only_counts(visit(fixture, {}, UtcTimePoint{15s}), 0);
        check_run_only_counts(visit(fixture, {}, UtcTimePoint{30s}), 1);
    }
}

TEST_CASE("Retention validates options without starting a transaction", "[jobu][retention]")
{
    RepositoryFixture fixture;
    fixture.faults->calls.clear();
    for (auto limit : {std::size_t{0}, std::size_t{1001}}) {
        require_error(fixture.retention.purge_next_batch(UtcTimePoint{20s}, 10s, limit, {}),
                      ErrorCategory::InvalidArgument,
                      "jobu.retention.invalid_options");
    }
    require_error(fixture.retention.purge_next_batch(UtcTimePoint{20s}, -1s, 100, {}),
                  ErrorCategory::InvalidArgument,
                  "jobu.retention.invalid_options");
    for (auto const& cursor : {
             RetentionSweepCursor{.after_queue = id(1), .phase = RetentionSweepPhase::DeletedJobs},
             RetentionSweepCursor{.after_owner = id(1)},
             RetentionSweepCursor{.phase = static_cast<RetentionSweepPhase>(99)},
    }) {
        require_error(fixture.retention.purge_next_batch(UtcTimePoint{20s}, 10s, 100, cursor),
                      ErrorCategory::InvalidArgument,
                      "jobu.retention.invalid_options");
    }
    CHECK(fixture.faults->calls.empty());
    auto result = visit(fixture);
    CHECK(result.next.phase == RetentionSweepPhase::OnceKeys);
    CHECK(finish_sweep(fixture, result.next).sweep_complete);
    check_run_only_counts(result, 0);
}

TEST_CASE("Retention pages equal completions atomically up to the maximum batch", "[jobu][retention][pagination]")
{
    RepositoryFixture fixture;
    insert_queue(fixture.database, id(1), "many");
    insert_job(fixture.database, id(2), id(1));
    for (std::uint16_t number = 10; number < 1011; ++number) {
        insert_terminal_run(fixture.database, id(number), id(2), id(1), 1);
    }
    check_run_only_counts(visit(fixture, {}, UtcTimePoint{20s}, 10s, 1000), 1000);
    CHECK(scalar_count(fixture.database, "jobu_runs") == 1);
    CHECK(has_run(fixture.database, id(1010)));
    check_run_only_counts(visit(fixture), 1);
}

TEST_CASE("Retention faults roll back queue history and its cascades", "[jobu][retention][fault]")
{
    auto fault = GENERATE(DatabaseCall{.boundary = "connection", .operation = DatabaseOperation::Begin},
                          DatabaseCall{.boundary = "retention.policy", .operation = DatabaseOperation::Fetch},
                          DatabaseCall{.boundary = "retention.policy", .operation = DatabaseOperation::Finish},
                          DatabaseCall{.boundary = "retention.first", .operation = DatabaseOperation::Fetch},
                          DatabaseCall{.boundary  = "retention.delete",
                                       .operation = DatabaseOperation::Execute,
                                       .phase     = DatabaseFaultPhase::AfterSuccess},
                          DatabaseCall{.boundary = "connection", .operation = DatabaseOperation::Commit});
    RepositoryFixture fixture;
    insert_queue(fixture.database, id(1), "fault", true);
    insert_job(fixture.database, id(2), id(1), true);
    insert_terminal_run(fixture.database, id(3), id(2), id(1), 1);
    insert_attempt_output(fixture.database, id(3));
    REQUIRE(fixture.idempotency.insert(cleanup_record(fixture, "job.create", id(1), "key", id(2))));
    auto const before = storage_snapshot(fixture.database);
    fixture.faults->faults.push_back({.at = std::move(fault), .error = fault_error()});
    auto failed = fixture.retention.purge_next_batch(UtcTimePoint{20s}, 10s, 100, {});
    REQUIRE_FALSE(failed);
    check_safe_error(failed.error(), "db.io");
    require_consumed_faults(*fixture.faults);
    CHECK(storage_snapshot(fixture.database) == before);
    CHECK(classify_storage_failure(failed.error(), StorageOperation::Mutation) == StorageFailureDisposition::Fatal);
    // No transaction/query crosses the call boundary; the connection can begin another unit.
    check_run_only_counts(visit(fixture), 1);
}

TEST_CASE("Retention continuation failure rolls back already deleted pages", "[jobu][retention][fault]")
{
    RepositoryFixture fixture;
    insert_queue(fixture.database, id(1), "pages");
    insert_job(fixture.database, id(2), id(1));
    for (std::uint16_t number = 10; number < 511; ++number) {
        insert_terminal_run(fixture.database, id(number), id(2), id(1), 1);
    }
    fixture.faults->faults.push_back({
        .at    = {.boundary = "retention.continuation", .operation = DatabaseOperation::Fetch},
        .error = fault_error()
    });
    auto const before = storage_snapshot(fixture.database);
    auto       failed = fixture.retention.purge_next_batch(UtcTimePoint{20s}, 10s, 1000, {});
    REQUIRE_FALSE(failed);
    require_consumed_faults(*fixture.faults);
    CHECK(storage_snapshot(fixture.database) == before);
}

TEST_CASE("Retention detects a skipped deletion and rolls the batch back", "[jobu][retention][fault]")
{
    RepositoryFixture fixture;
    insert_queue(fixture.database, id(1), "skipped");
    insert_job(fixture.database, id(2), id(1));
    insert_terminal_run(fixture.database, id(3), id(2), id(1), 1);
    insert_terminal_run(fixture.database, id(4), id(2), id(1), 1);
    execute(fixture.database,
            "CREATE TRIGGER skip_retention_run BEFORE DELETE ON jobu_runs "
            "WHEN OLD.id = X'00000000000070008000000000000004' BEGIN SELECT RAISE(IGNORE); END");
    auto const before = storage_snapshot(fixture.database);
    auto       failed = fixture.retention.purge_next_batch(UtcTimePoint{20s}, 10s, 100, {});
    REQUIRE_FALSE(failed);
    check_safe_error(failed.error(), "jobu.retention.invalid_relationship");
    CHECK(classify_storage_failure(failed.error(), StorageOperation::Mutation, StorageFailureOrigin::PersistedData) ==
          StorageFailureDisposition::Fatal);
    CHECK(storage_snapshot(fixture.database) == before);
}

TEST_CASE("Retention preserves uncertain commit and poisoned rollback outcomes", "[jobu][retention][fault]")
{
    RepositoryFixture fixture;
    insert_queue(fixture.database, id(1), "fatal");
    insert_job(fixture.database, id(2), id(1));
    insert_terminal_run(fixture.database, id(3), id(2), id(1), 1);
    SECTION("commit acknowledgement fails after durable deletion")
    {
        fixture.faults->faults.push_back({
            .at    = {.boundary  = "connection",
                      .operation = DatabaseOperation::Commit,
                      .phase     = DatabaseFaultPhase::AfterSuccess},
            .error = fault_error()
        });
        auto failed = fixture.retention.purge_next_batch(UtcTimePoint{20s}, 10s, 100, {});
        REQUIRE_FALSE(failed);
        check_safe_error(failed.error(), "db.io");
        require_consumed_faults(*fixture.faults);
        REQUIRE(fixture.database.close());
        REQUIRE(fixture.database.open());
        CHECK_FALSE(has_run(fixture.database, id(3)));
    }
    SECTION("rollback failure poisons the connection")
    {
        fixture.faults->faults.push_back({
            .at    = {.boundary  = "retention.delete",
                      .operation = DatabaseOperation::Execute,
                      .phase     = DatabaseFaultPhase::AfterSuccess},
            .error = fault_error()
        });
        fixture.faults->faults.push_back({
            .at    = {.boundary = "connection", .operation = DatabaseOperation::Rollback},
            .error = fault_error()
        });
        auto failed = fixture.retention.purge_next_batch(UtcTimePoint{20s}, 10s, 100, {});
        REQUIRE_FALSE(failed);
        check_safe_error(failed.error(), "db.io");
        require_consumed_faults(*fixture.faults);
        require_error(Transaction::begin(fixture.database), ErrorCategory::Internal, "db.connection_failed");
        REQUIRE(fixture.database.close());
        REQUIRE(fixture.database.open());
        CHECK(has_run(fixture.database, id(3)));
    }
}

TEST_CASE("Retention uses metadata projections and indexed first and continuation pages", "[jobu][retention][plan]")
{
    RepositoryFixture fixture;
    insert_queue(fixture.database, id(1), "projections");
    insert_job(fixture.database, id(2), id(1));
    for (std::uint16_t number = 10; number < 511; ++number) {
        insert_terminal_run(fixture.database, id(number), id(2), id(1), 1);
    }
    fixture.prepared_sql->clear();
    check_run_only_counts(visit(fixture, {}, UtcTimePoint{20s}, 10s, 1000), 501);
    auto const queries = *fixture.prepared_sql;
    auto       pages   = std::size_t{0};
    for (auto const& sql : queries) {
        // Selection never materializes payloads, attributes, results, or output.
        CHECK(sql.find("payload_json") == std::string::npos);
        CHECK(sql.find("defaults_json") == std::string::npos);
        CHECK(sql.find("attributes_json") == std::string::npos);
        CHECK(sql.find("jobu_attempt_output") == std::string::npos);
        if (!sql.starts_with("SELECT id AS run_id, completed_at_us")) {
            continue;
        }
        ++pages;
        Query plan{fixture.database};
        REQUIRE(plan.prepare("EXPLAIN QUERY PLAN " + sql));
        REQUIRE(plan.bind_value(":queue_id", uuid_to_storage(id(1))));
        REQUIRE(plan.bind_value(":cutoff", std::int64_t{10'000'000}));
        REQUIRE(plan.bind_value(":limit", std::int64_t{500}));
        if (sql.find(":after_completed") != std::string::npos) {
            REQUIRE(plan.bind_value(":after_completed", std::int64_t{1}));
            REQUIRE(plan.bind_value(":after_id", uuid_to_storage(id(509))));
        }
        REQUIRE(plan.exec());
        bool indexed = false;
        while (true) {
            auto next = plan.next();
            REQUIRE(next);
            if (!*next) {
                break;
            }
            auto const* detail = std::get_if<std::string>(plan.record().value("detail"));
            REQUIRE(detail);
            CHECK(detail->find("TEMP B-TREE") == std::string::npos);
            indexed = indexed || detail->find("jobu_runs_queue_completed_id_idx") != std::string::npos;
        }
        CHECK(indexed);
    }
    CHECK(pages == 2);
}

TEST_CASE("Retention cascades many attempts and binary output with one parent deletion", "[jobu][retention][cascade]")
{
    RepositoryFixture fixture;
    insert_queue(fixture.database, id(1), "large-output");
    insert_job(fixture.database, id(2), id(1));
    insert_terminal_run(fixture.database, id(3), id(2), id(1), 3);
    for (std::int64_t number = 1; number <= 128; ++number) {
        insert_attempt_output(fixture.database, id(3), number, std::size_t{64} * 1024);
    }
    auto const started = std::chrono::steady_clock::now();
    auto       result  = visit(fixture, {}, UtcTimePoint{20s}, 10s, 1);
    auto const elapsed = std::chrono::steady_clock::now() - started;
    check_run_only_counts(result, 1);
    CHECK(scalar_count(fixture.database, "jobu_attempts") == 0);
    CHECK(scalar_count(fixture.database, "jobu_attempt_output") == 0);
    CHECK(scalar_count(fixture.database, "jobu_run_timing") == 0);
    std::cout << "Retention cascade: 1 run, 128 attempts, 8 MiB output, "
              << std::chrono::duration<double, std::milli>{elapsed}.count() << " ms\n";
}

TEST_CASE("Retention end of sweep propagates cursor cleanup failure", "[jobu][retention][fault]")
{
    RepositoryFixture fixture;
    fixture.faults->faults.push_back({
        .at    = {.boundary = "retention.policy", .operation = DatabaseOperation::Finish},
        .error = fault_error()
    });
    auto failed = fixture.retention.purge_next_batch(UtcTimePoint{20s}, 10s, 100, {});
    REQUIRE_FALSE(failed);
    check_safe_error(failed.error(), "db.io");
    require_consumed_faults(*fixture.faults);
    CHECK(finish_sweep(fixture).sweep_complete);
}
