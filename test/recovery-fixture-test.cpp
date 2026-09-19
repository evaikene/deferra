#include "support/recovery_fixture.hpp"

#include "attempt_repository_priv.hpp"
#include "byte_buffer.hpp"
#include "job_repository_priv.hpp"
#include "query.hpp"
#include "queue_repository_priv.hpp"
#include "run_repository_priv.hpp"
#include "scheduler_repository_priv.hpp"
#include "transaction.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

using namespace jb::core;
using namespace jb::db;
using namespace jb::jobu;
using namespace jb::jobu::detail;
using namespace jb::test;
using namespace std::chrono_literals;

namespace {

void require_constraint(Database& database, std::string_view sql, std::string_view code)
{
    Query query{database};
    auto  result = query.exec(sql);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == code);
}

void execute(Database& database, std::string_view sql)
{
    Query query{database};
    REQUIRE(query.exec(sql));
}

} // namespace

TEST_CASE("Recovery fixtures round-trip run shapes and complete histories after reopen", "[jobu][recovery][sqlite]")
{
    auto const state  = GENERATE(RunState::Scheduled,
                                 RunState::Running,
                                 RunState::RetryWait,
                                 RunState::Succeeded,
                                 RunState::Failed,
                                 RunState::Interrupted,
                                 RunState::Cancelled);
    auto const origin = GENERATE(RunOrigin::Scheduled, RunOrigin::Manual);
    auto const type   = GENERATE(JobType::Cli, JobType::Http);
    CAPTURE(state, origin, type);
    RecoveryFixture fixture;
    auto            queue = recovery_queue(recovery_id(1));
    auto            job   = fixture.make_job(recovery_id(2), queue.id, type);
    job.revision          = 4;
    job.priority          = -7;
    fixture.insert_queue(queue);
    fixture.insert_job(job);

    // Retry and terminal histories include a previous failure; Scheduled and pre-dispatch cancellation have none.
    auto const failures = state == RunState::Scheduled || state == RunState::Cancelled ? 0U : 1U;
    auto       expected = fixture.make_run(recovery_id(3), job, state, failures, origin);
    if (origin == RunOrigin::Manual) {
        // A manual occurrence keeps the definition's future schedule-owned sibling intact.
        auto sibling            = fixture.make_run(recovery_id(4), job);
        sibling.run.planned_at  = UtcTimePoint{200s};
        sibling.run.runnable_at = UtcTimePoint{200s};
        fixture.insert_run(sibling);
    }
    fixture.insert_run(expected);
    fixture.reopen();
    fixture.require_run(expected);

    CHECK(expected.run.planned_at == UtcTimePoint{10s});
    if (state == RunState::Running || state == RunState::RetryWait) {
        CHECK(expected.run.started_at == UtcTimePoint{11s});
        CHECK_FALSE(expected.run.completed_at);
        CHECK_FALSE(expected.run.result);
        CHECK(expected.attempts.front().attempt.state == AttemptState::Completed);
        CHECK(expected.attempts.front().attempt.outcome == AttemptOutcome::Failed);
    }
    if (state == RunState::RetryWait) {
        CHECK(expected.attempts.size() == 1);
        SchedulerRepository scheduler{fixture.database, fixture.registry};
        auto                context = scheduler.find_dispatch_context(expected.run.id, UtcTimePoint{100s});
        REQUIRE(context);
        REQUIRE(context->has_value());
        CHECK((*context)->next_attempt == 2);
    }
}

TEST_CASE("Recovery fixtures preserve absent empty binary and lost capture", "[jobu][recovery][sqlite]")
{
    RecoveryFixture fixture;
    auto            queue = recovery_queue(recovery_id(1));
    auto            job   = fixture.make_job(recovery_id(2), queue.id);
    fixture.insert_queue(queue);
    fixture.insert_job(job);
    auto expected = fixture.make_run(recovery_id(3), job, RunState::Failed, 1);

    SECTION("No output row is required by the existing completion contract")
    {}
    SECTION("NULL channels differ from present empty blobs")
    {
        expected.attempts.back().output = AttemptOutput{.stdout_bytes = ByteBuffer{}};
    }
    SECTION("Binary capture and truncation survive without text conversion")
    {
        expected.attempts.back().output = AttemptOutput{
            .stdout_bytes     = ByteBuffer{std::byte{0}, std::byte{0xff}},
            .stderr_bytes     = ByteBuffer{std::byte{0x41}},
            .stdout_truncated = true,
            .stderr_truncated = true
        };
    }
    SECTION("Interrupted capture metadata fits the existing schema")
    {
        // This is the target recovery representation, not a claim that baseline recovery already writes it.
        expected = fixture.make_run(recovery_id(3), job, RunState::Interrupted, 1);
        JsonValue result{
            .data = JsonValue::Object{{"reason", {.data = std::string{"daemon_interrupted"}}},
                                      {"outcome_unknown", {.data = true}}}
        };
        expected.run.result                     = result;
        expected.attempts.back().attempt.result = result;
        expected.attempts.back().output =
            AttemptOutput{.stdout_bytes = ByteBuffer{}, .stderr_bytes = ByteBuffer{}, .capture_lost = true};
    }

    fixture.insert_run(expected);
    fixture.reopen();
    fixture.require_run(expected);
    Query schema{fixture.database};
    REQUIRE(schema.exec("SELECT version FROM jobu_schema"));
    auto next = schema.next();
    REQUIRE(next);
    REQUIRE(*next);
    CHECK(std::get<std::int64_t>(*schema.record().value("version")) == 1);
}

