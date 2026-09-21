#include "support/fake_cron_engine.hpp"
#include "support/recovery_fixture.hpp"
#include "support/sequence_uuid_generator.hpp"

#include "attribute_codec_priv.hpp"
#include "byte_buffer.hpp"
#include "job_repository_priv.hpp"
#include "job_validation_priv.hpp"
#include "json.hpp"
#include "query.hpp"
#include "queue_repository_priv.hpp"
#include "recovery_repository_priv.hpp"
#include "scheduler_repository_priv.hpp"
#include "transaction.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
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

struct RepairFixture {
    RecoveryFixture       storage;
    FakeCronEngine        cron;
    SequenceUuidGenerator generator{
        {recovery_id(100), recovery_id(101), recovery_id(102)}
    };
    RecoveryRepository recovery{storage.database, storage.registry};
};

auto recurring_job(RepairFixture const& fixture, Uuid queue_id, JobState state = JobState::Active) -> JobDefinition
{
    auto job     = fixture.storage.make_job(recovery_id(2), queue_id);
    job.schedule = CronSchedule{.expression = "* * * * *", .timezone = "UTC"};
    job.state    = state;
    if (state == JobState::Deleted) {
        job.deleted_at = UtcTimePoint{2s};
    }
    return job;
}

void execute(Database& database, std::string_view sql)
{
    INFO(sql);
    Query query{database};
    REQUIRE(query.exec(sql));
}

void update_definition(RepairFixture& fixture, JobDefinition const& job, JobRevision expected_revision)
{
    auto attributes = encode_and_serialize_attribute_document(fixture.storage.registry,
                                                              job.attributes,
                                                              AttributeScope::Job,
                                                              AttributeDocumentMode::Materialized);
    REQUIRE(attributes);
    auto payload = validate_and_serialize_job_payload(job.type, job.payload);
    REQUIRE(payload);
    JobRepository jobs{fixture.storage.database, fixture.storage.registry};
    auto          updated = jobs.update_definition(job, expected_revision, *attributes, *payload);
    REQUIRE(updated);
    REQUIRE(*updated);
}

auto interrupt(RepairFixture& fixture, RecoveryRunFixture original, UtcTimePoint time) -> RecoveryRunFixture
{
    // Compose just one transaction-local unit; the service's paged orchestration is Stage 7.7.
    auto key      = RecoveryAttemptKey{.run_id         = original.run.id,
                                       .attempt_number = original.attempts.back().attempt.attempt_number};
    auto decision = fixture.recovery.find_retry_decision(key, time);
    REQUIRE(decision);
    REQUIRE(fixture.recovery.interrupt_attempt(key, time));
    if (decision->retry) {
        REQUIRE(fixture.recovery.set_run_retry_wait(key, time, decision->retry->due_at));
        original.run.state       = RunState::RetryWait;
        original.run.runnable_at = decision->retry->due_at;
    }
    else {
        REQUIRE(fixture.recovery.set_run_interrupted(key, time));
        original.run.state        = RunState::Interrupted;
        original.run.completed_at = time;
    }
    auto result = parse_json(R"({"reason":"daemon_interrupted","outcome_unknown":true})");
    REQUIRE(result);
    auto& last                = original.attempts.back();
    last.attempt.state        = AttemptState::Completed;
    last.attempt.outcome      = AttemptOutcome::Interrupted;
    last.attempt.completed_at = time;
    last.attempt.result       = *result;
    last.output               = jb::jobu::detail::AttemptOutput{.stdout_bytes = ByteBuffer{},
                                                                .stderr_bytes = ByteBuffer{},
                                                                .capture_lost = true};
    if (!decision->retry) {
        original.run.result = *result;
    }
    return original;
}

void require_successor(RepairFixture& fixture, JobDefinition const& job, UtcTimePoint due)
{
    auto expected            = fixture.storage.make_run(recovery_id(100), job);
    expected.run.planned_at  = due;
    expected.run.runnable_at = due;
    fixture.storage.require_run(expected);
}

auto read_job(RepairFixture& fixture, Uuid id) -> JobDefinition
{
    JobRepository jobs{fixture.storage.database, fixture.storage.registry};
    auto          job = jobs.find_by_id(id, true);
    REQUIRE(job);
    REQUIRE(*job);
    return std::move(**job);
}

auto read_queue(RepairFixture& fixture, Uuid id) -> Queue
{
    QueueRepository queues{fixture.storage.database, fixture.storage.registry};
    auto            queue = queues.find_by_id(id, true);
    REQUIRE(queue);
    REQUIRE(*queue);
    return std::move(**queue);
}

void require_valid(RepairFixture& fixture)
{
    REQUIRE(fixture.recovery.list_runs(256));
    REQUIRE(fixture.recovery.list_attempts(256));
    REQUIRE(fixture.recovery.list_outputs(256));
    REQUIRE(fixture.recovery.list_jobs(256));
    REQUIRE(fixture.recovery.list_queues(256));
}

} // namespace

