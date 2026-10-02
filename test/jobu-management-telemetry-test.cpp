#include "execution_telemetry.hpp"

#include "domain_storage_priv.hpp"
#include "execution_telemetry_priv.hpp"
#include "management.hpp"
#include "query.hpp"
#include "run_repository_priv.hpp"
#include "scheduler.hpp"
#include "scheduler_core_priv.hpp"

#include "support/catch_utils.hpp" // IWYU pragma: keep for Catch::StringMaker specializations
#include "support/fake_attempt_executor.hpp"
#include "support/fake_cron_engine.hpp"
#include "support/fake_event_loop_backend.hpp"
#include "support/fake_time_source.hpp"
#include "support/fault_database_driver.hpp"
#include "support/recovery_fixture.hpp"
#include "support/rejecting_secret_provider.hpp"
#include "support/sequence_uuid_generator.hpp"
#include "support/storage_fault_helpers.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace jb::core;
using namespace jb::jobu;
using namespace jb::jobu::detail;
using namespace jb::test;
using namespace std::chrono_literals;

namespace {

auto identities() -> std::vector<Uuid>
{
    std::vector<Uuid> ids;
    for (std::uint32_t i = 10000; i < 10100; ++i) {
        ids.push_back(recovery_id(i));
    }
    return ids;
}

auto fault_boundary(std::string_view sql) -> std::string_view
{
    if (sql.starts_with("UPDATE jobu_run_timing")) {
        return "timing.write";
    }
    if (sql.starts_with("UPDATE jobu_queues")) {
        return "queue.write";
    }
    if (sql.starts_with("UPDATE jobu_jobs")) {
        return "job.write";
    }
    if (sql.starts_with("UPDATE jobu_runs")) {
        return "run.write";
    }
    return "read";
}

struct Fixture {
    Fixture(bool active = true, bool cli = true, bool http = false, bool supply = true)
    {
        time.set_utc(UtcTimePoint{100s});
        time.set_monotonic(TimePoint{1000s});
        executor.set_available(JobType::Cli, cli);
        executor.set_available(JobType::Http, http);
        telemetry = std::make_unique<ExecutionTelemetry>(storage.database, storage.registry, time, generator);
        // Construction registers capabilities through the real public scheduler seam. Keep dispatch
        // stopped: dispatch/completion timing belongs to the following implementation stage.
        scheduler = std::make_unique<Scheduler>(storage.database,
                                                storage.registry,
                                                cron,
                                                generator,
                                                time,
                                                executor,
                                                secrets,
                                                SchedulerOptions{.telemetry = telemetry.get()});
        if (active) {
            REQUIRE(telemetry->start());
        }
        service = std::make_unique<ManagementService>(
            storage.database,
            storage.registry,
            cron,
            generator,
            time,
            ManagementServiceOptions{.telemetry = supply ? telemetry.get() : nullptr});
        faults->classify = [](std::string_view sql) { return std::string{fault_boundary(sql)}; };
        telemetry->failed.connect(&receiver, [this](Error const& error) {
            telemetry_failures.push_back(error);
            calls_at_failure = faults->calls;
        });
        service->failed.connect(&receiver, [this](Error const& error) { management_failures.push_back(error); });
        service->mutation_committed.connect(&receiver, [this] { ++notifications; });
    }

    auto queue(std::string name = "queue") const -> Queue
    {
        auto created = service->create_queue({.name = std::move(name)});
        REQUIRE(created);
        return *created;
    }

    auto request(Queue const&        queue,
                 JobCreationSchedule schedule = ImmediateSchedule{},
                 JobType             type     = JobType::Cli) const -> CreateJobRequest
    {
        auto model = storage.make_job(recovery_id(9000), queue.id, type);
        return {
            .queue    = queue.id,
            .name     = "job",
            .type     = type,
            .schedule = std::move(schedule),
            .payload  = model.payload,
        };
    }