TEST_CASE("Recovery fixtures support owner states and both persisted recovery policies", "[jobu][recovery][sqlite]")
{
    auto const policy = GENERATE(RecoveryPolicy::FailInterrupted, RecoveryPolicy::RetryInterrupted);
    auto const state = GENERATE(QueueState::Active, QueueState::Suspending, QueueState::Suspended, QueueState::Deleted);
    RecoveryFixture fixture;
    auto            queue = recovery_queue(recovery_id(1), state, policy);
    auto            job   = fixture.make_job(recovery_id(2), queue.id);
    job.schedule          = CronSchedule{.expression = "* * * * *", .timezone = "UTC"};
    auto run_state        = RunState::Scheduled;
    if (state == QueueState::Suspending) {
        job.state = JobState::Suspending;
        run_state = RunState::Running;
    }
    else if (state == QueueState::Suspended) {
        job.state = JobState::Suspended;
        run_state = RunState::RetryWait;
    }
    else if (state == QueueState::Deleted) {
        job.state        = JobState::Deleted;
        job.deleted_at   = UtcTimePoint{30s};
        job.updated_at   = *job.deleted_at;
        queue.updated_at = *job.deleted_at;
        queue.deleted_at = *job.deleted_at;
        run_state        = RunState::Cancelled;
    }
    auto const failures = run_state == RunState::RetryWait || run_state == RunState::Cancelled ? 1U : 0U;
    auto       expected = fixture.make_run(recovery_id(3), job, run_state, failures);
    fixture.insert_queue(queue);
    fixture.insert_job(job);
    fixture.insert_run(expected);
    fixture.reopen();
    fixture.require_run(expected);

    QueueRepository queues{fixture.database, fixture.registry};
    auto            persisted_queue = queues.find_by_id(queue.id, true);
    REQUIRE(persisted_queue);
    REQUIRE(persisted_queue->has_value());
    CHECK((*persisted_queue)->name == queue.name);
    CHECK((*persisted_queue)->state == queue.state);
    CHECK((*persisted_queue)->recovery_policy == policy);
    CHECK((*persisted_queue)->deleted_at == queue.deleted_at);

    JobRepository jobs{fixture.database, fixture.registry};
    auto          persisted_job = jobs.find_by_id(job.id, true);
    REQUIRE(persisted_job);
    REQUIRE(persisted_job->has_value());
    CHECK((*persisted_job)->state == job.state);
    CHECK((*persisted_job)->deleted_at == job.deleted_at);
    CHECK(std::get<CronSchedule>((*persisted_job)->schedule).expression == "* * * * *");
}

TEST_CASE("Manual retry fixtures retain a barrier alongside the schedule-owned run", "[jobu][recovery][sqlite]")
{
    RecoveryFixture fixture;
    auto            queue = recovery_queue(recovery_id(1));
    auto            job   = fixture.make_job(recovery_id(2), queue.id);
    job.schedule          = CronSchedule{.expression = "* * * * *", .timezone = "UTC"};
    fixture.insert_queue(queue);
    fixture.insert_job(job);
    auto scheduled = fixture.make_run(recovery_id(3), job);
    auto manual    = fixture.make_run(recovery_id(4), job, RunState::RetryWait, 1, RunOrigin::Manual);
    fixture.insert_run(scheduled);
    fixture.insert_run(manual);
    fixture.reopen();
    fixture.require_run(scheduled);
    fixture.require_run(manual);

    SchedulerRepository scheduler{fixture.database, fixture.registry};
    auto                barriers = scheduler.list_manual_barriers(10, std::nullopt);
    REQUIRE(barriers);
    REQUIRE(barriers->size() == 1);
    CHECK(barriers->front().run_id == manual.run.id);
    auto runnable = scheduler.list_runnable(queue.id, job.type, UtcTimePoint{100s}, 10);
    REQUIRE(runnable);
    REQUIRE(runnable->size() == 1);
    CHECK(runnable->front().run.id == manual.run.id);
}