TEST_CASE("Interrupted recurrence uses the current definition and explicit recovery bound", "[jobu][recovery][sqlite]")
{
    auto const    recovery_time = UtcTimePoint{120s};
    auto const    fresh_now     = GENERATE(UtcTimePoint{60s}, UtcTimePoint{180s});
    auto const    lower_bound   = std::max(recovery_time, fresh_now);
    RepairFixture fixture;
    auto          queue = recovery_queue(recovery_id(1));
    auto          job   = recurring_job(fixture, queue.id);
    fixture.storage.insert_queue(queue);
    fixture.storage.insert_job(job);
    auto original           = fixture.storage.make_run(recovery_id(3), job, RunState::Running);
    // A backward wall-clock jump must not reuse normal completion's historical planned-time bound.
    original.run.planned_at = UtcTimePoint{600s};
    fixture.storage.insert_run(original);
    REQUIRE(fixture.recovery.find_run(original.run.id));

    auto latest     = fixture.storage.make_job(job.id, queue.id, JobType::Http);
    latest.revision = job.revision + 1;
    latest.schedule = CronSchedule{.expression = "*/2 * * * *", .timezone = "Europe/Tallinn"};
    latest.priority = 42;
    latest.attributes.at("retry.max_attempts").data = std::int64_t{7};
    latest.updated_at                               = UtcTimePoint{110s};
    fixture.cron.set_occurrences(std::get<CronSchedule>(latest.schedule),
                                 {UtcTimePoint{60s}, lower_bound, lower_bound + 60s, UtcTimePoint{660s}});
    RecoveryRunFixture recovered;
    {
        auto transaction = Transaction::begin(fixture.storage.database);
        REQUIRE(transaction);
        // Editing after candidate discovery must affect the new snapshot, never the interrupted one.
        update_definition(fixture, latest, job.revision);
        recovered     = interrupt(fixture, original, recovery_time);
        auto inserted = fixture.recovery.insert_interrupted_successor(original.run.id,
                                                                      lower_bound,
                                                                      fixture.cron,
                                                                      fixture.generator);
        REQUIRE(inserted);
        CHECK(*inserted);
        REQUIRE(transaction->commit());
    }
    fixture.storage.reopen();
    fixture.storage.require_run(recovered);
    require_successor(fixture, latest, lower_bound + 60s);
    REQUIRE(fixture.cron.next_calls().size() == 1);
    CHECK(fixture.cron.next_calls().front().exclusive_lower_bound == lower_bound);
    CHECK(fixture.cron.next_calls().front().schedule.expression == "*/2 * * * *");
    require_valid(fixture);
}

TEST_CASE("Missing recurring work is repaired for every nondeleted owner state", "[jobu][recovery][sqlite]")
{
    auto const    job_state   = GENERATE(JobState::Active, JobState::Suspending, JobState::Suspended);
    auto const    queue_state = GENERATE(QueueState::Active, QueueState::Suspending, QueueState::Suspended);
    RepairFixture fixture;
    auto          queue = recovery_queue(recovery_id(1), queue_state);
    auto          job   = recurring_job(fixture, queue.id, job_state);
    fixture.storage.insert_queue(queue);
    fixture.storage.insert_job(job);
    // Owners without any historical run are still discoverable repair candidates.
    auto page = fixture.recovery.list_jobs(1);
    REQUIRE(page);
    REQUIRE(page->size() == 1);
    auto latest = job;
    if (job_state != JobState::Suspending) {
        // Definition edits are supported only for Active/Suspended jobs.
        latest.revision += 1;
        latest.priority  = 19;
        latest.schedule  = CronSchedule{.expression = "*/5 * * * *", .timezone = "UTC"};
    }
    fixture.cron.set_occurrences(std::get<CronSchedule>(latest.schedule), {UtcTimePoint{300s}, UtcTimePoint{600s}});
    {
        auto transaction = Transaction::begin(fixture.storage.database);
        REQUIRE(transaction);
        if (job_state != JobState::Suspending) {
            update_definition(fixture, latest, job.revision);
        }
        auto inserted =
            fixture.recovery.repair_missing_successor(job.id, UtcTimePoint{300s}, fixture.cron, fixture.generator);
        REQUIRE(inserted);
        CHECK(*inserted);
        REQUIRE(transaction->commit());
    }
    fixture.storage.reopen();
    require_successor(fixture, latest, UtcTimePoint{600s});
    CHECK(read_job(fixture, job.id).state == job_state);
    CHECK(read_queue(fixture, queue.id).state == queue_state);
    {
        auto transaction = Transaction::begin(fixture.storage.database);
        REQUIRE(transaction);
        auto repeated =
            fixture.recovery.repair_missing_successor(job.id, UtcTimePoint{900s}, fixture.cron, fixture.generator);
        REQUIRE(repeated);
        CHECK_FALSE(*repeated);
        REQUIRE(transaction->commit());
    }
    require_successor(fixture, latest, UtcTimePoint{600s});
    CHECK(fixture.cron.next_calls().size() == 1);
    require_valid(fixture);
}