    auto job(Queue const& queue, JobCreationSchedule schedule = ImmediateSchedule{}, JobType type = JobType::Cli) const
        -> JobDefinition
    {
        auto created = service->create_job(request(queue, std::move(schedule), type));
        REQUIRE(created);
        return *created;
    }

    auto scheduled(Uuid id) -> JobRun
    {
        RunRepository runs{storage.database, storage.registry};
        auto          found = runs.find_schedule_owned(id);
        REQUIRE(found);
        REQUIRE(found->has_value());
        return **found;
    }

    auto timing(Uuid id) -> WaitTiming
    {
        WaitRepository repository{storage.database};
        auto           row = repository.read(id);
        REQUIRE(row);
        return *row;
    }

    void sql(std::string_view text)
    {
        jb::db::Query query{storage.database};
        REQUIRE(query.exec(text));
    }

    auto cancel(Uuid id) -> Result<CancelRunResult, Error>
    {
        SchedulerCore core{storage.database,
                           storage.registry,
                           cron,
                           generator,
                           time,
                           executor,
                           secrets,
                           {.telemetry = telemetry.get()}};
        return core.cancel_run(id);
    }

    jb::core::priv::FakeEventLoop          loop{jb::core::priv::make_fake_event_loop()};
    jb::core::priv::ScopedCurrentEventLoop current{loop.loop.get()};
    std::shared_ptr<DatabaseFaultState>    faults{std::make_shared<DatabaseFaultState>()};
    RecoveryFixture                        storage{[this](std::unique_ptr<jb::db::Driver> driver) {
        return std::make_unique<FaultDatabaseDriver>(std::move(driver), faults);
    }};
    FakeCronEngine                         cron;
    FakeTimeSource                         time;
    SequenceUuidGenerator                  generator{identities()};
    FakeAttemptExecutor                    executor;
    RejectingSecretProvider                secrets;
    Object                                 receiver;
    std::vector<Error>                     telemetry_failures;
    std::vector<Error>                     management_failures;
    std::vector<DatabaseCall>              calls_at_failure;
    std::size_t                            notifications{0};
    std::unique_ptr<ExecutionTelemetry>    telemetry;
    std::unique_ptr<Scheduler>             scheduler;
    std::unique_ptr<ManagementService>     service;
};

} // namespace

TEST_CASE("Covered creation distinguishes complete eligibility from future or unavailable work",
          "[jobu][telemetry][management]")
{
    Fixture f;
    auto    queue       = f.queue();
    auto    due         = f.scheduled(f.job(queue).id);
    auto    future      = f.scheduled(f.job(queue, OnceSchedule{.planned_at = UtcTimePoint{200s}}).id);
    auto    unavailable = f.scheduled(f.job(queue, ImmediateSchedule{}, JobType::Http).id);
    CHECK(f.timing(due.id).quality == WaitQuality::Complete);
    CHECK(f.timing(due.id).open_tick_us == 0);
    CHECK(f.timing(future.id).quality == WaitQuality::Complete);
    CHECK_FALSE(f.timing(future.id).open_epoch);
    CHECK(f.timing(unavailable.id).quality == WaitQuality::Complete);
    CHECK_FALSE(f.timing(unavailable.id).open_epoch);
    CHECK(f.executor.start_requests().empty());
}

TEST_CASE("Job and queue suspension settle and reopen at one monotonic boundary", "[jobu][telemetry][management]")
{
    auto    queue_scope = GENERATE(false, true);
    Fixture f;
    auto    queue = f.queue();
    auto    job   = f.job(queue);
    auto    run   = f.scheduled(job.id);
    f.time.advance(3s);
    if (queue_scope) {
        REQUIRE(f.service->suspend_queue(queue.id));
    }
    else {
        REQUIRE(f.service->suspend_job(job.id));
    }
    CHECK(f.timing(run.id).runnable_wait_us == 3000000);
    CHECK_FALSE(f.timing(run.id).open_epoch);

    f.time.advance(50s);
    if (queue_scope) {
        REQUIRE(f.service->resume_queue(queue.id));
    }
    else {
        REQUIRE(f.service->resume_job(job.id));
    }
    CHECK(f.timing(run.id).open_tick_us == 53000000);
    f.time.advance(2s);
    REQUIRE(f.service->suspend_queue(queue.id));
    CHECK(f.timing(run.id).runnable_wait_us == 5000000);
}

