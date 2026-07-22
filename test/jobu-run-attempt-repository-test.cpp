#include "attempt_repository_priv.hpp"
#include "run_repository_priv.hpp"

#include "attribute_registry.hpp"
#include "database.hpp"
#include "domain_storage_priv.hpp"
#include "query.hpp"
#include "sqlite/sqlite_driver.hpp"
#include "sqlite/sqlite_schema.hpp"
#include "support/temporary_directory.hpp"
#include "transaction.hpp"

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
using namespace jb::rpc;
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

auto json_string(std::string value) -> JsonValue
{
    auto result = JsonValue{};
    result.data = std::move(value);
    return result;
}

auto json_object(std::string key, std::string value) -> JsonValue
{
    auto result = JsonValue{};
    result.data = JsonValue::Object{
        {std::move(key), json_string(std::move(value))}
    };
    return result;
}

struct RepositoryFixture {
    RepositoryFixture()
        : runs{database, registry}
        , attempts{database}
    {
        REQUIRE(database.open());
        REQUIRE(jb::jobu::sqlite::ensure_schema(database));
    }

    TemporaryDirectory        directory;
    std::filesystem::path     database_file{directory.path() / "jobu.sqlite"};
    Database                  database{make_database(database_file)};
    StandardAttributeRegistry registry;
    RunRepository             runs;
    AttemptRepository         attempts;
};

void insert_queue(Database& database, Uuid const& queue_id, std::string_view name)
{
    Query query{database};
    REQUIRE(query.prepare(
        "INSERT INTO jobu_queues(id, name, deleted_name, state, weight, concurrency_limit, recovery_policy, "
        "defaults_json, retention_seconds, runnable_wait_warning_ms, created_at_us, updated_at_us, deleted_at_us) "
        "VALUES(:id, :name, NULL, 'active', 1, 1, 'fail_interrupted', '{\"version\":1,\"values\":{}}', "
        "NULL, 10000, 0, 0, NULL)"));
    REQUIRE(query.bind_value(":id", uuid_to_storage(queue_id)));
    REQUIRE(query.bind_value(":name", make_text(name)));
    REQUIRE(query.exec());
}

void insert_job(Database& database, Uuid const& job_id, Uuid const& queue_id)
{
    Query query{database};
    REQUIRE(query.prepare(
        "INSERT INTO jobu_jobs(id, queue_id, revision, name, state, type, schedule_kind, scheduled_at_us, "
        "cron_expression, cron_timezone, priority, attributes_json, payload_json, created_at_us, updated_at_us, "
        "deleted_at_us) VALUES(:id, :queue_id, 1, NULL, 'active', 'cli', 'once', 0, NULL, NULL, 0, "
        "'{\"version\":1,\"values\":{}}', '{}', 0, 0, NULL)"));
    REQUIRE(query.bind_value(":id", uuid_to_storage(job_id)));
    REQUIRE(query.bind_value(":queue_id", uuid_to_storage(queue_id)));
    REQUIRE(query.exec());
}

auto materialized_attributes(StandardAttributeRegistry const& registry, std::int64_t attempts = 1) -> AttributeSet
{
    auto values = materialize_attributes(registry,
                                         {
    },
                                         {},
                                         {{"retry.max_attempts", {.data = attempts}}});
    REQUIRE(values);
    return std::move(values).value();
}

auto make_run(StandardAttributeRegistry const& registry,
              Uuid const&                      run_id,
              Uuid const&                      job_id,
              Uuid const&                      queue_id,
              RunState                         state      = RunState::Scheduled,
              UtcTimePoint                     planned_at = UtcTimePoint{10s}) -> JobRun
{
    auto run = JobRun{
        .id             = run_id,
        .job_id         = job_id,
        .job_revision   = 1,
        .queue_id       = queue_id,
        .origin         = RunOrigin::Scheduled,
        .schedule_owned = true,
        .planned_at     = planned_at,
        .runnable_at    = planned_at,
        .type           = JobType::Cli,
        .priority       = 0,
        .attributes     = materialized_attributes(registry),
        .payload        = json_object("command", "/bin/true"),
        .state          = state,
    };
    if (state == RunState::Running || state == RunState::RetryWait || state == RunState::Succeeded ||
        state == RunState::Failed || state == RunState::Interrupted) {
        run.started_at = planned_at + 1s;
    }
    if (state == RunState::Succeeded || state == RunState::Failed || state == RunState::Interrupted ||
        state == RunState::Cancelled) {
        run.completed_at = planned_at + 2s;
        run.result       = json_object("reason", "fixture");
    }
    return run;
}