TEST_CASE("Missing-work repair preserves existing schedule-owned snapshots", "[jobu][recovery][sqlite]")
{
    auto const    state = GENERATE(RunState::Scheduled, RunState::RetryWait, RunState::Running);
    RepairFixture fixture;
    auto          queue = recovery_queue(recovery_id(1));
    auto          job   = recurring_job(fixture, queue.id);
    fixture.storage.insert_queue(queue);
    fixture.storage.insert_job(job);
    // Candidate discovery precedes an existing run becoming visible; repair must recheck absence.
    REQUIRE(fixture.recovery.list_jobs(1));
    auto existing = fixture.storage.make_run(recovery_id(3), job, state, state == RunState::RetryWait ? 1 : 0);
    fixture.storage.insert_run(existing);
    auto edited      = job;
    edited.revision += 1;
    edited.priority  = 50;
    {
        auto transaction = Transaction::begin(fixture.storage.database);
        REQUIRE(transaction);
        update_definition(fixture, edited, job.revision);
        auto repaired =
            fixture.recovery.repair_missing_successor(job.id, UtcTimePoint{600s}, fixture.cron, fixture.generator);
        REQUIRE(repaired);
        CHECK_FALSE(*repaired);
        REQUIRE(transaction->commit());
    }
    fixture.storage.require_run(existing);
    CHECK(fixture.cron.next_calls().empty());
    require_valid(fixture);
}

TEST_CASE("Recurrence repair excludes current Once and deleted definitions", "[jobu][recovery][sqlite]")
{
    auto const    deleted = GENERATE(false, true);
    RepairFixture fixture;
    auto          queue = recovery_queue(recovery_id(1), deleted ? QueueState::Deleted : QueueState::Active);
    auto          job   = deleted ? recurring_job(fixture, queue.id, JobState::Deleted)
                                  : fixture.storage.make_job(recovery_id(2), queue.id);
    fixture.storage.insert_queue(queue);
    fixture.storage.insert_job(job);
    auto terminal = fixture.storage.make_run(recovery_id(3), job, RunState::Interrupted);
    fixture.storage.insert_run(terminal);
    {
        auto transaction = Transaction::begin(fixture.storage.database);
        REQUIRE(transaction);
        auto terminal_result = fixture.recovery.insert_interrupted_successor(terminal.run.id,
                                                                             UtcTimePoint{120s},
                                                                             fixture.cron,
                                                                             fixture.generator);
        REQUIRE(terminal_result);
        CHECK_FALSE(*terminal_result);
        auto missing_result =
            fixture.recovery.repair_missing_successor(job.id, UtcTimePoint{120s}, fixture.cron, fixture.generator);
        REQUIRE(missing_result);
        CHECK_FALSE(*missing_result);
        REQUIRE(transaction->commit());
    }
    fixture.storage.require_run(terminal);
    CHECK(fixture.cron.validation_calls().empty());
    auto rows = fixture.recovery.list_runs(256);
    REQUIRE(rows);
    CHECK(rows->size() == 1);
    require_valid(fixture);
}

TEST_CASE("Manual interruption releases or retains its derived barrier without creating a successor",
          "[jobu][recovery][sqlite]")
{
    auto const    policy = GENERATE(RecoveryPolicy::FailInterrupted, RecoveryPolicy::RetryInterrupted);
    RepairFixture fixture;
    auto          queue = recovery_queue(recovery_id(1), QueueState::Suspending, policy);
    auto          job   = recurring_job(fixture, queue.id, JobState::Suspending);
    fixture.storage.insert_queue(queue);
    fixture.storage.insert_job(job);
    auto scheduled = fixture.storage.make_run(recovery_id(3), job);
    auto manual    = fixture.storage.make_run(recovery_id(4), job, RunState::Running, 0, RunOrigin::Manual);
    fixture.storage.insert_run(scheduled);
    fixture.storage.insert_run(manual);
    RecoveryRunFixture recovered;
    {
        auto transaction = Transaction::begin(fixture.storage.database);
        REQUIRE(transaction);
        recovered = interrupt(fixture, manual, UtcTimePoint{120s});
        if (policy == RecoveryPolicy::FailInterrupted) {
            auto successor = fixture.recovery.insert_interrupted_successor(manual.run.id,
                                                                           UtcTimePoint{120s},
                                                                           fixture.cron,
                                                                           fixture.generator);
            REQUIRE(successor);
            CHECK_FALSE(*successor);
        }
        auto queue_drained = fixture.recovery.complete_drained_queue_suspension(queue.id, UtcTimePoint{120s});
        auto job_drained   = fixture.recovery.complete_drained_job_suspension(job.id, UtcTimePoint{120s});
        REQUIRE(queue_drained);
        REQUIRE(job_drained);
        CHECK(*queue_drained);
        CHECK(*job_drained);
        REQUIRE(transaction->commit());
    }
    fixture.storage.reopen();
    fixture.storage.require_run(recovered);
    fixture.storage.require_run(scheduled);
    CHECK(fixture.cron.next_calls().empty());
    SchedulerRepository scheduler{fixture.storage.database, fixture.storage.registry};
    auto                barriers = scheduler.list_manual_barriers(256, {});
    REQUIRE(barriers);
    CHECK(barriers->size() == (policy == RecoveryPolicy::RetryInterrupted ? 1U : 0U));
    CHECK(read_job(fixture, job.id).state == JobState::Suspended);
    CHECK(read_queue(fixture, queue.id).state == QueueState::Suspended);
    require_valid(fixture);
}