TEST_CASE("Accepted manual work bypasses job suspension but obeys queue suspension", "[jobu][telemetry][management]")
{
    Fixture f;
    auto    queue     = f.queue();
    auto    job       = f.job(queue, OnceSchedule{.planned_at = UtcTimePoint{200s}});
    auto    scheduled = f.scheduled(job.id);
    REQUIRE(f.service->suspend_job(job.id));
    auto manual = f.service->run_now({.job_id = job.id});
    REQUIRE(manual);
    CHECK(f.timing(manual->id).quality == WaitQuality::Complete);
    CHECK(f.timing(manual->id).open_epoch);
    f.time.advance(2s);
    REQUIRE(f.service->resume_job(job.id));
    CHECK(f.timing(manual->id).runnable_wait_us == 2000000);
    CHECK_FALSE(f.timing(scheduled.id).open_epoch);
    f.time.advance(3s);
    REQUIRE(f.service->suspend_queue(queue.id));
    CHECK(f.timing(manual->id).runnable_wait_us == 5000000);
    CHECK_FALSE(f.timing(manual->id).open_epoch);
    f.time.advance(20s);
    REQUIRE(f.service->resume_queue(queue.id));
    CHECK(f.timing(manual->id).open_tick_us == 25000000);
}

TEST_CASE("Pending manual cancellation releases its due scheduled sibling atomically", "[jobu][telemetry][management]")
{
    Fixture f;
    auto    queue     = f.queue();
    auto    job       = f.job(queue, OnceSchedule{.planned_at = UtcTimePoint{110s}});
    auto    scheduled = f.scheduled(job.id);
    auto    manual    = f.service->run_now({.job_id = job.id});
    REQUIRE(manual);
    f.time.advance(15s);
    auto cancelled = f.cancel(manual->id);
    REQUIRE(cancelled);
    CHECK(cancelled->run.state == RunState::Cancelled);
    CHECK(f.timing(manual->id).runnable_wait_us == 15000000);
    CHECK_FALSE(f.timing(manual->id).open_epoch);
    CHECK(f.timing(scheduled.id).open_tick_us == 15000000);
    f.time.advance(1s);
    REQUIRE(f.service->suspend_job(job.id));
    CHECK(f.timing(scheduled.id).runnable_wait_us == 1000000);
}

TEST_CASE("Queue updates preserve wait under full capacity and wall clock correction", "[jobu][telemetry][management]")
{
    Fixture f;
    auto    queue  = f.queue();
    auto    first  = f.scheduled(f.job(queue).id);
    auto    second = f.scheduled(f.job(queue).id);
    f.time.set_monotonic(TimePoint{1007s});
    f.time.set_utc(UtcTimePoint{1s});
    REQUIRE(f.service->update_queue({.queue = queue.id, .concurrency_limit = 1, .runnable_wait_warning = 0ms}));
    CHECK(f.timing(first.id).runnable_wait_us == 7000000);
    CHECK(f.timing(second.id).runnable_wait_us == 7000000);
    CHECK_FALSE(f.timing(first.id).open_epoch); // Due eligibility follows observed UTC, not inferred clock crossings.
    f.time.set_utc(UtcTimePoint{300s});
    REQUIRE(f.service->update_queue({.queue = queue.id, .weight = 2}));
    CHECK(f.timing(first.id).open_tick_us == 7000000);
    CHECK(f.timing(second.id).open_tick_us == 7000000);
}

