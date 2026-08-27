#include "scheduler.hpp"

#include "attribute_registry.hpp"
#include "database.hpp"
#include "event_loop.hpp"
#include "json.hpp"
#include "management.hpp"
#include "run_repository_priv.hpp"
#include "sqlite/sqlite_driver.hpp"
#include "sqlite/sqlite_schema.hpp"
#include "support/fake_attempt_executor.hpp"
#include "support/fake_cron_engine.hpp"
#include "support/fake_event_loop_backend.hpp"
#include "support/fake_time_source.hpp"
#include "support/sequence_uuid_generator.hpp"
#include "support/temporary_directory.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
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

auto test_ids() -> std::vector<Uuid>
{
    auto values = std::vector<Uuid>{};
    values.reserve(200);
    for (auto suffix = std::uint16_t{1}; suffix <= 200; ++suffix) {
        values.push_back(id(static_cast<std::uint8_t>(suffix)));
    }
    return values;
}

auto at_seconds(std::int64_t seconds) -> UtcTimePoint
{
    return UtcTimePoint{std::chrono::seconds{seconds}};
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

auto cli_payload(std::string command) -> JsonValue
{
    auto result = JsonValue{};
    result.data = JsonValue::Object{
        {"command", json_string(std::move(command))}
    };
    return result;
}

auto completion_result(std::string status) -> JsonValue
{
    auto result = JsonValue{};
    result.data = JsonValue::Object{
        {"status", json_string(std::move(status))}
    };
    return result;
}

auto succeeded(AttemptKey key) -> AttemptCompletion
{
    return {
        .key     = key,
        .outcome = AttemptOutcome::Succeeded,
        .result  = completion_result("succeeded"),
    };
}

auto retryable_failure(AttemptKey key) -> AttemptCompletion
{
    return {
        .key                 = key,
        .outcome             = AttemptOutcome::Failed,
        .failure_disposition = FailureDisposition::Retryable,
        .result              = completion_result("retryable"),
    };
}

struct CreatedJob {
    JobDefinition definition;
    JobRun        run;
};

struct SchedulerFixture {
    explicit SchedulerFixture(SchedulerOptions options = {})
        : event_loop{jb::core::priv::make_fake_event_loop()}
        , current_loop{event_loop.loop.get()}
        , database_file{directory.path() / "jobu.sqlite"}
        , database{make_database(database_file)}
        , generator{test_ids()}
        , runs{database, registry}
    {
        REQUIRE(database.open());
        REQUIRE(jb::jobu::sqlite::ensure_schema(database));
        time.set_utc(at_seconds(100));
        management = std::make_unique<ManagementService>(database, registry, cron, generator, time);
        scheduler  = std::make_unique<Scheduler>(database, registry, cron, generator, time, executor, options);
    }

    ~SchedulerFixture()
    {
        if (!scheduler) {
            return;
        }
        scheduler->stop();
        while (!executor.pending_keys().empty()) {
            auto const key = executor.pending_keys().front();
            (void)executor.complete(key, succeeded(key));
        }
        scheduler.reset();
    }

    auto create_queue(std::string name, std::uint32_t weight, std::uint32_t concurrency) const -> Queue
    {
        auto created = management->create_queue({
            .name              = std::move(name),
            .weight            = weight,
            .concurrency_limit = concurrency,
        });
        REQUIRE(created);
        return std::move(created).value();
    }

    auto create_job(Queue const& queue,
                    JobSchedule  schedule,
                    std::string  command,
                    std::int32_t priority   = 0,
                    AttributeSet attributes = {}) -> CreatedJob
    {
        auto definition = management->create_job({
            .queue      = queue.id,
            .schedule   = std::move(schedule),
            .priority   = priority,
            .attributes = std::move(attributes),
            .payload    = cli_payload(std::move(command)),
        });
        REQUIRE(definition);
        auto run = runs.find_schedule_owned(definition->id);
        REQUIRE(run);
        REQUIRE(run->has_value());
        return {
            .definition = std::move(definition).value(),
            .run        = std::move(run->value()),
        };
    }

    auto find_run(Uuid const& run_id) -> JobRun
    {
        auto found = runs.find_by_id(run_id);
        REQUIRE(found);
        REQUIRE(found->has_value());
        return std::move(found->value());
    }

    auto find_schedule_owned(Uuid const& job_id) -> JobRun
    {
        auto found = runs.find_schedule_owned(job_id);
        REQUIRE(found);
        REQUIRE(found->has_value());
        return std::move(found->value());
    }

    void process_timers() const
    {
        REQUIRE(event_loop.loop->process_events(EventFlag::Timers) == ProcessEventsResult::Stopped);
    }

    jb::core::priv::FakeEventLoop          event_loop;
    jb::core::priv::ScopedCurrentEventLoop current_loop;
    TemporaryDirectory                     directory;
    std::filesystem::path                  database_file;
    Database                               database;
    StandardAttributeRegistry              registry;
    FakeCronEngine                         cron;
    SequenceUuidGenerator                  generator;
    FakeTimeSource                         time;
    FakeAttemptExecutor                    executor;
    RunRepository                          runs;
    std::unique_ptr<ManagementService>     management;
    std::unique_ptr<Scheduler>             scheduler;
};

} // anonymous namespace