TEST_CASE("Recovery drains recurring owners atomically after the final Running run leaves", "[jobu][recovery][sqlite]")
{
    auto const    policy = GENERATE(RecoveryPolicy::FailInterrupted, RecoveryPolicy::RetryInterrupted);
    RepairFixture fixture;
    auto          queue = recovery_queue(recovery_id(1), QueueState::Suspending, policy);
    auto          job   = recurring_job(fixture, queue.id, JobState::Suspending);
    fixture.storage.insert_queue(queue);
    fixture.storage.insert_job(job);
    auto original = fixture.storage.make_run(recovery_id(3), job, RunState::Running);
    fixture.storage.insert_run(original);
    fixture.cron.set_occurrences(std::get<CronSchedule>(job.schedule), {UtcTimePoint{180s}});
    RecoveryRunFixture recovered;
    {
        auto transaction = Transaction::begin(fixture.storage.database);
        REQUIRE(transaction);
        auto queue_waits = fixture.recovery.complete_drained_queue_suspension(queue.id, UtcTimePoint{100s});
        auto job_waits   = fixture.recovery.complete_drained_job_suspension(job.id, UtcTimePoint{100s});
        REQUIRE(queue_waits);
        REQUIRE(job_waits);
        CHECK_FALSE(*queue_waits);
        CHECK_FALSE(*job_waits);
        CHECK(read_job(fixture, job.id).revision == job.revision);
        CHECK(read_queue(fixture, queue.id).updated_at == queue.updated_at);

        recovered = interrupt(fixture, original, UtcTimePoint{120s});
        if (policy == RecoveryPolicy::FailInterrupted) {
            auto successor = fixture.recovery.insert_interrupted_successor(original.run.id,
                                                                           UtcTimePoint{120s},
                                                                           fixture.cron,
                                                                           fixture.generator);
            REQUIRE(successor);
            CHECK(*successor);
        }
        auto queue_drained = fixture.recovery.complete_drained_queue_suspension(queue.id, UtcTimePoint{120s});
        auto job_drained   = fixture.recovery.complete_drained_job_suspension(job.id, UtcTimePoint{120s});
        REQUIRE(queue_drained);
        REQUIRE(job_drained);
        CHECK(*queue_drained);
        CHECK(*job_drained);
        REQUIRE(transaction->commit());
    }
    fixture.storage.reopen();
    fixture.storage.require_run(recovered);
    if (policy == RecoveryPolicy::FailInterrupted) {
        // The successor records the definition before the suspension's revision increment.
        require_successor(fixture, job, UtcTimePoint{180s});
    }
    auto drained_job   = read_job(fixture, job.id);
    auto drained_queue = read_queue(fixture, queue.id);
    CHECK(drained_job.state == JobState::Suspended);
    CHECK(drained_job.revision == job.revision + 1);
    CHECK(drained_job.updated_at == UtcTimePoint{120s});
    CHECK(drained_queue.state == QueueState::Suspended);
    CHECK(drained_queue.updated_at == UtcTimePoint{120s});
    {
        auto transaction = Transaction::begin(fixture.storage.database);
        REQUIRE(transaction);
        auto queue_again = fixture.recovery.complete_drained_queue_suspension(queue.id, UtcTimePoint{180s});
        auto job_again   = fixture.recovery.complete_drained_job_suspension(job.id, UtcTimePoint{180s});
        REQUIRE(queue_again);
        REQUIRE(job_again);
        CHECK_FALSE(*queue_again);
        CHECK_FALSE(*job_again);
        REQUIRE(transaction->commit());
    }
    CHECK(read_job(fixture, job.id).updated_at == drained_job.updated_at);
    CHECK(read_job(fixture, job.id).revision == drained_job.revision);
    CHECK(read_queue(fixture, queue.id).updated_at == drained_queue.updated_at);
    SchedulerRepository scheduler{fixture.storage.database, fixture.storage.registry};
    auto                runnable = scheduler.list_runnable(queue.id, job.type, UtcTimePoint{600s}, 256);
    REQUIRE(runnable);
    CHECK(runnable->empty());
    require_valid(fixture);
}

TEST_CASE("Suspension repair includes empty queues and jobs without interrupted rows", "[jobu][recovery][sqlite]")
{
    RepairFixture fixture;
    auto          empty_queue = recovery_queue(recovery_id(1), QueueState::Suspending);
    auto          queue       = recovery_queue(recovery_id(5), QueueState::Suspending);
    auto          job         = fixture.storage.make_job(recovery_id(2), queue.id);
    job.state                 = JobState::Suspending;
    auto waiting_job          = fixture.storage.make_job(recovery_id(6), queue.id);
    waiting_job.state         = JobState::Suspending;
    fixture.storage.insert_queue(empty_queue);
    fixture.storage.insert_queue(queue);
    fixture.storage.insert_job(job);
    fixture.storage.insert_job(waiting_job);
    auto waiting = fixture.storage.make_run(recovery_id(7), waiting_job, RunState::RetryWait, 1);
    fixture.storage.insert_run(waiting);
    // These scans include every owner even though the Running scan has no candidates.
    auto running = fixture.recovery.list_runs(256, {}, RunState::Running);
    REQUIRE(running);
    CHECK(running->empty());
    auto jobs   = fixture.recovery.list_jobs(256);
    auto queues = fixture.recovery.list_queues(256);
    REQUIRE(jobs);
    REQUIRE(queues);
    CHECK(jobs->size() == 2);
    CHECK(queues->size() == 2);
    {
        auto transaction = Transaction::begin(fixture.storage.database);
        REQUIRE(transaction);
        for (auto const& owner : *queues) {
            auto drained = fixture.recovery.complete_drained_queue_suspension(owner.id, UtcTimePoint{120s});
            REQUIRE(drained);
            CHECK(*drained);
        }
        for (auto const& owner : *jobs) {
            auto drained = fixture.recovery.complete_drained_job_suspension(owner.id, UtcTimePoint{120s});
            REQUIRE(drained);
            CHECK(*drained);
        }
        REQUIRE(transaction->commit());
    }
    fixture.storage.reopen();
    fixture.storage.require_run(waiting);
    CHECK(read_queue(fixture, empty_queue.id).state == QueueState::Suspended);
    CHECK(read_queue(fixture, queue.id).state == QueueState::Suspended);
    CHECK(read_job(fixture, job.id).state == JobState::Suspended);
    CHECK(read_job(fixture, waiting_job.id).state == JobState::Suspended);
    require_valid(fixture);
}

