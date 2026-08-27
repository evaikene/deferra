#include "scheduler.hpp"

#include "attribute_registry.hpp"
#include "database.hpp"
#include "event_loop.hpp"
#include "json.hpp"
#include "management.hpp"
#include "query.hpp"
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
#include <cstdint>
#include <filesystem>
#include <memory>
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

auto payload(JobType type) -> JsonValue
{
    auto result = JsonValue{};
    result.data = type == JobType::Cli ? JsonValue::Object{{"command", json_string("test")}}
                                       : JsonValue::Object{{"url", json_string("https://example.test")}};
    return result;
}

auto success(AttemptKey key) -> AttemptCompletion
{
    auto result = JsonValue{};
    result.data = JsonValue::Object{
        {"status", json_string("completed")}
    };
    return {
        .key     = key,
        .outcome = AttemptOutcome::Succeeded,
        .result  = std::move(result),
    };
}

void execute(Database& database, std::string_view sql)
{
    Query query{database};
    REQUIRE(query.exec(sql));
}

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
        for (auto const& key : executor.pending_keys()) {
            (void)executor.complete(key, success(key));
        }
        scheduler.reset();
    }

    auto create_queue(std::string name = "queue", std::uint32_t concurrency = 1) const -> Queue
    {
        auto created = management->create_queue({.name = std::move(name), .concurrency_limit = concurrency});
        REQUIRE(created);
        return std::move(created).value();
    }

    auto create_job(Queue const& queue, JobType type, UtcTimePoint planned_at) -> JobRun
    {
        auto definition = management->create_job({
            .queue    = queue.id,
            .type     = type,
            .schedule = OnceSchedule{.planned_at = planned_at},
            .payload  = payload(type),
        });
        REQUIRE(definition);
        auto run = runs.find_schedule_owned(definition->id);
        REQUIRE(run);
        REQUIRE(run->has_value());
        return std::move(**run);
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

TEST_CASE("Scheduler start performs one immediate cycle and is idempotent",
          "[jobu][scheduler][event-loop][lifecycle][sqlite]")
{
    SchedulerFixture fixture;
    auto const       queue = fixture.create_queue();
    fixture.create_job(queue, JobType::Cli, at_seconds(90));
    fixture.executor.set_available(JobType::Cli, true);

    CHECK(fixture.scheduler->state() == SchedulerState::Stopped);
    CHECK_FALSE(fixture.scheduler->failure());
    REQUIRE(fixture.scheduler->start());
    CHECK(fixture.scheduler->state() == SchedulerState::Running);
    REQUIRE(fixture.executor.start_requests().size() == 1U);

    REQUIRE(fixture.scheduler->start());
    CHECK(fixture.executor.start_requests().size() == 1U);
    auto const first_key = fixture.executor.pending_keys().front();
    REQUIRE(fixture.executor.complete(first_key, success(first_key)));
    fixture.scheduler->stop();
    fixture.scheduler->stop();
    CHECK(fixture.scheduler->state() == SchedulerState::Stopped);

    fixture.create_job(queue, JobType::Cli, at_seconds(90));
    REQUIRE(fixture.scheduler->start());
    CHECK(fixture.scheduler->state() == SchedulerState::Running);
    CHECK(fixture.executor.start_requests().size() == 2U);
}

TEST_CASE("Scheduler start rejects invalid construction and startup recovery state",
          "[jobu][scheduler][event-loop][start][sqlite]")
{
    SECTION("invalid options")
    {
        for (auto const options : std::vector<SchedulerOptions>{
                 {.cli_concurrency = 0},
                 {.http_concurrency = 0},
                 {.candidate_batch_size = 0},
                 {.wall_clock_recheck = 0s},
             }) {
            SchedulerFixture fixture{options};
            auto             started = fixture.scheduler->start();
            REQUIRE_FALSE(started);
            CHECK(started.error().code == "jobu.scheduler.invalid_options");
            CHECK(fixture.scheduler->state() == SchedulerState::Stopped);
            CHECK_FALSE(fixture.scheduler->failure());
        }
    }

    SECTION("persisted running state")
    {
        SchedulerFixture fixture;
        auto const       queue = fixture.create_queue();
        fixture.create_job(queue, JobType::Cli, at_seconds(200));
        execute(fixture.database,
                "UPDATE jobu_runs SET state = 'running', started_at_us = runnable_at_us WHERE state = 'scheduled'");

        auto started = fixture.scheduler->start();
        REQUIRE_FALSE(started);
        CHECK(started.error().code == "jobu.scheduler.recovery_required");
        CHECK(fixture.scheduler->state() == SchedulerState::Stopped);
    }

    SECTION("missing owner event loop")
    {
        jb::core::priv::ScopedCurrentEventLoop no_loop{nullptr};
        TemporaryDirectory                     directory;
        auto                                   database = make_database(directory.path() / "jobu.sqlite");
        REQUIRE(database.open());
        REQUIRE(jb::jobu::sqlite::ensure_schema(database));
        StandardAttributeRegistry registry;
        FakeCronEngine            cron;
        SequenceUuidGenerator     generator{test_ids()};
        FakeTimeSource            time;
        FakeAttemptExecutor       executor;
        Scheduler                 scheduler{database, registry, cron, generator, time, executor};

        auto started = scheduler.start();
        REQUIRE_FALSE(started);
        CHECK(started.error().code == "jobu.scheduler.event_loop_unavailable");
        CHECK(scheduler.state() == SchedulerState::Stopped);
    }
}

TEST_CASE("Scheduler arms only the earliest available-type wake and caps wall-clock waits",
          "[jobu][scheduler][event-loop][wake][sqlite]")
{
    SECTION("earliest available type")
    {
        SchedulerFixture fixture;
        auto const       queue = fixture.create_queue("mixed", 2);
        fixture.create_job(queue, JobType::Http, at_seconds(110));
        fixture.create_job(queue, JobType::Cli, at_seconds(120));
        fixture.executor.set_available(JobType::Cli, true);

        REQUIRE(fixture.scheduler->start());
        CHECK(fixture.event_loop.loop->process_events(EventFlag::Watchers) == ProcessEventsResult::Stopped);
        CHECK(fixture.event_loop.backend->last_timeout_ms >= 19000);
        CHECK(fixture.event_loop.backend->last_timeout_ms <= 20000);
    }

    SECTION("wall-clock recheck cap")
    {
        SchedulerFixture fixture;
        auto const       queue = fixture.create_queue();
        fixture.create_job(queue, JobType::Cli, at_seconds(1000));
        fixture.executor.set_available(JobType::Cli, true);

        REQUIRE(fixture.scheduler->start());
        CHECK(fixture.event_loop.loop->process_events(EventFlag::Watchers) == ProcessEventsResult::Stopped);
        CHECK(fixture.event_loop.backend->last_timeout_ms >= 59000);
        CHECK(fixture.event_loop.backend->last_timeout_ms <= 60000);
    }
}

TEST_CASE("Scheduler coalesces rescans onto the event loop and replaces a later wake",
          "[jobu][scheduler][event-loop][rescan][sqlite]")
{
    SchedulerFixture fixture;
    auto const       queue = fixture.create_queue();
    fixture.create_job(queue, JobType::Cli, at_seconds(200));
    fixture.executor.set_available(JobType::Cli, true);
    REQUIRE(fixture.scheduler->start());

    fixture.time.set_utc(at_seconds(200));
    fixture.scheduler->request_rescan();
    fixture.scheduler->request_rescan();
    fixture.scheduler->request_rescan();
    CHECK(fixture.executor.start_requests().empty());

    CHECK(fixture.event_loop.loop->process_events(EventFlag::Timers) == ProcessEventsResult::Stopped);
    REQUIRE(fixture.executor.start_requests().size() == 1U);
    CHECK(fixture.event_loop.loop->process_events(EventFlag::Timers) == ProcessEventsResult::Stopped);
    CHECK(fixture.executor.start_requests().size() == 1U);
}

TEST_CASE("Scheduler stop retains completion persistence without restarting dispatch",
          "[jobu][scheduler][event-loop][stop][completion][sqlite]")
{
    SchedulerFixture fixture;
    auto const       queue = fixture.create_queue();
    fixture.create_job(queue, JobType::Cli, at_seconds(90));
    fixture.create_job(queue, JobType::Cli, at_seconds(90));
    fixture.executor.set_available(JobType::Cli, true);
    REQUIRE(fixture.scheduler->start());
    REQUIRE(fixture.executor.pending_keys().size() == 1U);
    auto const key = fixture.executor.pending_keys().front();

    fixture.scheduler->stop();
    REQUIRE(fixture.executor.complete(key, success(key)));
    auto persisted = fixture.runs.find_by_id(key.run_id);
    REQUIRE(persisted);
    REQUIRE(persisted->has_value());
    CHECK(persisted->value().state == RunState::Succeeded);
    CHECK(fixture.event_loop.loop->process_events(EventFlag::Timers) == ProcessEventsResult::Stopped);
    CHECK(fixture.executor.start_requests().size() == 1U);
    CHECK(fixture.scheduler->state() == SchedulerState::Stopped);
}

TEST_CASE("Scheduler exposes cancellation only while running and schedules a later rescan",
          "[jobu][scheduler][event-loop][cancellation][sqlite]")
{
    SchedulerFixture fixture;
    auto const       queue = fixture.create_queue();
    auto const       run   = fixture.create_job(queue, JobType::Cli, at_seconds(200));

    auto stopped = fixture.scheduler->cancel_run(run.id);
    REQUIRE_FALSE(stopped);
    CHECK(stopped.error().code == "jobu.scheduler.invalid_state");

    REQUIRE(fixture.scheduler->start());
    auto cancelled = fixture.scheduler->cancel_run(run.id);
    REQUIRE(cancelled);
    CHECK(cancelled->disposition == CancelDisposition::Completed);
    CHECK(cancelled->run.state == RunState::Cancelled);
    CHECK(fixture.event_loop.loop->process_events(EventFlag::Timers) == ProcessEventsResult::Stopped);
    CHECK(fixture.scheduler->state() == SchedulerState::Running);
}

TEST_CASE("Scheduler stores and emits the first synchronous cycle failure once",
          "[jobu][scheduler][event-loop][failure][sqlite]")
{
    SchedulerFixture fixture;
    auto const       queue = fixture.create_queue();
    fixture.create_job(queue, JobType::Cli, at_seconds(200));
    fixture.executor.set_available(JobType::Cli, true);
    execute(fixture.database, "UPDATE jobu_runs SET attributes_json = '{}'");
    auto failures = std::vector<Error>{};
    fixture.scheduler->failed.connect([&failures](Error const& error) -> void { failures.push_back(error); });

    auto started = fixture.scheduler->start();
    REQUIRE_FALSE(started);
    REQUIRE(failures.size() == 1U);
    CHECK(failures.front() == started.error());
    CHECK(fixture.scheduler->state() == SchedulerState::Failed);
    REQUIRE(fixture.scheduler->failure());
    CHECK(*fixture.scheduler->failure() == failures.front());

    auto repeated = fixture.scheduler->start();
    REQUIRE_FALSE(repeated);
    CHECK(repeated.error() == failures.front());
    CHECK(failures.size() == 1U);
}

TEST_CASE("Scheduler observes an asynchronous completion failure and fails once",
          "[jobu][scheduler][event-loop][completion][failure][sqlite]")
{
    SchedulerFixture fixture;
    auto const       queue = fixture.create_queue();
    fixture.create_job(queue, JobType::Cli, at_seconds(90));
    fixture.executor.set_available(JobType::Cli, true);
    auto failures = std::vector<Error>{};
    fixture.scheduler->failed.connect([&failures](Error const& error) -> void { failures.push_back(error); });
    REQUIRE(fixture.scheduler->start());
    REQUIRE(fixture.executor.pending_keys().size() == 1U);
    auto const key = fixture.executor.pending_keys().front();
    execute(fixture.database,
            "CREATE TRIGGER fail_scheduler_adapter_completion BEFORE UPDATE OF state ON jobu_runs "
            "WHEN OLD.state = 'running' AND NEW.state = 'succeeded' "
            "BEGIN SELECT RAISE(ABORT, 'injected adapter completion failure'); END");

    REQUIRE(fixture.executor.complete(key, success(key)));
    REQUIRE(failures.size() == 1U);
    CHECK(failures.front().code == "db.constraint");
    CHECK(fixture.scheduler->state() == SchedulerState::Failed);
    REQUIRE(fixture.scheduler->failure());
    CHECK(*fixture.scheduler->failure() == failures.front());

    fixture.scheduler->request_rescan();
    auto restarted = fixture.scheduler->start();
    REQUIRE_FALSE(restarted);
    CHECK(restarted.error() == failures.front());
    CHECK(failures.size() == 1U);
}