TEST_CASE("Cancelled execution and moved terminal history remain readable", "[jobu][recovery][sqlite]")
{
    RecoveryFixture fixture;
    auto            old_queue = recovery_queue(recovery_id(1), QueueState::Deleted);
    old_queue.updated_at      = UtcTimePoint{100s};
    old_queue.deleted_at      = old_queue.updated_at;
    auto new_queue            = recovery_queue(recovery_id(2));
    auto job                  = fixture.make_job(recovery_id(3), new_queue.id);
    job.revision              = 5;
    auto history              = fixture.make_run(recovery_id(4), job, RunState::Running, 1);

    // An observed cancellation completes the active attempt. A later move changes only nonterminal snapshots.
    history.run.queue_id     = old_queue.id;
    history.run.job_revision = 2;
    history.run.state        = RunState::Cancelled;
    history.run.completed_at = UtcTimePoint{22s};
    history.run.result       = JsonValue{.data = JsonValue::Object{}};
    auto& last               = history.attempts.back().attempt;
    last.state               = AttemptState::Completed;
    last.outcome             = AttemptOutcome::Cancelled;
    last.completed_at        = history.run.completed_at;
    last.result              = history.run.result;
    fixture.insert_queue(old_queue);
    fixture.insert_queue(new_queue);
    fixture.insert_job(job);
    fixture.insert_run(history);
    fixture.reopen();
    fixture.require_run(history);
}

TEST_CASE("Drained suspension fixtures preserve retry work while advancing owner metadata", "[jobu][recovery][sqlite]")
{
    RecoveryFixture fixture;
    auto            queue = recovery_queue(recovery_id(1), QueueState::Suspending);
    auto            job   = fixture.make_job(recovery_id(2), queue.id);
    job.state             = JobState::Suspending;
    job.revision          = 7;
    auto retry            = fixture.make_run(recovery_id(3), job, RunState::RetryWait, 1);
    fixture.insert_queue(queue);
    fixture.insert_job(job);
    fixture.insert_run(retry);

    // Deliberately seed the repair input: a Suspending owner whose last Running work is already gone.
    auto begun = Transaction::begin(fixture.database);
    REQUIRE(begun);
    auto                transaction = std::move(*begun);
    SchedulerRepository scheduler{fixture.database, fixture.registry};
    REQUIRE(scheduler.complete_drained_suspensions(queue.id, job.id, UtcTimePoint{100s}));
    REQUIRE(transaction.commit());
    fixture.reopen();
    fixture.require_run(retry);

    QueueRepository queues{fixture.database, fixture.registry};
    JobRepository   jobs{fixture.database, fixture.registry};
    auto            suspended_queue = queues.find_by_id(queue.id, true);
    auto            suspended_job   = jobs.find_by_id(job.id, true);
    REQUIRE(suspended_queue);
    REQUIRE(suspended_queue->has_value());
    REQUIRE(suspended_job);
    REQUIRE(suspended_job->has_value());
    CHECK((*suspended_queue)->state == QueueState::Suspended);
    CHECK((*suspended_queue)->updated_at == UtcTimePoint{100s});
    CHECK((*suspended_job)->state == JobState::Suspended);
    CHECK((*suspended_job)->revision == 8);
    CHECK((*suspended_job)->updated_at == UtcTimePoint{100s});
}