TEST_CASE("A different job's Running work keeps its queue suspending", "[jobu][recovery][sqlite]")
{
    RepairFixture fixture;
    auto          queue       = recovery_queue(recovery_id(1), QueueState::Suspending);
    auto          drained_job = fixture.storage.make_job(recovery_id(2), queue.id);
    drained_job.state         = JobState::Suspending;
    auto busy_job             = fixture.storage.make_job(recovery_id(3), queue.id);
    busy_job.state            = JobState::Suspending;
    fixture.storage.insert_queue(queue);
    fixture.storage.insert_job(drained_job);
    fixture.storage.insert_job(busy_job);
    auto running = fixture.storage.make_run(recovery_id(4), busy_job, RunState::Running);
    fixture.storage.insert_run(running);
    {
        auto transaction = Transaction::begin(fixture.storage.database);
        REQUIRE(transaction);
        auto first  = fixture.recovery.complete_drained_job_suspension(drained_job.id, UtcTimePoint{120s});
        auto second = fixture.recovery.complete_drained_job_suspension(busy_job.id, UtcTimePoint{120s});
        auto owner  = fixture.recovery.complete_drained_queue_suspension(queue.id, UtcTimePoint{120s});
        REQUIRE(first);
        REQUIRE(second);
        REQUIRE(owner);
        CHECK(*first);
        CHECK_FALSE(*second);
        CHECK_FALSE(*owner);
        REQUIRE(transaction->commit());
    }
    CHECK(read_job(fixture, drained_job.id).state == JobState::Suspended);
    CHECK(read_job(fixture, busy_job.id).state == JobState::Suspending);
    CHECK(read_queue(fixture, queue.id).state == QueueState::Suspending);
    fixture.storage.require_run(running);
}

TEST_CASE("Repair never resumes or revises owners outside Suspending", "[jobu][recovery][sqlite]")
{
    auto const job_state   = GENERATE(JobState::Active, JobState::Suspended, JobState::Deleted);
    auto       queue_state = QueueState::Suspended;
    if (job_state == JobState::Active) {
        queue_state = QueueState::Active;
    }
    else if (job_state == JobState::Deleted) {
        queue_state = QueueState::Deleted;
    }
    RepairFixture fixture;
    auto          queue = recovery_queue(recovery_id(1), queue_state);
    auto          job   = recurring_job(fixture, queue.id, job_state);
    fixture.storage.insert_queue(queue);
    fixture.storage.insert_job(job);
    auto before_job   = read_job(fixture, job.id);
    auto before_queue = read_queue(fixture, queue.id);
    {
        auto transaction = Transaction::begin(fixture.storage.database);
        REQUIRE(transaction);
        auto first  = fixture.recovery.complete_drained_job_suspension(job.id, UtcTimePoint{120s});
        auto second = fixture.recovery.complete_drained_queue_suspension(queue.id, UtcTimePoint{120s});
        REQUIRE(first);
        REQUIRE(second);
        CHECK_FALSE(*first);
        CHECK_FALSE(*second);
        REQUIRE(transaction->commit());
    }
    CHECK(read_job(fixture, job.id).state == before_job.state);
    CHECK(read_job(fixture, job.id).revision == before_job.revision);
    CHECK(read_job(fixture, job.id).updated_at == before_job.updated_at);
    CHECK(read_queue(fixture, queue.id).state == before_queue.state);
    CHECK(read_queue(fixture, queue.id).updated_at == before_queue.updated_at);
}