auto require_error(auto const& result, ErrorCategory category, std::string_view code) -> Error
{
    REQUIRE_FALSE(result);
    CHECK(result.error().category == category);
    CHECK(result.error().code == code);
    return result.error();
}

void execute(Database& database, std::string_view sql)
{
    Query query{database};
    REQUIRE(query.exec(sql));
}

} // anonymous namespace

TEST_CASE("Run repository round-trips schedule-owned snapshots and enforces uniqueness", "[jobu][run][sqlite]")
{
    RepositoryFixture fixture;
    auto const        queue_id = id(1);
    auto const        job_id   = id(2);
    auto const        run_id   = id(3);
    insert_queue(fixture.database, queue_id, "primary");
    insert_job(fixture.database, job_id, queue_id);

    auto run         = make_run(fixture.registry, run_id, job_id, queue_id);
    run.job_revision = 4;
    run.runnable_at  = UtcTimePoint{11s};
    run.type         = JobType::Http;
    run.priority     = -7;
    run.attributes   = materialized_attributes(fixture.registry, 3);
    run.payload      = json_object("url", "https://example.test/");
    REQUIRE(fixture.runs.insert_schedule_owned(run));

    auto found = fixture.runs.find_by_id(run_id);
    REQUIRE(found);
    REQUIRE(found->has_value());
    CHECK((*found)->id == run.id);
    CHECK((*found)->job_id == run.job_id);
    CHECK((*found)->job_revision == 4);
    CHECK((*found)->queue_id == queue_id);
    CHECK((*found)->origin == RunOrigin::Scheduled);
    CHECK((*found)->schedule_owned);
    CHECK((*found)->planned_at == UtcTimePoint{10s});
    CHECK((*found)->runnable_at == UtcTimePoint{11s});
    CHECK((*found)->type == JobType::Http);
    CHECK((*found)->priority == -7);
    CHECK(std::get<std::int64_t>((*found)->attributes.at("retry.max_attempts").data) == 3);
    CHECK((*found)->payload == run.payload);
    CHECK((*found)->state == RunState::Scheduled);
    CHECK_FALSE((*found)->result);

    auto schedule_owned = fixture.runs.find_schedule_owned(job_id);
    REQUIRE(schedule_owned);
    REQUIRE(schedule_owned->has_value());
    CHECK((*schedule_owned)->id == run_id);

    auto duplicate = run;
    duplicate.id   = id(4);
    require_error(fixture.runs.insert_schedule_owned(duplicate), ErrorCategory::Conflict, "jobu.run.schedule_conflict");

    auto const second_job_id = id(5);
    insert_job(fixture.database, second_job_id, queue_id);
    auto duplicate_id   = run;
    duplicate_id.job_id = second_job_id;
    require_error(fixture.runs.insert_schedule_owned(duplicate_id), ErrorCategory::Conflict, "db.constraint.unique");

    auto missing = fixture.runs.find_by_id(id(99));
    REQUIRE(missing);
    CHECK_FALSE(missing->has_value());
}

TEST_CASE("Run repository enforces execution start lifecycle invariants", "[jobu][run][sqlite]")
{
    RepositoryFixture fixture;
    auto const        queue_id = id(49);
    auto              suffix   = std::uint8_t{50};
    insert_queue(fixture.database, queue_id, "primary");

    constexpr RunState requires_started_at[] = {
        RunState::Running,
        RunState::RetryWait,
        RunState::Succeeded,
        RunState::Failed,
        RunState::Interrupted,
    };
    for (auto const state : requires_started_at) {
        auto const job_id = id(suffix++);
        auto const run_id = id(suffix++);
        insert_job(fixture.database, job_id, queue_id);

        auto run = make_run(fixture.registry, run_id, job_id, queue_id, state);
        run.started_at.reset();
        auto insertion_error =
            require_error(fixture.runs.insert_schedule_owned(run), ErrorCategory::Internal, "jobu.storage.invariant");
        CHECK(insertion_error.detail == "reason=insert_state_mismatch");

        run.started_at = run.planned_at + 1s;
        REQUIRE(fixture.runs.insert_schedule_owned(run));
        {
            Query query{fixture.database};
            REQUIRE(query.prepare("UPDATE jobu_runs SET started_at_us = NULL WHERE id = :id"));
            REQUIRE(query.bind_value(":id", uuid_to_storage(run_id)));
            REQUIRE(query.exec());
        }
        auto decoding_error =
            require_error(fixture.runs.find_by_id(run_id), ErrorCategory::Internal, "jobu.storage.invariant");
        CHECK(decoding_error.detail == "reason=started_state_mismatch");
    }

    auto const cancelled_job_id = id(suffix++);
    auto const cancelled_run_id = id(suffix);
    insert_job(fixture.database, cancelled_job_id, queue_id);
    auto cancelled = make_run(fixture.registry, cancelled_run_id, cancelled_job_id, queue_id, RunState::Cancelled);
    REQUIRE_FALSE(cancelled.started_at);
    REQUIRE(fixture.runs.insert_schedule_owned(cancelled));
    auto persisted_cancelled = fixture.runs.find_by_id(cancelled_run_id);
    REQUIRE(persisted_cancelled);
    REQUIRE(persisted_cancelled->has_value());
    CHECK_FALSE((*persisted_cancelled)->started_at);
}