TEST_CASE("Pending schedule and type replacement retain accumulated operational wait", "[jobu][telemetry][management]")
{
    Fixture f;
    auto    queue = f.queue();
    auto    job   = f.job(queue);
    auto    run   = f.scheduled(job.id);
    f.time.advance(4s);
    auto updated = f.service->update_job({
        .job_id            = job.id,
        .expected_revision = job.revision,
        .schedule          = OnceSchedule{.planned_at = UtcTimePoint{200s}},
    });
    REQUIRE(updated);
    CHECK(f.timing(run.id).runnable_wait_us == 4000000);
    CHECK_FALSE(f.timing(run.id).open_epoch);
    f.time.advance(10s);
    auto model = f.storage.make_job(recovery_id(9000), queue.id, JobType::Http);
    updated    = f.service->update_job({
        .job_id            = job.id,
        .expected_revision = updated->revision,
        .type              = JobType::Http,
        .schedule          = OnceSchedule{.planned_at = UtcTimePoint{100s}},
        .payload           = model.payload,
    });
    REQUIRE(updated);
    CHECK(f.timing(run.id).runnable_wait_us == 4000000);
    CHECK_FALSE(f.timing(run.id).open_epoch);
    model = f.storage.make_job(recovery_id(9000), queue.id);
    REQUIRE(f.service->update_job(
        {.job_id = job.id, .expected_revision = updated->revision, .type = JobType::Cli, .payload = model.payload}));
    CHECK(f.timing(run.id).open_tick_us == 14000000);
    CHECK(f.timing(run.id).quality == WaitQuality::Complete);
}

TEST_CASE("Moving a suspended job preserves manual wait and obeys the destination queue",
          "[jobu][telemetry][management]")
{
    auto    target_active = GENERATE(false, true);
    Fixture f;
    auto    old_queue = f.queue("old");
    auto    target    = f.queue("new");
    if (!target_active) {
        REQUIRE(f.service->suspend_queue(target.id));
    }
    auto job       = f.job(old_queue, OnceSchedule{.planned_at = UtcTimePoint{200s}});
    auto suspended = f.service->suspend_job(job.id);
    REQUIRE(suspended);
    auto manual = f.service->run_now({.job_id = job.id});
    REQUIRE(manual);
    f.time.advance(6s);
    auto moved =
        f.service->move_job({.job_id = job.id, .expected_revision = suspended->revision, .target_queue = target.id});
    REQUIRE(moved);
    CHECK(f.timing(manual->id).runnable_wait_us == 6000000);
    CHECK(f.timing(manual->id).open_epoch.has_value() == target_active);
    if (target_active) {
        CHECK(f.timing(manual->id).open_tick_us == 6000000);
    }
    CHECK(f.scheduled(job.id).queue_id == target.id);
}

TEST_CASE("Deleted jobs and queues cancel pending manual work with no open tail", "[jobu][telemetry][management]")
{
    auto    delete_queue = GENERATE(false, true);
    Fixture f;
    auto    queue     = f.queue();
    auto    job       = f.job(queue, OnceSchedule{.planned_at = UtcTimePoint{200s}});
    auto    suspended = f.service->suspend_job(job.id);
    REQUIRE(suspended);
    auto manual = f.service->run_now({.job_id = job.id});
    REQUIRE(manual);
    f.time.advance(5s);
    if (delete_queue) {
        REQUIRE(f.service->suspend_queue(queue.id));
        REQUIRE(f.service->delete_queue(queue.id));
    }
    else {
        REQUIRE(f.service->delete_job({.job_id = job.id, .expected_revision = suspended->revision}));
    }
    CHECK(f.timing(manual->id).state == RunState::Cancelled);
    CHECK(f.timing(manual->id).runnable_wait_us == 5000000);
    CHECK_FALSE(f.timing(manual->id).open_epoch);
}