TEST_CASE("SQLite permits cross-row shapes that future recovery must reject", "[jobu][recovery][sqlite]")
{
    RecoveryFixture fixture;
    auto            queue = recovery_queue(recovery_id(1));
    auto            job   = fixture.make_job(recovery_id(2), queue.id);
    fixture.insert_queue(queue);
    fixture.insert_job(job);
    auto expected = fixture.make_run(recovery_id(3), job, RunState::Running);

    // Each section is intentionally inconsistent. Successful row decoding is not cross-row validation.
    SECTION("Running run without an attempt")
    {
        expected.attempts.clear();
    }
    SECTION("Multiple Running attempts")
    {
        auto duplicate                   = expected.attempts.front();
        duplicate.attempt.attempt_number = 2;
        expected.attempts.push_back(duplicate);
    }
    SECTION("Pending is representable but not emitted by dispatch")
    {
        expected = fixture.make_run(recovery_id(3), job);
        expected.attempts.push_back({
            .attempt = {.run_id = expected.run.id, .due_at = expected.run.runnable_at}
        });
    }
    SECTION("Output before completion")
    {
        expected.attempts.front().output = AttemptOutput{};
    }
    SECTION("Running attempt under a terminal run")
    {
        expected.run.state        = RunState::Succeeded;
        expected.run.completed_at = UtcTimePoint{12s};
    }
    SECTION("Run queue differs from its current owner")
    {
        auto other = recovery_queue(recovery_id(9));
        fixture.insert_queue(other);
        expected.run.queue_id = other.id;
    }
    SECTION("Deleted definition with Running work")
    {
        execute(fixture.database, "UPDATE jobu_jobs SET state = 'deleted', deleted_at_us = 30000000");
    }
    fixture.insert_run(expected);
    fixture.reopen();
    fixture.require_run(expected);
}

TEST_CASE("Recovery fixtures expose schema guards and application-only manual uniqueness", "[jobu][recovery][sqlite]")
{
    RecoveryFixture fixture;
    auto            queue = recovery_queue(recovery_id(1));
    auto            job   = fixture.make_job(recovery_id(2), queue.id);
    fixture.insert_queue(queue);
    fixture.insert_job(job);
    auto scheduled = fixture.make_run(recovery_id(3), job);
    fixture.insert_run(scheduled);

    SECTION("Partial unique index rejects a second schedule-owned nonterminal run")
    {
        auto manual = fixture.make_run(recovery_id(4), job, RunState::Scheduled, 0, RunOrigin::Manual);
        fixture.insert_run(manual);
        require_constraint(fixture.database,
                           "UPDATE jobu_runs SET origin = 'scheduled', schedule_owned = 1 WHERE origin = 'manual'",
                           "db.constraint.unique");
    }
    SECTION("Manual uniqueness is enforced by repositories rather than a schema index")
    {
        auto manual = fixture.make_run(recovery_id(4), job, RunState::Scheduled, 0, RunOrigin::Manual);
        fixture.insert_run(manual);
        auto          duplicate = fixture.make_run(recovery_id(5), job, RunState::Scheduled, 0, RunOrigin::Manual);
        RunRepository runs{fixture.database, fixture.registry};
        auto          rejected = runs.insert_manual(duplicate.run);
        REQUIRE_FALSE(rejected);
        CHECK(rejected.error().code == "jobu.run.manual_conflict");

        // Explicitly bypass that application guard to retain a duplicate-barrier fixture for Stage 7.3.
        fixture.insert_run(duplicate);
        fixture.require_run(manual);
        fixture.require_run(duplicate);
    }
    SECTION("Foreign keys reject orphan attempts and output")
    {
        require_constraint(
            fixture.database,
            "INSERT INTO jobu_attempts(run_id, attempt_number, due_at_us, state) VALUES(zeroblob(16), 1, 0, 'pending')",
            "db.constraint.foreign_key");
        require_constraint(
            fixture.database,
            "INSERT INTO jobu_attempt_output(run_id, attempt_number, stdout_truncated, stderr_truncated, capture_lost) "
            "SELECT id, 1, 0, 0, 0 FROM jobu_runs",
            "db.constraint.foreign_key");
    }
    SECTION("Attempt and output composite identities cannot be duplicated")
    {
        auto completed                    = fixture.make_run(recovery_id(4), job, RunState::Succeeded);
        completed.attempts.front().output = AttemptOutput{};
        fixture.insert_run(completed);
        require_constraint(fixture.database,
                           "INSERT INTO jobu_attempts SELECT * FROM jobu_attempts",
                           "db.constraint.unique");
        require_constraint(fixture.database,
                           "INSERT INTO jobu_attempt_output SELECT * FROM jobu_attempt_output",
                           "db.constraint.unique");
        require_constraint(fixture.database, "UPDATE jobu_attempts SET attempt_number = 0", "db.constraint");
    }
    SECTION("Schema rejects invalid enums but row decoders own state-field consistency")
    {
        require_constraint(fixture.database, "UPDATE jobu_runs SET state = 'unknown'", "db.constraint");
        execute(fixture.database, "UPDATE jobu_runs SET state = 'running'");
        RunRepository runs{fixture.database, fixture.registry};
        auto          invalid = runs.find_by_id(scheduled.run.id);
        REQUIRE_FALSE(invalid);
        CHECK(invalid.error().code == "jobu.storage.invariant");
    }
}