TEST_CASE("Attempt repository enforces execution start outcome invariants", "[jobu][attempt][sqlite]")
{
    RepositoryFixture fixture;
    auto const        queue_id = id(62);
    auto const        job_id   = id(63);
    auto const        run_id   = id(64);
    insert_queue(fixture.database, queue_id, "primary");
    insert_job(fixture.database, job_id, queue_id);
    REQUIRE(fixture.runs.insert_schedule_owned(make_run(fixture.registry, run_id, job_id, queue_id)));

    constexpr AttemptOutcome requires_started_at[] = {
        AttemptOutcome::Succeeded,
        AttemptOutcome::Failed,
        AttemptOutcome::Interrupted,
    };
    auto attempt_number = AttemptNumber{1};
    for (auto const outcome : requires_started_at) {
        auto attempt = JobAttempt{
            .run_id         = run_id,
            .attempt_number = attempt_number,
            .due_at         = UtcTimePoint{30s},
            .completed_at   = UtcTimePoint{30s} + 2ms,
            .state          = AttemptState::Completed,
            .outcome        = outcome,
        };
        auto insertion_error =
            require_error(fixture.attempts.insert_attempt(attempt), ErrorCategory::Internal, "jobu.storage.invariant");
        CHECK(insertion_error.detail == "reason=insert_state_fields_mismatch");

        attempt.started_at = UtcTimePoint{30s} + 1ms;
        REQUIRE(fixture.attempts.insert_attempt(attempt));
        ++attempt_number;
    }

    execute(fixture.database, "UPDATE jobu_attempts SET started_at_us = NULL");
    for (auto number = AttemptNumber{1}; number < attempt_number; ++number) {
        auto decoding_error =
            require_error(fixture.attempts.find(run_id, number), ErrorCategory::Internal, "jobu.storage.invariant");
        CHECK(decoding_error.detail == "reason=state_fields_mismatch");
    }

    auto cancelled = JobAttempt{
        .run_id         = run_id,
        .attempt_number = attempt_number,
        .due_at         = UtcTimePoint{30s},
        .completed_at   = UtcTimePoint{30s} + 2ms,
        .state          = AttemptState::Completed,
        .outcome        = AttemptOutcome::Cancelled,
    };
    REQUIRE(fixture.attempts.insert_attempt(cancelled));
    auto persisted_cancelled = fixture.attempts.find(run_id, attempt_number);
    REQUIRE(persisted_cancelled);
    REQUIRE(persisted_cancelled->has_value());
    CHECK_FALSE((*persisted_cancelled)->started_at);
    CHECK((*persisted_cancelled)->outcome == AttemptOutcome::Cancelled);
}