TEST_CASE("Replays no-ops and rejected mutations leave timing unchanged", "[jobu][telemetry][management]")
{
    Fixture f;
    auto    queue           = f.queue();
    auto    request         = f.request(queue, OnceSchedule{.planned_at = UtcTimePoint{200s}});
    request.idempotency_key = "create";
    auto job                = f.service->create_job(request);
    REQUIRE(job);
    auto manual = f.service->run_now({.job_id = job->id, .idempotency_key = "manual"});
    REQUIRE(manual);
    auto before        = storage_snapshot(f.storage.database);
    auto notifications = f.notifications;
    f.time.advance(20s);
    REQUIRE(f.service->create_job(request));
    REQUIRE(f.service->run_now({.job_id = job->id, .idempotency_key = "manual"}));
    // Creation retains its established notification behavior; Run Now replay remains silent.
    CHECK(f.notifications == notifications + 1);
    REQUIRE(f.service->resume_job(job->id));
    REQUIRE(f.service->resume_queue(queue.id));
    REQUIRE_FALSE(f.service->update_job({.job_id = job->id, .expected_revision = 99, .name = "other"}));
    CHECK(storage_snapshot(f.storage.database) == before);
}

TEST_CASE("Inactive and omitted telemetry keep embedded creation unmeasured", "[jobu][telemetry][management]")
{
    auto    supply = GENERATE(false, true);
    Fixture f{false, true, false, supply};
    auto    queue = f.queue();
    auto    run   = f.scheduled(f.job(queue).id);
    REQUIRE(f.service->suspend_queue(queue.id));
    REQUIRE(f.service->resume_queue(queue.id));
    CHECK(f.timing(run.id).quality == WaitQuality::Unmeasured);
    CHECK_FALSE(f.timing(run.id).open_epoch);
}

TEST_CASE("An embedded mutation rejects abandoned open timing before domain changes", "[jobu][telemetry][management]")
{
    Fixture           f;
    auto              queue = f.queue();
    auto              run   = f.scheduled(f.job(queue).id);
    ManagementService embedded{f.storage.database, f.storage.registry, f.cron, f.generator, f.time};
    auto              before   = storage_snapshot(f.storage.database);
    auto              rejected = embedded.suspend_queue(queue.id);
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().code == "jobu.telemetry.invalid_state");
    CHECK(storage_snapshot(f.storage.database) == before);
    CHECK(f.timing(run.id).open_epoch);
}

TEST_CASE("Late instrumentation remains partial and scoped pages reach the tail", "[jobu][telemetry][management]")
{
    Fixture f;
    auto    queue = f.queue();
    for (std::uint32_t i = 0; i < 205; ++i) {
        auto job = f.storage.make_job(recovery_id(1000 + i), queue.id);
        f.storage.insert_job(job);
        f.storage.insert_run(f.storage.make_run(recovery_id(2000 + i), job));
    }
    REQUIRE(f.service->update_queue({.queue = queue.id, .weight = 2}));
    CHECK(f.timing(recovery_id(2000)).quality == WaitQuality::Partial);
    CHECK(f.timing(recovery_id(2204)).quality == WaitQuality::Partial);
    CHECK(f.timing(recovery_id(2204)).open_epoch);
    f.time.advance(3s);
    REQUIRE(f.service->suspend_queue(queue.id));
    CHECK(f.timing(recovery_id(2204)).runnable_wait_us == 3000000);
    CHECK_FALSE(f.timing(recovery_id(2204)).open_epoch);
}

TEST_CASE("Eligibility observation does not decode run payload or attribute snapshots", "[jobu][telemetry][management]")
{
    Fixture f;
    auto    queue = f.queue();
    auto    run   = f.scheduled(f.job(queue).id);
    f.sql("UPDATE jobu_runs SET payload_json = 'not-json', attributes_json = 'not-json'");
    f.time.advance(1s);
    REQUIRE(f.service->suspend_queue(queue.id));
    CHECK(f.timing(run.id).runnable_wait_us == 1000000);
}