TEST_CASE("Recurrence failures roll back interruption and never treat a conflict as success",
          "[jobu][recovery][sqlite]")
{
    enum class Failure : std::uint8_t {
        Cron,
        Uuid,
        Constraint
    };
    auto const    failure = GENERATE(Failure::Cron, Failure::Uuid, Failure::Constraint);
    auto const    missing = GENERATE(false, true);
    RepairFixture fixture;
    auto          queue = recovery_queue(recovery_id(1));
    auto          job   = recurring_job(fixture, queue.id);
    fixture.storage.insert_queue(queue);
    fixture.storage.insert_job(job);
    auto original = fixture.storage.make_run(recovery_id(3), job, RunState::Running);
    if (!missing) {
        fixture.storage.insert_run(original);
    }
    fixture.cron.set_occurrences(std::get<CronSchedule>(job.schedule), {UtcTimePoint{180s}});
    auto expected_code = std::string{};
    if (failure == Failure::Cron) {
        expected_code = "test.cron.failure";
        fixture.cron.set_next_error(Error{.category = ErrorCategory::ResourceExhausted,
                                          .code     = expected_code,
                                          .message  = "sensitive test payload",
                                          .detail   = "sensitive SQL"});
    }
    else if (failure == Failure::Uuid) {
        expected_code     = "test.uuid.sequence_exhausted";
        fixture.generator = SequenceUuidGenerator{{}};
    }
    else {
        expected_code = "db.constraint.unique";
        // Insert a conflicting schedule-owned row only after the normal absence check. The
        // unique index must reject the requested insert and rollback both rows with the unit.
        execute(fixture.storage.database,
                "CREATE TRIGGER conflicting_successor BEFORE INSERT ON jobu_runs "
                "WHEN NEW.id <> zeroblob(16) BEGIN "
                "INSERT INTO jobu_runs(id, job_id, job_revision, queue_id, origin, schedule_owned, planned_at_us, "
                "runnable_at_us, type, priority, attributes_json, payload_json, state) "
                "VALUES(zeroblob(16), NEW.job_id, NEW.job_revision, NEW.queue_id, 'scheduled', 1, "
                "NEW.planned_at_us, NEW.runnable_at_us, NEW.type, NEW.priority, NEW.attributes_json, NEW.payload_json, "
                "'scheduled'); END");
    }
    {
        auto transaction = Transaction::begin(fixture.storage.database);
        REQUIRE(transaction);
        if (!missing) {
            static_cast<void>(interrupt(fixture, original, UtcTimePoint{120s}));
        }
        auto result =
            missing
                ? fixture.recovery.repair_missing_successor(job.id, UtcTimePoint{120s}, fixture.cron, fixture.generator)
                : fixture.recovery.insert_interrupted_successor(original.run.id,
                                                                UtcTimePoint{120s},
                                                                fixture.cron,
                                                                fixture.generator);
        REQUIRE_FALSE(result);
        CHECK(result.error().code == expected_code);
        CHECK(result.error().message.find("sensitive") == std::string::npos);
        CHECK(result.error().detail.find("sensitive") == std::string::npos);
        // RAII rolls back the complete unit after the failing helper returns.
    }
    fixture.storage.reopen();
    if (!missing) {
        fixture.storage.require_run(original);
    }
    auto runs = fixture.recovery.list_runs(256);
    REQUIRE(runs);
    CHECK(runs->size() == (missing ? 0U : 1U));
    require_valid(fixture);
}

TEST_CASE("Suspension failure rolls back interruption capture successor and earlier queue drain",
          "[jobu][recovery][sqlite]")
{
    enum class Failure : std::uint8_t {
        Revision,
        Write,
        AffectedRows
    };
    auto const    failure = GENERATE(Failure::Revision, Failure::Write, Failure::AffectedRows);
    RepairFixture fixture;
    auto          queue = recovery_queue(recovery_id(1), QueueState::Suspending);
    auto          job   = recurring_job(fixture, queue.id, JobState::Suspending);
    if (failure == Failure::Revision) {
        job.revision = static_cast<JobRevision>(std::numeric_limits<std::int64_t>::max());
    }
    fixture.storage.insert_queue(queue);
    fixture.storage.insert_job(job);
    auto original = fixture.storage.make_run(recovery_id(3), job, RunState::Running);
    fixture.storage.insert_run(original);
    fixture.cron.set_occurrences(std::get<CronSchedule>(job.schedule), {UtcTimePoint{180s}});
    if (failure == Failure::Write) {
        execute(fixture.storage.database,
                "CREATE TRIGGER fail_job_drain BEFORE UPDATE OF state ON jobu_jobs "
                "WHEN NEW.state = 'suspended' BEGIN SELECT RAISE(ABORT, 'sensitive SQL'); END");
    }
    else if (failure == Failure::AffectedRows) {
        execute(fixture.storage.database,
                "CREATE TRIGGER ignore_job_drain BEFORE UPDATE OF state ON jobu_jobs "
                "WHEN NEW.state = 'suspended' BEGIN SELECT RAISE(IGNORE); END");
    }
    {
        auto transaction = Transaction::begin(fixture.storage.database);
        REQUIRE(transaction);
        static_cast<void>(interrupt(fixture, original, UtcTimePoint{120s}));
        auto successor = fixture.recovery.insert_interrupted_successor(original.run.id,
                                                                       UtcTimePoint{120s},
                                                                       fixture.cron,
                                                                       fixture.generator);
        REQUIRE(successor);
        REQUIRE(*successor);
        auto queue_drained = fixture.recovery.complete_drained_queue_suspension(queue.id, UtcTimePoint{120s});
        REQUIRE(queue_drained);
        REQUIRE(*queue_drained);
        auto failed = fixture.recovery.complete_drained_job_suspension(job.id, UtcTimePoint{120s});
        REQUIRE_FALSE(failed);
        auto expected_code = std::string_view{"jobu.recovery.invariant"};
        if (failure == Failure::Revision) {
            expected_code = "jobu.job.revision_exhausted";
        }
        else if (failure == Failure::Write) {
            expected_code = "db.constraint";
        }
        CHECK(failed.error().code == expected_code);
        CHECK(failed.error().detail.find("sensitive") == std::string::npos);
    }
    fixture.storage.reopen();
    fixture.storage.require_run(original);
    auto after_job = read_job(fixture, job.id);
    CHECK(after_job.state == JobState::Suspending);
    CHECK(after_job.revision == job.revision);
    CHECK(after_job.updated_at == job.updated_at);
    auto after_queue = read_queue(fixture, queue.id);
    CHECK(after_queue.state == QueueState::Suspending);
    CHECK(after_queue.updated_at == queue.updated_at);
    auto runs = fixture.recovery.list_runs(256);
    REQUIRE(runs);
    CHECK(runs->size() == 1);
    require_valid(fixture);
}