TEST_CASE("Run refresh and attempt persistence preserve guarded snapshots and binary output", "[jobu][attempt][sqlite]")
{
    RepositoryFixture fixture;
    auto const        first_queue  = id(10);
    auto const        second_queue = id(11);
    auto const        job_id       = id(12);
    auto const        run_id       = id(13);
    insert_queue(fixture.database, first_queue, "first");
    insert_queue(fixture.database, second_queue, "second");
    insert_job(fixture.database, job_id, first_queue);
    REQUIRE(fixture.runs.insert_schedule_owned(make_run(fixture.registry, run_id, job_id, first_queue)));

    auto snapshot = RunSnapshot{
        .job_revision = 2,
        .queue_id     = second_queue,
        .planned_at   = UtcTimePoint{20s},
        .runnable_at  = UtcTimePoint{21s},
        .type         = JobType::Http,
        .priority     = 9,
        .attributes   = materialized_attributes(fixture.registry, 5),
        .payload      = json_object("url", "https://updated.test/"),
    };
    auto refreshed = fixture.runs.refresh_unstarted_schedule_owned(job_id, snapshot);
    REQUIRE(refreshed);
    CHECK(*refreshed);

    auto pending = JobAttempt{
        .run_id         = run_id,
        .attempt_number = 2,
        .due_at         = UtcTimePoint{22s},
        .state          = AttemptState::Pending,
    };
    REQUIRE(fixture.attempts.insert_attempt(pending));
    CHECK(*fixture.attempts.has_any_for_run(run_id));
    CHECK_FALSE(*fixture.attempts.has_started_for_job(job_id));
    snapshot.job_revision = 3;
    snapshot.priority     = 10;
    refreshed             = fixture.runs.refresh_unstarted_schedule_owned(job_id, snapshot);
    REQUIRE(refreshed);
    CHECK_FALSE(*refreshed);

    auto persisted_run = fixture.runs.find_by_id(run_id);
    REQUIRE(persisted_run);
    REQUIRE(persisted_run->has_value());
    CHECK((*persisted_run)->job_revision == 2);
    CHECK((*persisted_run)->queue_id == second_queue);
    CHECK((*persisted_run)->priority == 9);

    auto completed = JobAttempt{
        .run_id         = run_id,
        .attempt_number = 1,
        .due_at         = UtcTimePoint{20s},
        .started_at     = UtcTimePoint{20s} + 1ms,
        .completed_at   = UtcTimePoint{20s} + 2ms,
        .state          = AttemptState::Completed,
        .outcome        = AttemptOutcome::Failed,
        .result         = json_object("message", "fixture failure"),
    };
    REQUIRE(fixture.attempts.insert_attempt(completed));
    require_error(fixture.attempts.insert_attempt(completed), ErrorCategory::Conflict, "db.constraint.unique");

    auto attempts = fixture.attempts.list_for_run(run_id, 1);
    REQUIRE(attempts);
    REQUIRE(attempts->size() == 1);
    CHECK(attempts->front().attempt_number == 1);
    CHECK(attempts->front().outcome == AttemptOutcome::Failed);
    CHECK(attempts->front().result == completed.result);
    CHECK(*fixture.attempts.has_any_for_run(run_id));
    CHECK(*fixture.attempts.has_started_for_job(job_id));
    require_error(fixture.attempts.list_for_run(run_id, 0),
                  ErrorCategory::InvalidArgument,
                  "jobu.storage.invalid_limit");
    require_error(fixture.attempts.list_for_run(run_id, 1001),
                  ErrorCategory::InvalidArgument,
                  "jobu.storage.invalid_limit");

    auto first_output = AttemptOutput{
        .stdout_bytes     = ByteBuffer{std::byte{0x00}, std::byte{0x41}, std::byte{0x00}},
        .stderr_bytes     = ByteBuffer{},
        .stdout_truncated = true,
        .stderr_truncated = false,
        .capture_lost     = false,
    };
    {
        auto begun = Transaction::begin(fixture.database);
        REQUIRE(begun);
        auto transaction = std::move(begun).value();
        REQUIRE(fixture.attempts.insert_or_replace_output(run_id, 1, first_output));
        REQUIRE(transaction.commit());
    }

    auto output = fixture.attempts.find_output(run_id, 1);
    REQUIRE(output);
    REQUIRE(output->has_value());
    CHECK((*output)->stdout_bytes == first_output.stdout_bytes);
    REQUIRE((*output)->stderr_bytes);
    CHECK((*output)->stderr_bytes->empty());
    CHECK((*output)->stdout_truncated);
    CHECK_FALSE((*output)->capture_lost);

    auto replacement = AttemptOutput{
        .stdout_bytes     = std::nullopt,
        .stderr_bytes     = ByteBuffer{std::byte{0xff}, std::byte{0x00}},
        .stdout_truncated = false,
        .stderr_truncated = true,
        .capture_lost     = true,
    };
    {
        auto begun = Transaction::begin(fixture.database);
        REQUIRE(begun);
        auto transaction = std::move(begun).value();
        REQUIRE(fixture.attempts.insert_or_replace_output(run_id, 1, replacement));
        REQUIRE(fixture.attempts.insert_or_replace_output(run_id, 1, replacement));
        REQUIRE(transaction.commit());
    }

    output = fixture.attempts.find_output(run_id, 1);
    REQUIRE(output);
    REQUIRE(output->has_value());
    CHECK_FALSE((*output)->stdout_bytes);
    CHECK((*output)->stderr_bytes == replacement.stderr_bytes);
    CHECK((*output)->stderr_truncated);
    CHECK((*output)->capture_lost);
}