TEST_CASE("Timing and domain write faults roll back the original open interval", "[jobu][telemetry][management][fault]")
{
    auto const* boundary = GENERATE("timing.write", "queue.write");
    auto        phase    = GENERATE(DatabaseFaultPhase::Before, DatabaseFaultPhase::AfterSuccess);
    Fixture     f;
    auto        queue  = f.queue();
    auto        run    = f.scheduled(f.job(queue).id);
    auto        before = storage_snapshot(f.storage.database);
    f.time.advance(5s);
    f.faults->calls.clear();
    f.faults->faults.push_back({
        .at    = {.boundary = boundary, .operation = DatabaseOperation::Execute, .phase = phase},
        .error = fault_error(),
    });
    auto failed = f.service->suspend_queue(queue.id);
    REQUIRE_FALSE(failed);
    check_safe_error(failed.error(), "db.io");
    require_consumed_faults(*f.faults);
    CHECK(storage_snapshot(f.storage.database) == before);
    CHECK(f.timing(run.id).open_tick_us == 0);
    CHECK(f.management_failures.size() == 1);
    if (std::string_view{boundary} == "timing.write") {
        REQUIRE(f.telemetry_failures.size() == 1);
        CHECK(f.calls_at_failure.back().operation == DatabaseOperation::Rollback);
    }
}

TEST_CASE("Accounting failure delivery permits direct owner destruction after cleanup",
          "[jobu][telemetry][management][lifetime]")
{
    Fixture f;
    auto    queue = f.queue();
    f.job(queue);
    f.telemetry->failed.connect(&f.receiver, [&f](Error const&) { f.telemetry.reset(); });
    f.time.set_monotonic(TimePoint{999s});
    auto failed = f.service->suspend_queue(queue.id);
    REQUIRE_FALSE(failed);
    CHECK(failed.error().code == "jobu.telemetry.clock_regression");
    CHECK_FALSE(f.telemetry);
    CHECK(f.service->resume_queue(queue.id).error().code == "jobu.service.stopping");
}

TEST_CASE("Pending recurring cancellation creates a complete future successor", "[jobu][telemetry][management]")
{
    Fixture            f;
    auto               queue = f.queue();
    CronSchedule const schedule{.expression = "* * * * *", .timezone = "UTC"};
    f.cron.set_occurrences(schedule, {UtcTimePoint{110s}, UtcTimePoint{200s}});
    auto job = f.job(queue, CronScheduleInput{.expression = schedule.expression, .timezone = schedule.timezone});
    auto run = f.scheduled(job.id);
    f.time.advance(15s);
    REQUIRE(f.service->update_queue({.queue = queue.id, .weight = 2}));
    f.time.advance(3s);
    REQUIRE(f.cancel(run.id));
    CHECK(f.timing(run.id).runnable_wait_us == 3000000);
    auto successor = f.scheduled(job.id);
    CHECK(successor.id != run.id);
    CHECK(f.timing(successor.id).quality == WaitQuality::Complete);
    CHECK_FALSE(f.timing(successor.id).open_epoch);
}

TEST_CASE("A mutation never resamples between settling and reopening", "[jobu][telemetry][management]")
{
    Fixture f;
    auto    queue = f.queue();
    auto    run   = f.scheduled(f.job(queue).id);
    f.time.advance(4s);
    f.faults->classify = [&f](std::string_view sql) {
        if (sql.starts_with("UPDATE jobu_queues")) {
            // Model a clock advancing while the domain write is prepared. Both timing phases
            // must still use the logical transaction boundary captured before that write.
            f.time.advance(7s);
        }
        return std::string{fault_boundary(sql)};
    };
    REQUIRE(f.service->update_queue({.queue = queue.id, .weight = 2}));
    CHECK(f.timing(run.id).runnable_wait_us == 4000000);
    CHECK(f.timing(run.id).open_tick_us == 4000000);
}