TEST_CASE("Scheduler integrates deterministic Phase 4 behavior", "[jobu][scheduler][integration][sqlite]")
{
    SchedulerFixture fixture{
        {
         .cli_concurrency      = 1,
         .http_concurrency     = 1,
         .candidate_batch_size = 2,
         }
    };
    fixture.executor.set_available(JobType::Cli, true);

    auto const light_queue = fixture.create_queue("light", 1, 1);
    auto const heavy_queue = fixture.create_queue("heavy", 2, 1);
    for (auto index = std::size_t{0}; index < 3; ++index) {
        fixture.create_job(light_queue, OnceSchedule{.planned_at = at_seconds(90)}, "light-" + std::to_string(index));
    }
    for (auto index = std::size_t{0}; index < 6; ++index) {
        fixture.create_job(heavy_queue, OnceSchedule{.planned_at = at_seconds(90)}, "heavy-" + std::to_string(index));
    }

    // Drain sustained one-time work through one global slot so the public adapter preserves the core's 1:2 credits.
    REQUIRE(fixture.scheduler->start());
    constexpr auto fairness_starts = std::size_t{9};
    for (auto index = std::size_t{0}; index < fairness_starts; ++index) {
        REQUIRE(fixture.executor.pending_keys().size() == 1U);
        auto const key = fixture.executor.pending_keys().front();
        REQUIRE(fixture.executor.complete(key, succeeded(key)));
        fixture.process_timers();
    }
    REQUIRE(fixture.executor.start_requests().size() == fairness_starts);
    auto light_starts = std::size_t{0};
    auto heavy_starts = std::size_t{0};
    for (auto const& request : fixture.executor.start_requests()) {
        light_starts += request.queue_id == light_queue.id ? 1U : 0U;
        heavy_starts += request.queue_id == heavy_queue.id ? 1U : 0U;
    }
    CHECK(light_starts == 3U);
    CHECK(heavy_starts == 6U);
    CHECK(fixture.executor.pending_keys().empty());

    auto const schedule = CronSchedule{.expression = "*/5 * * * *", .timezone = "UTC"};
    fixture.cron.set_occurrences(schedule, {at_seconds(200), at_seconds(300), at_seconds(400)});
    auto recurring =
        fixture.create_job(light_queue,
                           schedule,
                           "recurring",
                           0,
                           {
                               {"retry.initial_delay", {.data = std::chrono::duration_cast<Duration>(100s)}},
                               {"retry.max_attempts",  {.data = std::int64_t{2}}                           },
    });
    auto manual = fixture.management->run_now({.job_id = recurring.definition.id});
    REQUIRE(manual);
    fixture.scheduler->request_rescan();
    fixture.process_timers();
    REQUIRE(fixture.executor.pending_keys().size() == 1U);
    auto const manual_first = fixture.executor.pending_keys().front();
    CHECK(manual_first.run_id == manual->id);

    fixture.time.set_utc(at_seconds(150));
    REQUIRE(fixture.executor.complete(manual_first, retryable_failure(manual_first)));
    fixture.process_timers();
    auto waiting = fixture.find_run(manual->id);
    CHECK(waiting.state == RunState::RetryWait);
    CHECK(waiting.runnable_at == at_seconds(250));

    // At the scheduled occurrence, the active manual retry is the only reason the recurring run remains blocked.
    fixture.time.set_utc(at_seconds(200));
    fixture.scheduler->request_rescan();
    fixture.process_timers();
    CHECK(fixture.executor.pending_keys().empty());
    CHECK(fixture.find_run(recurring.run.id).state == RunState::Scheduled);

    auto suspended = fixture.management->suspend_job(recurring.definition.id);
    REQUIRE(suspended);
    CHECK(suspended->state == JobState::Suspended);
    fixture.scheduler->request_rescan();
    fixture.process_timers();

    // Manual retries bypass job suspension, while the schedule-owned occurrence continues to respect it.
    fixture.time.set_utc(at_seconds(250));
    fixture.scheduler->request_rescan();
    fixture.process_timers();
    REQUIRE(fixture.executor.pending_keys().size() == 1U);
    auto const manual_retry = fixture.executor.pending_keys().front();
    CHECK(manual_retry.run_id == manual->id);
    CHECK(manual_retry.attempt_number == 2U);
    fixture.time.set_utc(at_seconds(260));
    REQUIRE(fixture.executor.complete(manual_retry, succeeded(manual_retry)));
    fixture.process_timers();
    CHECK(fixture.find_run(manual->id).state == RunState::Succeeded);
    CHECK(fixture.executor.pending_keys().empty());

    auto resumed = fixture.management->resume_job(recurring.definition.id);
    REQUIRE(resumed);
    CHECK(resumed->state == JobState::Active);
    fixture.scheduler->request_rescan();
    fixture.process_timers();
    REQUIRE(fixture.executor.pending_keys().size() == 1U);
    auto const scheduled_key = fixture.executor.pending_keys().front();
    CHECK(scheduled_key.run_id == recurring.run.id);
    fixture.time.set_utc(at_seconds(270));
    REQUIRE(fixture.executor.complete(scheduled_key, succeeded(scheduled_key)));
    fixture.process_timers();

    auto const successor = fixture.find_schedule_owned(recurring.definition.id);
    CHECK(successor.id != recurring.run.id);
    CHECK(successor.state == RunState::Scheduled);
    CHECK(successor.planned_at == at_seconds(300));
    REQUIRE(fixture.cron.next_calls().size() == 2U);
    CHECK(fixture.cron.next_calls().back().exclusive_lower_bound == at_seconds(270));

    auto const cancel_target =
        fixture.create_job(heavy_queue, OnceSchedule{.planned_at = at_seconds(260)}, "cancel-target", 10);
    auto const cancel_follower =
        fixture.create_job(heavy_queue, OnceSchedule{.planned_at = at_seconds(260)}, "cancel-follower");
    fixture.scheduler->request_rescan();
    fixture.process_timers();
    REQUIRE(fixture.executor.pending_keys().size() == 1U);
    auto const cancelled_key = fixture.executor.pending_keys().front();
    REQUIRE(cancelled_key.run_id == cancel_target.run.id);

    auto requested = fixture.scheduler->cancel_run(cancel_target.run.id);
    REQUIRE(requested);
    CHECK(requested->disposition == CancelDisposition::Requested);
    auto repeated = fixture.scheduler->cancel_run(cancel_target.run.id);
    REQUIRE(repeated);
    CHECK(repeated->disposition == CancelDisposition::Requested);
    REQUIRE(fixture.executor.cancel_calls().size() == 1U);
    CHECK(fixture.executor.cancel_calls().front() == cancelled_key);

    // An accepted cancellation keeps its slot until the executor's exactly-once completion commits.
    auto const starts_before_cancel_completion = fixture.executor.start_requests().size();
    fixture.scheduler->request_rescan();
    fixture.process_timers();
    CHECK(fixture.executor.start_requests().size() == starts_before_cancel_completion);
    fixture.time.set_utc(at_seconds(280));
    REQUIRE(fixture.executor.complete(cancelled_key, succeeded(cancelled_key)));
    CHECK(fixture.find_run(cancel_target.run.id).state == RunState::Cancelled);
    CHECK(fixture.executor.start_requests().size() == starts_before_cancel_completion);
    fixture.process_timers();
    REQUIRE(fixture.executor.pending_keys().size() == 1U);
    auto const follower_key = fixture.executor.pending_keys().front();
    CHECK(follower_key.run_id == cancel_follower.run.id);
    fixture.time.set_utc(at_seconds(281));
    REQUIRE(fixture.executor.complete(follower_key, succeeded(follower_key)));
    fixture.process_timers();

    CHECK(fixture.find_run(cancel_follower.run.id).state == RunState::Succeeded);
    CHECK(fixture.executor.pending_keys().empty());
    CHECK(fixture.scheduler->state() == SchedulerState::Running);
    CHECK_FALSE(fixture.scheduler->failure());
    fixture.scheduler->stop();
}