TEST_CASE("Run movement cancellation and terminal deletion preserve lifecycle boundaries", "[jobu][run][sqlite]")
{
    RepositoryFixture fixture;
    auto const        first_queue  = id(20);
    auto const        second_queue = id(21);
    auto const        move_job     = id(22);
    auto const        retry_job    = id(23);
    auto const        running_job  = id(24);
    auto const        pending_job  = id(25);
    insert_queue(fixture.database, first_queue, "first");
    insert_queue(fixture.database, second_queue, "second");
    for (auto const& job_id : {move_job, retry_job, running_job, pending_job}) {
        insert_job(fixture.database, job_id, first_queue);
    }

    auto history = make_run(fixture.registry, id(30), move_job, first_queue, RunState::Succeeded, UtcTimePoint{1s});
    auto current = make_run(fixture.registry, id(31), move_job, first_queue);
    REQUIRE(fixture.runs.insert_schedule_owned(history));
    REQUIRE(fixture.runs.insert_schedule_owned(current));
    auto moved = fixture.runs.move_non_terminal(move_job, second_queue, 5);
    REQUIRE(moved);
    CHECK(*moved == 1);
    auto persisted_current = fixture.runs.find_by_id(current.id);
    REQUIRE(persisted_current);
    REQUIRE(persisted_current->has_value());
    CHECK((*persisted_current)->queue_id == second_queue);
    CHECK((*persisted_current)->job_revision == 5);
    auto persisted_history = fixture.runs.find_by_id(history.id);
    REQUIRE(persisted_history);
    REQUIRE(persisted_history->has_value());
    CHECK((*persisted_history)->queue_id == first_queue);
    CHECK((*persisted_history)->job_revision == 1);

    auto retry_run = make_run(fixture.registry, id(32), retry_job, first_queue, RunState::RetryWait);
    auto running   = make_run(fixture.registry, id(33), running_job, first_queue, RunState::Running);
    auto pending   = make_run(fixture.registry, id(34), pending_job, first_queue);
    REQUIRE(fixture.runs.insert_schedule_owned(retry_run));
    REQUIRE(fixture.runs.insert_schedule_owned(running));
    REQUIRE(fixture.runs.insert_schedule_owned(pending));

    CHECK(*fixture.runs.count_running_for_job(running_job) == 1);
    CHECK(*fixture.runs.count_running_for_queue(first_queue) == 1);
    auto cancelled = fixture.runs.cancel_pending_for_job(retry_job, UtcTimePoint{20s}, "job_deleted");
    REQUIRE(cancelled);
    CHECK(*cancelled == 1);
    cancelled = fixture.runs.cancel_pending_for_queue(first_queue, UtcTimePoint{21s}, "queue_deleted");
    REQUIRE(cancelled);
    CHECK(*cancelled == 1);

    auto cancelled_retry = fixture.runs.find_by_id(retry_run.id);
    REQUIRE(cancelled_retry);
    REQUIRE(cancelled_retry->has_value());
    CHECK((*cancelled_retry)->state == RunState::Cancelled);
    CHECK((*cancelled_retry)->completed_at == UtcTimePoint{20s});
    REQUIRE((*cancelled_retry)->result);
    CHECK((*cancelled_retry)->result->as_object().at("reason").as_string() == "job_deleted");
    auto preserved_running = fixture.runs.find_by_id(running.id);
    REQUIRE(preserved_running);
    REQUIRE(preserved_running->has_value());
    CHECK((*preserved_running)->state == RunState::Running);

    auto completed_attempt = JobAttempt{
        .run_id         = history.id,
        .attempt_number = 1,
        .due_at         = UtcTimePoint{1s},
        .started_at     = UtcTimePoint{1s} + 1ms,
        .completed_at   = UtcTimePoint{1s} + 2ms,
        .state          = AttemptState::Completed,
        .outcome        = AttemptOutcome::Succeeded,
        .result         = json_object("message", "done"),
    };
    REQUIRE(fixture.attempts.insert_attempt(completed_attempt));
    {
        auto begun = Transaction::begin(fixture.database);
        REQUIRE(begun);
        auto transaction = std::move(begun).value();
        REQUIRE(fixture.attempts.insert_or_replace_output(history.id,
                                                          1,
                                                          AttemptOutput{.stdout_bytes = ByteBuffer{std::byte{0x01}}}));
        REQUIRE(transaction.commit());
    }

    auto terminal = fixture.runs.list_terminal_before(UtcTimePoint{30s}, 2);
    REQUIRE(terminal);
    REQUIRE(terminal->size() == 2);
    CHECK((*terminal)[0] == history.id);
    CHECK((*terminal)[1] == retry_run.id);
    require_error(fixture.runs.list_terminal_before(UtcTimePoint{30s}, 0),
                  ErrorCategory::InvalidArgument,
                  "jobu.storage.invalid_limit");

    auto delete_ids = std::vector<Uuid>{history.id, running.id};
    {
        auto begun = Transaction::begin(fixture.database);
        REQUIRE(begun);
        auto transaction = std::move(begun).value();
        auto deleted     = fixture.runs.delete_selected_terminal(delete_ids);
        REQUIRE(deleted);
        CHECK(*deleted == 1);
        REQUIRE(transaction.commit());
    }

    auto deleted_attempt = fixture.attempts.find(history.id, 1);
    REQUIRE(deleted_attempt);
    CHECK_FALSE(deleted_attempt->has_value());
    auto deleted_output = fixture.attempts.find_output(history.id, 1);
    REQUIRE(deleted_output);
    CHECK_FALSE(deleted_output->has_value());
    preserved_running = fixture.runs.find_by_id(running.id);
    REQUIRE(preserved_running);
    CHECK(preserved_running->has_value());
}