TEST_CASE("Due blocking retries count wait while backoff and running attempts are excluded",
          "[jobu][telemetry][management]")
{
    Fixture f;
    auto    queue       = f.queue();
    // A running row occupies the queue's only slot. The due retry owns another retained
    // blocking slot; capacity still cannot remove its observed runner wait.
    auto    running_job = f.storage.make_job(recovery_id(1), queue.id);
    f.storage.insert_job(running_job);
    f.storage.insert_run(f.storage.make_run(recovery_id(2), running_job, RunState::Running));
    auto retry_job = f.storage.make_job(recovery_id(3), queue.id);
    f.storage.insert_job(retry_job);
    f.storage.insert_run(f.storage.make_run(recovery_id(4), retry_job, RunState::RetryWait, 1));
    auto future_job = f.storage.make_job(recovery_id(5), queue.id);
    f.storage.insert_job(future_job);
    auto future            = f.storage.make_run(recovery_id(6), future_job, RunState::RetryWait, 1);
    future.run.runnable_at = UtcTimePoint{200s};
    f.storage.insert_run(future);
    REQUIRE(f.service->update_queue({.queue = queue.id, .weight = 2}));
    CHECK(f.timing(recovery_id(4)).quality == WaitQuality::Partial);
    CHECK(f.timing(recovery_id(4)).open_epoch);
    CHECK(f.timing(recovery_id(6)).quality == WaitQuality::Unmeasured);
    CHECK_FALSE(f.timing(recovery_id(6)).open_epoch);
    CHECK_FALSE(f.timing(recovery_id(2)).open_epoch);
}

TEST_CASE("Missing timing foreign epochs and malformed eligibility fail before mutation",
          "[jobu][telemetry][management][fault]")
{
    auto    corruption = GENERATE(0, 1, 2);
    Fixture f;
    auto    queue = f.queue();
    f.job(queue);
    if (corruption == 0) {
        f.sql("DELETE FROM jobu_run_timing");
    }
    else if (corruption == 1) {
        jb::db::Query query{f.storage.database};
        REQUIRE(query.prepare("UPDATE jobu_run_timing SET open_epoch = :epoch"));
        REQUIRE(query.bind_value(":epoch", uuid_to_storage(recovery_id(9999))));
        REQUIRE(query.exec());
    }
    else {
        // Deliberately bypass the schema check to exercise persisted-data validation,
        // independently of the ordinary API's schedule validation.
        f.sql("PRAGMA ignore_check_constraints = ON");
        f.sql("UPDATE jobu_jobs SET schedule_kind = 'invalid'");
    }
    auto before = storage_snapshot(f.storage.database);
    auto result = f.service->suspend_queue(queue.id);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == "jobu.telemetry.invalid_state");
    CHECK(result.error().detail.find("durable_invariant") != std::string::npos);
    CHECK(storage_snapshot(f.storage.database) == before);
    CHECK(f.telemetry_failures.size() == 1);
}

TEST_CASE("Registration validates actual capabilities and rejects mixed or late ownership",
          "[jobu][telemetry][management]")
{
    Fixture f{false, false, true};
    REQUIRE(f.telemetry->start());
    auto types = TelemetryAccess::available_types(*f.telemetry);
    REQUIRE(types);
    CHECK_FALSE(types->cli);
    CHECK(types->http);
    auto queue = f.queue();
    auto run   = f.scheduled(f.job(queue).id);
    CHECK_FALSE(f.timing(run.id).open_epoch); // Positive CLI limits do not invent a runner.
    auto late =
        TelemetryAccess::register_executor(*f.telemetry, f.storage.database, f.storage.registry, f.time, f.executor);
    REQUIRE_FALSE(late);
    CHECK(late.error().error.code == "jobu.telemetry.invalid_state");
    FakeTimeSource other_clock;
    auto           mixed =
        TelemetryAccess::mutation_boundary(f.telemetry.get(), f.storage.database, f.storage.registry, other_clock);
    REQUIRE_FALSE(mixed);
    CHECK(mixed.error().error.code == "jobu.telemetry.invalid_state");
}