TEST_CASE("Missing-work repair rechecks a definition changed to Once after candidate discovery",
          "[jobu][recovery][sqlite]")
{
    RepairFixture fixture;
    auto          queue = recovery_queue(recovery_id(1));
    auto          job   = recurring_job(fixture, queue.id);
    fixture.storage.insert_queue(queue);
    fixture.storage.insert_job(job);
    REQUIRE(fixture.recovery.list_jobs(1));
    {
        auto transaction = Transaction::begin(fixture.storage.database);
        REQUIRE(transaction);
        auto once      = job;
        once.revision += 1;
        once.schedule  = OnceSchedule{UtcTimePoint{600s}};
        update_definition(fixture, once, job.revision);
        auto result =
            fixture.recovery.repair_missing_successor(job.id, UtcTimePoint{120s}, fixture.cron, fixture.generator);
        REQUIRE(result);
        CHECK_FALSE(*result);
        REQUIRE(transaction->commit());
    }
    auto rows = fixture.recovery.list_runs(256);
    REQUIRE(rows);
    CHECK(rows->empty());
    CHECK(fixture.cron.next_calls().empty());
}

TEST_CASE("Terminal successor insertion rejects retrying runs and duplicate successors", "[jobu][recovery][sqlite]")
{
    auto const    retry = GENERATE(false, true);
    RepairFixture fixture;
    auto          queue = recovery_queue(recovery_id(1));
    auto          job   = recurring_job(fixture, queue.id);
    fixture.storage.insert_queue(queue);
    fixture.storage.insert_job(job);
    auto original = fixture.storage.make_run(recovery_id(3),
                                             job,
                                             retry ? RunState::RetryWait : RunState::Interrupted,
                                             retry ? 1 : 0);
    fixture.storage.insert_run(original);
    if (!retry) {
        fixture.storage.insert_run(fixture.storage.make_run(recovery_id(4), job));
    }
    fixture.cron.set_occurrences(std::get<CronSchedule>(job.schedule), {UtcTimePoint{180s}});
    {
        auto transaction = Transaction::begin(fixture.storage.database);
        REQUIRE(transaction);
        auto result = fixture.recovery.insert_interrupted_successor(original.run.id,
                                                                    UtcTimePoint{120s},
                                                                    fixture.cron,
                                                                    fixture.generator);
        REQUIRE_FALSE(result);
        CHECK(result.error().code == "jobu.recovery.invariant");
        CHECK(result.error().detail ==
              (retry ? "reason=successor_requires_interrupted_run" : "reason=unexpected_successor_conflict"));
    }
    fixture.storage.require_run(original);
    if (!retry) {
        fixture.storage.require_run(fixture.storage.make_run(recovery_id(4), job));
    }
}

TEST_CASE("Recovery rejects a cron engine returning a nonfuture successor", "[jobu][recovery][sqlite]")
{
    // Deliberately violate the injected engine contract to exercise the shared strict-bound guard.
    class NonFutureCron final : public CronEngine {
    public:
        auto validate(CronSchedule const& /*schedule*/) const -> Result<void, Error> override
        {
            return Result<void, Error>::success();
        }

        auto next_after(CronSchedule const& /*schedule*/, UtcTimePoint lower_bound) const
            -> Result<UtcTimePoint, Error> override
        {
            return Result<UtcTimePoint, Error>::success(lower_bound);
        }
    } cron;

    RepairFixture fixture;
    auto          queue = recovery_queue(recovery_id(1));
    auto          job   = recurring_job(fixture, queue.id);
    fixture.storage.insert_queue(queue);
    fixture.storage.insert_job(job);
    {
        auto transaction = Transaction::begin(fixture.storage.database);
        REQUIRE(transaction);
        auto result = fixture.recovery.repair_missing_successor(job.id, UtcTimePoint{120s}, cron, fixture.generator);
        REQUIRE_FALSE(result);
        CHECK(result.error().code == "jobu.recovery.invariant");
    }
    auto rows = fixture.recovery.list_runs(256);
    REQUIRE(rows);
    CHECK(rows->empty());
}