TEST_CASE("Run retention deletion preserves the maximum batch contract", "[jobu][run][sqlite]")
{
    RepositoryFixture fixture;
    auto const        queue_id = id(70);
    auto const        job_id   = id(71);
    insert_queue(fixture.database, queue_id, "primary");
    insert_job(fixture.database, job_id, queue_id);

    auto first  = make_run(fixture.registry, id(72), job_id, queue_id, RunState::Succeeded);
    auto second = make_run(fixture.registry, id(73), job_id, queue_id, RunState::Failed);
    REQUIRE(fixture.runs.insert_schedule_owned(first));
    REQUIRE(fixture.runs.insert_schedule_owned(second));

    auto delete_ids    = std::vector<Uuid>(1000U, id(99));
    delete_ids.front() = first.id;
    delete_ids.back()  = second.id;
    {
        auto begun = Transaction::begin(fixture.database);
        REQUIRE(begun);
        auto transaction = std::move(begun).value();
        auto deleted     = fixture.runs.delete_selected_terminal(delete_ids);
        REQUIRE(deleted);
        CHECK(*deleted == 2);
        REQUIRE(transaction.commit());
    }

    auto persisted_first = fixture.runs.find_by_id(first.id);
    REQUIRE(persisted_first);
    CHECK_FALSE(persisted_first->has_value());
    auto persisted_second = fixture.runs.find_by_id(second.id);
    REQUIRE(persisted_second);
    CHECK_FALSE(persisted_second->has_value());
}

TEST_CASE("Run repository rejects malformed persisted snapshot documents", "[jobu][run][storage]")
{
    RepositoryFixture fixture;
    auto const        queue_id = id(40);
    auto const        job_id   = id(41);
    auto const        run_id   = id(42);
    insert_queue(fixture.database, queue_id, "primary");
    insert_job(fixture.database, job_id, queue_id);
    REQUIRE(fixture.runs.insert_schedule_owned(make_run(fixture.registry, run_id, job_id, queue_id)));

    execute(fixture.database, "UPDATE jobu_runs SET payload_json = '[]'");
    require_error(fixture.runs.find_by_id(run_id), ErrorCategory::Internal, "jobu.storage.invalid_json");

    execute(fixture.database, "UPDATE jobu_runs SET payload_json = '{}', attributes_json = '{}'");
    require_error(fixture.runs.find_by_id(run_id), ErrorCategory::Internal, "jobu.attribute.invalid_document");
}