TEST_CASE("Commit uncertainty and rollback poisoning gate mutations after timing cleanup",
          "[jobu][telemetry][management][fault]")
{
    auto    poisoned_rollback = GENERATE(false, true);
    Fixture f;
    auto    queue = f.queue();
    f.job(queue);
    f.time.advance(2s);
    f.faults->calls.clear();
    if (poisoned_rollback) {
        f.faults->faults.push_back({
            .at    = {.boundary = "timing.write", .operation = DatabaseOperation::Execute},
            .error = fault_error(),
        });
        f.faults->faults.push_back({
            .at    = {.boundary = "connection", .operation = DatabaseOperation::Rollback},
            .error = fault_error("db.rollback_failed"),
        });
    }
    else {
        f.faults->faults.push_back({
            .at =
                {
                     .boundary  = "connection",
                     .operation = DatabaseOperation::Commit,
                     .phase     = DatabaseFaultPhase::AfterSuccess,
                     },
            .error = fault_error("db.commit_failed"),
        });
    }
    auto result = f.service->suspend_queue(queue.id);
    REQUIRE_FALSE(result);
    require_consumed_faults(*f.faults);
    CHECK(f.storage.database.is_poisoned());
    auto count = f.faults->calls.size();
    CHECK(f.service->resume_queue(queue.id).error().code == "jobu.service.stopping");
    CHECK(f.faults->calls.size() == count);
    if (poisoned_rollback) {
        REQUIRE(f.telemetry_failures.size() == 1);
        check_safe_error(f.telemetry_failures.front(), "db.io"); // Original fatal write failure wins.
    }
}

TEST_CASE("Suspension preserves an already claimed delay warning", "[jobu][telemetry][management]")
{
    Fixture f;
    auto    queue = f.queue();
    auto    run   = f.scheduled(f.job(queue).id);
    f.sql("UPDATE jobu_run_timing SET delay_warned = 1");
    f.time.advance(1s);
    REQUIRE(f.service->suspend_queue(queue.id));
    REQUIRE(f.service->resume_queue(queue.id));
    CHECK(f.timing(run.id).delay_warned);
}

TEST_CASE("Integrated counter overflow rolls back instead of wrapping or clamping",
          "[jobu][telemetry][management][fault]")
{
    Fixture f;
    auto    queue = f.queue();
    f.job(queue);
    f.sql("UPDATE jobu_run_timing SET runnable_wait_us = 9223372036854775807");
    auto before = storage_snapshot(f.storage.database);
    f.time.advance(1us);
    auto result = f.service->suspend_queue(queue.id);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == "jobu.telemetry.counter_overflow");
    CHECK(storage_snapshot(f.storage.database) == before);
    CHECK(f.telemetry_failures.size() == 1);
}

TEST_CASE("Pending cancellation failure permits direct telemetry destruction after rollback",
          "[jobu][telemetry][management][lifetime]")
{
    Fixture f;
    auto    queue  = f.queue();
    auto    run    = f.scheduled(f.job(queue).id);
    auto    before = storage_snapshot(f.storage.database);
    f.telemetry->failed.connect(&f.receiver, [&f](Error const&) { f.telemetry.reset(); });
    f.faults->calls.clear();
    f.faults->faults.push_back({
        .at    = {.boundary = "timing.write", .operation = DatabaseOperation::Execute},
        .error = fault_error(),
    });
    f.time.advance(2s);
    auto result = f.cancel(run.id);
    REQUIRE_FALSE(result);
    CHECK_FALSE(f.telemetry);
    CHECK(f.calls_at_failure.back().operation == DatabaseOperation::Rollback);
    CHECK(storage_snapshot(f.storage.database) == before);
}

TEST_CASE("A management failure receiver can destroy the telemetry target before its notification",
          "[jobu][telemetry][management][lifetime]")
{
    Fixture f;
    auto    queue = f.queue();
    f.job(queue);
    f.service->failed.connect(&f.receiver, [&f](Error const&) { f.telemetry.reset(); });
    f.time.set_monotonic(TimePoint{999s});
    auto result = f.service->suspend_queue(queue.id);
    REQUIRE_FALSE(result);
    CHECK_FALSE(f.telemetry);
    CHECK(f.telemetry_failures.empty());
    CHECK(f.service->resume_queue(queue.id).error().code == "jobu.service.stopping");
}