TEST_CASE("Recovery repairs revalidate owner existence and timestamp representation", "[jobu][recovery][sqlite]")
{
    auto const    commit = GENERATE(false, true);
    RepairFixture fixture;
    auto          queue = recovery_queue(recovery_id(1), QueueState::Suspending);
    auto          job   = recurring_job(fixture, queue.id, JobState::Suspending);
    fixture.storage.insert_queue(queue);
    fixture.storage.insert_job(job);
    {
        auto transaction = Transaction::begin(fixture.storage.database);
        REQUIRE(transaction);
        auto no_job       = fixture.recovery.repair_missing_successor(recovery_id(20),
                                                                      UtcTimePoint{120s},
                                                                      fixture.cron,
                                                                      fixture.generator);
        auto no_queue     = fixture.recovery.complete_drained_queue_suspension(recovery_id(20), UtcTimePoint{120s});
        auto no_job_drain = fixture.recovery.complete_drained_job_suspension(recovery_id(20), UtcTimePoint{120s});
        REQUIRE_FALSE(no_job);
        REQUIRE_FALSE(no_queue);
        REQUIRE_FALSE(no_job_drain);
        CHECK(no_job.error().code == "jobu.recovery.invariant");
        CHECK(no_queue.error().code == "jobu.recovery.invariant");
        CHECK(no_job_drain.error().code == "jobu.recovery.invariant");
    }

    // A nanosecond clock's minimum can floor below its decodable range. A microsecond clock's
    // minimum (macOS) is exact and must survive storage/reopen instead of being rejected.
    auto const minimum       = UtcTimePoint::min();
    auto const exact_minimum = std::chrono::floor<std::chrono::microseconds>(minimum.time_since_epoch()) ==
                               std::chrono::ceil<std::chrono::microseconds>(minimum.time_since_epoch());
    fixture.cron.set_occurrences(std::get<CronSchedule>(job.schedule), {UtcTimePoint{180s}});
    {
        auto transaction = Transaction::begin(fixture.storage.database);
        REQUIRE(transaction);
        auto job_drain   = fixture.recovery.complete_drained_job_suspension(job.id, minimum);
        auto queue_drain = fixture.recovery.complete_drained_queue_suspension(queue.id, minimum);
        auto recurrence  = fixture.recovery.repair_missing_successor(job.id, minimum, fixture.cron, fixture.generator);
        if (exact_minimum) {
            REQUIRE(job_drain);
            REQUIRE(queue_drain);
            REQUIRE(recurrence);
            CHECK(*job_drain);
            CHECK(*queue_drain);
            CHECK(*recurrence);
            CHECK(read_job(fixture, job.id).updated_at == minimum);
            CHECK(read_queue(fixture, queue.id).updated_at == minimum);
            REQUIRE(fixture.cron.next_calls().size() == 1);
            CHECK(fixture.cron.next_calls().front().exclusive_lower_bound == minimum);
        }
        else {
            REQUIRE_FALSE(job_drain);
            REQUIRE_FALSE(queue_drain);
            REQUIRE_FALSE(recurrence);
            CHECK(job_drain.error().detail == "reason=timestamp_out_of_range");
            CHECK(queue_drain.error().detail == "reason=timestamp_out_of_range");
            CHECK(recurrence.error().detail == "reason=timestamp_out_of_range");
            CHECK(fixture.cron.next_calls().empty());
        }
        if (commit) {
            REQUIRE(transaction->commit());
        }
    }

    // Reopen after both commit and rollback: rejected repairs must never leave partial writes.
    fixture.storage.reopen();
    auto const persisted_job   = read_job(fixture, job.id);
    auto const persisted_queue = read_queue(fixture, queue.id);
    auto const changed         = exact_minimum && commit;
    CHECK(persisted_job.state == (changed ? JobState::Suspended : JobState::Suspending));
    CHECK(persisted_job.revision == job.revision + (changed ? 1 : 0));
    CHECK(persisted_job.updated_at == (changed ? minimum : job.updated_at));
    CHECK(persisted_queue.state == (changed ? QueueState::Suspended : QueueState::Suspending));
    CHECK(persisted_queue.updated_at == (changed ? minimum : queue.updated_at));
    if (changed) {
        require_successor(fixture, persisted_job, UtcTimePoint{180s});
    }
    else {
        auto runs = fixture.recovery.list_runs(256);
        REQUIRE(runs);
        CHECK(runs->empty());
    }
}

TEST_CASE("Queue-only repair reports malformed durable owners as recovery invariants", "[jobu][recovery][sqlite]")
{
    RepairFixture fixture;
    auto          queue = recovery_queue(recovery_id(1), QueueState::Suspending);
    fixture.storage.insert_queue(queue);
    {
        auto transaction = Transaction::begin(fixture.storage.database);
        REQUIRE(transaction);
        // The schema permits text that the domain decoder rejects; the repair must not drain it.
        execute(fixture.storage.database, "UPDATE jobu_queues SET name = char(10)");
        auto result = fixture.recovery.complete_drained_queue_suspension(queue.id, UtcTimePoint{120s});
        REQUIRE_FALSE(result);
        CHECK(result.error().code == "jobu.recovery.invariant");
        CHECK(result.error().detail == "reason=durable_decode");
    }
    CHECK(read_queue(fixture, queue.id).state == QueueState::Suspending);
    CHECK(read_queue(fixture, queue.id).updated_at == queue.updated_at);
}
