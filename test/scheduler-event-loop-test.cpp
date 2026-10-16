#include "scheduler.hpp"

#include "attempt_repository_priv.hpp"
#include "attribute_registry.hpp"
#include "database.hpp"
#include "event_loop.hpp"
#include "json.hpp"
#include "management.hpp"
#include "query.hpp"
#include "run_repository_priv.hpp"
#include "sqlite/sqlite_driver.hpp"
#include "sqlite/sqlite_schema.hpp"
#include "support/catch_utils.hpp" // IWYU pragma: keep for Catch::StringMaker specializations
#include "support/fake_attempt_executor.hpp"
#include "support/fake_cron_engine.hpp"
#include "support/fake_event_loop_backend.hpp"
#include "support/fake_time_source.hpp"
#include "support/rejecting_secret_provider.hpp"
#include "support/sequence_uuid_generator.hpp"
#include "support/temporary_directory.hpp"
#include "transaction.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
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

auto test_ids() -> std::vector<Uuid>
{
    auto values = std::vector<Uuid>{};
    values.reserve(200);
    for (auto suffix = std::uint16_t{1}; suffix <= 200; ++suffix) {
        values.push_back(id(static_cast<std::uint8_t>(suffix)));
    }
    return values;
}

auto at_seconds(std::int64_t seconds, std::chrono::nanoseconds fraction = {}) -> UtcTimePoint
{
    // Preserve sub-microsecond ticks where supported; macOS's UTC clock only represents microseconds.
    return UtcTimePoint{std::chrono::seconds{seconds}} + std::chrono::duration_cast<UtcTimePoint::duration>(fraction);
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
    result.data = type == JobType::Cli ? JsonValue::Object{{"command", json_string("/test")}}
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
        scheduler  = std::make_unique<Scheduler>(database, registry, cron, generator, time, executor, secrets, options);
        scheduler->attempt_started.connect(scheduler.get(),
                                           [this](AttemptStarted const& value) { starts.push_back(value); });
        scheduler->attempt_finished.connect(scheduler.get(), [this](AttemptFinished const& value) {
            completions.push_back(value);
            events.emplace_back("attempt");
        });
        scheduler->retry_scheduled.connect(scheduler.get(), [this](RetryScheduled const& value) {
            retries.push_back(value);
            events.emplace_back("retry");
        });
        scheduler->run_finished.connect(scheduler.get(), [this](RunFinished const& value) {
            finishes.push_back(value);
            events.emplace_back("run");
        });
    }

    auto create_queue(std::string name = "queue", std::uint32_t concurrency = 1) const -> Queue
    {
        auto created = management->create_queue({.name = std::move(name), .concurrency_limit = concurrency});
        REQUIRE(created);
        return std::move(created).value();
    }

    auto create_job(Queue const& queue, JobType type, UtcTimePoint planned_at, AttributeSet attributes = {}) -> JobRun
    {
        auto definition = management->create_job({
            .queue      = queue.id,
            .type       = type,
            .schedule   = OnceSchedule{.planned_at = planned_at},
            .attributes = std::move(attributes),
            .payload    = payload(type),
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
    RejectingSecretProvider                secrets;
    StandardAttributeRegistry              registry;
    FakeCronEngine                         cron;
    SequenceUuidGenerator                  generator;
    FakeTimeSource                         time;
    FakeAttemptExecutor                    executor;
    RunRepository                          runs;
    std::vector<AttemptStarted>            starts;
    std::vector<AttemptFinished>           completions;
    std::vector<RetryScheduled>            retries;
    std::vector<RunFinished>               finishes;
    std::vector<std::string_view>          events;
    std::unique_ptr<ManagementService>     management;
    std::unique_ptr<Scheduler>             scheduler;
};

class NotifyingExecutor final : public AttemptExecutor {
public:
    FakeAttemptExecutor   fake;
    std::function<void()> started;

    [[nodiscard]] auto is_available(JobType type) const noexcept -> bool override { return fake.is_available(type); }

    [[nodiscard]] auto start(AttemptStartRequest request, AttemptCompletionHandler completion)
        -> Result<void, Error> override
    {
        auto result = fake.start(std::move(request), std::move(completion));
        // Models a separate infrastructure notification on the start stack, not synchronous attempt completion.
        if (started) {
            started();
        }
        return result;
    }

    [[nodiscard]] auto cancel(AttemptKey const& key) -> Result<void, Error> override { return fake.cancel(key); }
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

TEST_CASE("Scheduler start observers see accepted committed attempts once for both origins and runner families",
          "[jobu][scheduler][event-loop][execution][sqlite]")
{
    for (auto type : {JobType::Cli, JobType::Http}) {
        for (bool manual : {false, true}) {
            DYNAMIC_SECTION("type " << static_cast<int>(type) << " manual " << manual)
            {
                SchedulerFixture fixture;
                // Exercise storage truncation where the native UTC clock supports sub-microsecond ticks.
                fixture.time.set_utc(at_seconds(100, 321ns));
                auto const queue = fixture.create_queue();
                auto       run   = fixture.create_job(queue, type, at_seconds(manual ? 200 : 90));
                if (manual) {
                    auto requested = fixture.management->run_now({.job_id = run.job_id});
                    REQUIRE(requested);
                    run = *requested;
                }
                fixture.executor.set_available(type, true);
                Object receiver;
                fixture.scheduler->attempt_started.connect(&receiver, [&](AttemptStarted const& value) {
                    CHECK(value.execution.queue_id == queue.id);
                    CHECK(value.execution.job_id == run.job_id);
                    CHECK(value.execution.run_id == run.id);
                    CHECK(value.execution.type == type);
                    CHECK(value.execution.origin == (manual ? RunOrigin::Manual : RunOrigin::Scheduled));
                    CHECK(value.attempt_number == 1);
                    CHECK(value.started_at == at_seconds(100));
                    CHECK(fixture.scheduler->state() == SchedulerState::Running);
                    CHECK(fixture.executor.pending_keys().size() == 1);

                    auto durable_run = fixture.runs.find_by_id(run.id);
                    REQUIRE(durable_run);
                    REQUIRE(durable_run->has_value());
                    CHECK(durable_run->value().state == RunState::Running);
                    AttemptRepository attempts{fixture.database};
                    auto              attempt = attempts.find(run.id, value.attempt_number);
                    REQUIRE(attempt);
                    REQUIRE(attempt->has_value());
                    CHECK(attempt->value().state == AttemptState::Running);
                    CHECK(attempt->value().started_at == value.started_at);

                    // A new transaction proves the claim helper has released its transaction and queries.
                    auto transaction = Transaction::begin(fixture.database);
                    REQUIRE(transaction);
                    REQUIRE(transaction->rollback());
                    fixture.scheduler->request_rescan();
                });

                REQUIRE(fixture.scheduler->start());
                REQUIRE(fixture.starts.size() == 1);
                REQUIRE(fixture.scheduler->start());
                CHECK(fixture.event_loop.loop->process_events(EventFlag::Timers) == ProcessEventsResult::Stopped);
                CHECK(fixture.starts.size() == 1);
                CHECK(fixture.executor.start_requests().size() == 1);
            }
        }
    }
}

TEST_CASE("Completion observers see committed public events for both origins and runner families",
          "[jobu][scheduler][event-loop][execution][sqlite]")
{
    for (auto type : {JobType::Cli, JobType::Http}) {
        for (bool manual : {false, true}) {
            DYNAMIC_SECTION("type " << static_cast<int>(type) << " manual " << manual)
            {
                SchedulerFixture fixture;
                auto const       queue = fixture.create_queue();
                auto             run   = fixture.create_job(queue, type, at_seconds(manual ? 200 : 90));
                if (manual) {
                    auto requested = fixture.management->run_now({.job_id = run.job_id});
                    REQUIRE(requested);
                    run = *requested;
                }
                fixture.executor.set_available(type, true);
                fixture.scheduler->attempt_finished.connect(fixture.scheduler.get(), [&](AttemptFinished const& value) {
                    CHECK(value.execution.run_id == run.id);
                    CHECK(value.execution.queue_id == queue.id);
                    CHECK(value.execution.job_id == run.job_id);
                    CHECK(value.execution.type == type);
                    CHECK(value.execution.origin == run.origin);
                    auto durable = fixture.runs.find_by_id(run.id);
                    REQUIRE(durable);
                    REQUIRE(durable->has_value());
                    CHECK(durable->value().state == RunState::Succeeded);
                    CHECK(durable->value().completed_at == value.completed_at);
                    auto transaction = Transaction::begin(fixture.database);
                    REQUIRE(transaction);
                    REQUIRE(transaction->rollback());
                    fixture.scheduler->request_rescan();
                });
                REQUIRE(fixture.scheduler->start());
                auto const key = fixture.executor.pending_keys().front();
                fixture.time.set_utc(at_seconds(101, 999ns));
                REQUIRE(fixture.executor.complete(key, success(key)));
                CHECK(fixture.events == std::vector<std::string_view>{"attempt", "run"});
                REQUIRE(fixture.finishes.size() == 1);
                CHECK(fixture.finishes.front().execution.run_id == run.id);
                CHECK(fixture.finishes.front().completing_attempt == key.attempt_number);
                CHECK(fixture.finishes.front().completed_at == at_seconds(101));
                CHECK(fixture.event_loop.loop->process_events(EventFlag::Timers) == ProcessEventsResult::Stopped);
                CHECK(fixture.completions.size() == 1);
            }
        }
    }
}

TEST_CASE("Pending cancellation emits only a terminal run after cleanup and rescan handling",
          "[jobu][scheduler][event-loop][execution][cancellation][sqlite]")
{
    for (bool retry_wait : {false, true}) {
        SchedulerFixture fixture;
        auto const       queue = fixture.create_queue();
        auto             run   = fixture.create_job(queue,
                                                    JobType::Cli,
                                                    at_seconds(retry_wait ? 90 : 200),
                                                    {
                                                        {"retry.initial_delay", {.data = Duration{10s}}  },
                                                        {"retry.max_attempts",  {.data = std::int64_t{2}}}
        });
        fixture.executor.set_available(JobType::Cli, true);
        REQUIRE(fixture.scheduler->start());
        if (retry_wait) {
            auto const key                 = fixture.executor.pending_keys().front();
            auto       completion          = success(key);
            completion.outcome             = AttemptOutcome::Failed;
            completion.failure_disposition = FailureDisposition::Retryable;
            REQUIRE(fixture.executor.complete(key, std::move(completion)));
            REQUIRE(fixture.retries.size() == 1);
            fixture.completions.clear();
            fixture.retries.clear();
            fixture.events.clear();
        }
        fixture.scheduler->run_finished.connect(fixture.scheduler.get(), [&](RunFinished const& event) {
            CHECK(event.execution.run_id == run.id);
            CHECK(event.state == RunState::Cancelled);
            CHECK_FALSE(event.completing_attempt);
            CHECK(event.completed_at == at_seconds(102));
            auto transaction = Transaction::begin(fixture.database);
            REQUIRE(transaction);
            REQUIRE(transaction->rollback());
            CHECK(jb::core::priv::EventLoopTestAccess::active_timer_count(*fixture.event_loop.loop) == 1);
        });

        fixture.time.set_utc(at_seconds(102, 765ns));
        auto cancelled = fixture.scheduler->cancel_run(run.id);
        REQUIRE(cancelled);
        CHECK(cancelled->disposition == CancelDisposition::Completed);
        CHECK(cancelled->run.completed_at == at_seconds(102));
        CHECK(fixture.completions.empty());
        CHECK(fixture.retries.empty());
        CHECK(fixture.events == std::vector<std::string_view>{"run"});
        CHECK_FALSE(fixture.scheduler->cancel_run(run.id));
        CHECK(fixture.finishes.size() == 1);
    }
}

TEST_CASE("Completion receivers can shut down between events without later writes or timer arming",
          "[jobu][scheduler][event-loop][execution][shutdown][sqlite]")
{
    for (bool retry : {false, true}) {
        for (bool at_attempt : {false, true}) {
            DYNAMIC_SECTION("retry " << retry << " attempt receiver " << at_attempt)
            {
                SchedulerFixture fixture;
                auto const       queue            = fixture.create_queue("mixed", 2);
                auto const       retry_attributes = AttributeSet{
                    {"retry.max_attempts", {.data = std::int64_t{2}}}
                };
                fixture.create_job(queue, JobType::Cli, at_seconds(90), retry_attributes);
                fixture.create_job(queue, JobType::Http, at_seconds(90), retry_attributes);
                fixture.create_job(queue, JobType::Cli, at_seconds(200));
                fixture.executor.set_available(JobType::Cli, true);
                fixture.executor.set_available(JobType::Http, true);
                if (at_attempt) {
                    fixture.scheduler->attempt_finished.connect(fixture.scheduler.get(), [&](AttemptFinished const&) {
                        fixture.scheduler->shutdown();
                    });
                }
                else if (retry) {
                    fixture.scheduler->retry_scheduled.connect(fixture.scheduler.get(), [&](RetryScheduled const&) {
                        fixture.scheduler->shutdown();
                    });
                }
                else {
                    fixture.scheduler->run_finished.connect(fixture.scheduler.get(),
                                                            [&](RunFinished const&) { fixture.scheduler->shutdown(); });
                }
                REQUIRE(fixture.scheduler->start());
                auto keys = fixture.executor.pending_keys();
                REQUIRE(keys.size() == 2);
                auto completion = success(keys.front());
                if (retry) {
                    completion.outcome             = AttemptOutcome::Failed;
                    completion.failure_disposition = FailureDisposition::Retryable;
                }
                REQUIRE(fixture.executor.complete(keys.front(), std::move(completion)));
                CHECK(fixture.scheduler->state() == SchedulerState::Shutdown);
                CHECK(fixture.completions.size() == 1);
                CHECK(fixture.retries.size() == (retry && !at_attempt ? 1U : 0U));
                CHECK(fixture.finishes.size() == (!retry && !at_attempt ? 1U : 0U));

                REQUIRE(fixture.executor.complete(keys.back(), success(keys.back())));
                auto unresolved = fixture.runs.find_by_id(keys.back().run_id);
                REQUIRE(unresolved);
                REQUIRE(unresolved->has_value());
                CHECK(unresolved->value().state == RunState::Running);
                CHECK(fixture.event_loop.loop->process_events(EventFlag::All) == ProcessEventsResult::Stopped);
                CHECK(jb::core::priv::EventLoopTestAccess::active_timer_count(*fixture.event_loop.loop) == 0);
                CHECK(fixture.starts.size() == 2);
                CHECK(fixture.completions.size() == 1);
            }
        }
    }
}

TEST_CASE("Queued lifecycle values outlive active attempts and skip destroyed receivers",
          "[jobu][scheduler][event-loop][execution][lifetime][sqlite]")
{
    SchedulerFixture fixture;
    auto const       queue = fixture.create_queue();
    auto const       run   = fixture.create_job(queue,
                                                JobType::Cli,
                                                at_seconds(90),
                                                {
                                                    {"retry.max_attempts", {.data = std::int64_t{2}}}
    });
    fixture.executor.set_available(JobType::Cli, true);
    Object receiver;
    auto   destroyed            = std::make_unique<Object>();
    auto   attempts             = std::vector<AttemptFinished>{};
    auto   retries              = std::vector<RetryScheduled>{};
    auto   finishes             = std::vector<RunFinished>{};
    auto   destroyed_deliveries = 0;
    fixture.scheduler->attempt_finished.connect(
        &receiver,
        [&](AttemptFinished value) { attempts.push_back(value); },
        ConnectionType::Queued);
    fixture.scheduler->retry_scheduled.connect(
        &receiver,
        [&](RetryScheduled value) { retries.push_back(value); },
        ConnectionType::Queued);
    fixture.scheduler->run_finished.connect(
        &receiver,
        [&](RunFinished value) { finishes.push_back(value); },
        ConnectionType::Queued);
    fixture.scheduler->attempt_finished.connect(
        destroyed.get(),
        [&](AttemptFinished const&) { ++destroyed_deliveries; },
        ConnectionType::Queued);
    fixture.scheduler->retry_scheduled.connect(
        destroyed.get(),
        [&](RetryScheduled const&) { ++destroyed_deliveries; },
        ConnectionType::Queued);
    fixture.scheduler->run_finished.connect(
        destroyed.get(),
        [&](RunFinished const&) { ++destroyed_deliveries; },
        ConnectionType::Queued);

    REQUIRE(fixture.scheduler->start());
    auto key                   = fixture.executor.pending_keys().front();
    auto failed                = success(key);
    failed.outcome             = AttemptOutcome::Failed;
    failed.failure_disposition = FailureDisposition::Retryable;
    REQUIRE(fixture.executor.complete(key, std::move(failed)));
    CHECK(fixture.event_loop.loop->process_events(EventFlag::Timers) == ProcessEventsResult::Stopped);
    REQUIRE(fixture.executor.pending_keys().size() == 1);
    key = fixture.executor.pending_keys().front();
    REQUIRE(fixture.executor.complete(key, success(key)));
    CHECK(attempts.empty());
    CHECK(retries.empty());
    CHECK(finishes.empty());
    fixture.scheduler->shutdown();
    destroyed.reset();

    // Active correlation is retired before delivery; retained receiver copies must then outlive the scheduler too.
    CHECK(fixture.event_loop.loop->process_events(EventFlag::Events) == ProcessEventsResult::Stopped);
    fixture.scheduler.reset();
    REQUIRE(attempts.size() == 2);
    CHECK(attempts.front().outcome == AttemptOutcome::Failed);
    CHECK(attempts.back().outcome == AttemptOutcome::Succeeded);
    REQUIRE(retries.size() == 1);
    CHECK(retries.front().execution.run_id == run.id);
    CHECK(retries.front().next_attempt == 2);
    REQUIRE(finishes.size() == 1);
    CHECK(finishes.front().execution.run_id == run.id);
    CHECK(finishes.front().completing_attempt == 2);
    CHECK(destroyed_deliveries == 0);
}

TEST_CASE("Queued attempt starts retain their values after completion and skip destroyed receivers",
          "[jobu][scheduler][event-loop][execution][lifetime][sqlite]")
{
    SchedulerFixture fixture;
    auto const       queue = fixture.create_queue();
    auto const       run   = fixture.create_job(queue, JobType::Cli, at_seconds(90));
    fixture.executor.set_available(JobType::Cli, true);
    Object                        receiver;
    auto                          destroyed_receiver = std::make_unique<Object>();
    std::optional<AttemptStarted> delivered;
    unsigned                      destroyed_deliveries = 0;
    fixture.scheduler->attempt_started.connect(
        &receiver,
        [&](AttemptStarted const& value) { delivered = value; },
        ConnectionType::Queued);
    fixture.scheduler->attempt_started.connect(
        destroyed_receiver.get(),
        [&](AttemptStarted const&) { ++destroyed_deliveries; },
        ConnectionType::Queued);

    REQUIRE(fixture.scheduler->start());
    CHECK_FALSE(delivered);
    auto const key = fixture.executor.pending_keys().front();
    REQUIRE(fixture.executor.complete(key, success(key)));
    fixture.scheduler->shutdown();
    destroyed_receiver.reset();

    // The dispatch return and active correlation are gone before queued delivery runs.
    fixture.time.advance(10s);
    CHECK(fixture.event_loop.loop->process_events(EventFlag::Events) == ProcessEventsResult::Stopped);
    REQUIRE(delivered);
    CHECK(delivered->execution.queue_id == queue.id);
    CHECK(delivered->execution.job_id == run.job_id);
    CHECK(delivered->execution.run_id == run.id);
    CHECK(delivered->execution.type == JobType::Cli);
    CHECK(delivered->execution.origin == RunOrigin::Scheduled);
    CHECK(delivered->attempt_number == key.attempt_number);
    CHECK(delivered->started_at == at_seconds(100));
    CHECK(destroyed_deliveries == 0);
}

TEST_CASE("Shutdown from an accepted-start signal prevents later work and wake arming",
          "[jobu][scheduler][event-loop][execution][shutdown][sqlite]")
{
    SchedulerFixture fixture;
    auto const       queue = fixture.create_queue("mixed", 3);
    fixture.create_job(queue, JobType::Cli, at_seconds(90));
    fixture.create_job(queue, JobType::Http, at_seconds(90));
    fixture.create_job(queue, JobType::Cli, at_seconds(200));
    fixture.executor.set_available(JobType::Cli, true);
    fixture.executor.set_available(JobType::Http, true);
    fixture.scheduler->attempt_started.connect(fixture.scheduler.get(),
                                               [&](AttemptStarted const&) { fixture.scheduler->shutdown(); });

    auto started = fixture.scheduler->start();
    REQUIRE_FALSE(started);
    CHECK(started.error().code == "jobu.scheduler.stopping");
    CHECK(fixture.scheduler->state() == SchedulerState::Shutdown);
    CHECK_FALSE(fixture.scheduler->failure());
    REQUIRE(fixture.starts.size() == 1);
    CHECK(fixture.executor.start_requests().size() == 1);
    CHECK(fixture.executor.cancel_calls().empty());
    CHECK(jb::core::priv::EventLoopTestAccess::active_timer_count(*fixture.event_loop.loop) == 0);

    // A legitimate executor callback delivered later must leave unresolved Running work for recovery.
    auto const key = fixture.executor.pending_keys().front();
    REQUIRE(fixture.event_loop.loop->post([&]() { REQUIRE(fixture.executor.complete(key, success(key))); }));
    CHECK(fixture.event_loop.loop->process_events(EventFlag::All) == ProcessEventsResult::Stopped);
    auto run = fixture.runs.find_by_id(key.run_id);
    REQUIRE(run);
    REQUIRE(run->has_value());
    CHECK(run->value().state == RunState::Running);
    AttemptRepository attempts{fixture.database};
    auto              attempt = attempts.find(key.run_id, key.attempt_number);
    REQUIRE(attempt);
    REQUIRE(attempt->has_value());
    CHECK(attempt->value().state == AttemptState::Running);
    CHECK(fixture.starts.size() == 1);
    CHECK(jb::core::priv::EventLoopTestAccess::active_timer_count(*fixture.event_loop.loop) == 0);
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
        RejectingSecretProvider   secrets;
        StandardAttributeRegistry registry;
        FakeCronEngine            cron;
        SequenceUuidGenerator     generator{test_ids()};
        FakeTimeSource            time;
        FakeAttemptExecutor       executor;
        Scheduler                 scheduler{database, registry, cron, generator, time, executor, secrets};

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

TEST_CASE("Scheduler destruction disarms an armed wake", "[jobu][scheduler][event-loop][wake][lifetime][sqlite]")
{
    SchedulerFixture fixture;
    auto const       baseline = jb::core::priv::EventLoopTestAccess::active_timer_count(*fixture.event_loop.loop);
    auto const       queue    = fixture.create_queue();
    fixture.create_job(queue, JobType::Cli, at_seconds(200));
    fixture.executor.set_available(JobType::Cli, true);

    REQUIRE(fixture.scheduler->start());
    REQUIRE(jb::core::priv::EventLoopTestAccess::active_timer_count(*fixture.event_loop.loop) == baseline + 1U);

    fixture.scheduler.reset();
    CHECK(jb::core::priv::EventLoopTestAccess::active_timer_count(*fixture.event_loop.loop) == baseline);
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

TEST_CASE("Management mutations request one later scheduler rescan",
          "[jobu][scheduler][management][signal][rescan][sqlite]")
{
    SchedulerFixture fixture;
    fixture.executor.set_available(JobType::Cli, true);
    REQUIRE(fixture.scheduler->start());
    auto* scheduler = fixture.scheduler.get();
    // The receiver-aware connection deactivates the Object-capturing slot with the scheduler's lifetime.
    fixture.management->mutation_committed.connect(scheduler, [scheduler]() -> void { scheduler->request_rescan(); });

    auto const queue = fixture.create_queue();
    fixture.create_job(queue, JobType::Cli, at_seconds(90));
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
    CHECK(fixture.events == std::vector<std::string_view>{"attempt", "run"});
    auto persisted = fixture.runs.find_by_id(key.run_id);
    REQUIRE(persisted);
    REQUIRE(persisted->has_value());
    CHECK(persisted->value().state == RunState::Succeeded);
    CHECK(fixture.event_loop.loop->process_events(EventFlag::Timers) == ProcessEventsResult::Stopped);
    CHECK(fixture.executor.start_requests().size() == 1U);
    CHECK(fixture.scheduler->state() == SchedulerState::Stopped);

    REQUIRE(fixture.scheduler->start());
    CHECK(fixture.executor.start_requests().size() == 2U);
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
    CHECK(repeated.error().code == "jobu.scheduler.stopping");
    CHECK(*fixture.scheduler->failure() == failures.front());
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
    CHECK(restarted.error().code == "jobu.scheduler.stopping");
    CHECK(*fixture.scheduler->failure() == failures.front());
    CHECK(failures.size() == 1U);
}

TEST_CASE("Scheduler shutdown is terminal before startup and after resumable stop",
          "[jobu][scheduler][event-loop][shutdown][sqlite]")
{
    SchedulerFixture fixture;
    auto const       queue = fixture.create_queue();
    auto const       run   = fixture.create_job(queue, JobType::Cli, at_seconds(90));
    fixture.executor.set_available(JobType::Cli, true);
    SECTION("before first start")
    {}
    SECTION("after stop")
    {
        fixture.time.set_utc(at_seconds(80));
        REQUIRE(fixture.scheduler->start());
        fixture.scheduler->stop();
    }

    fixture.scheduler->shutdown();
    fixture.scheduler->shutdown();
    fixture.scheduler->stop();
    fixture.scheduler->request_rescan();
    CHECK(fixture.scheduler->state() == SchedulerState::Shutdown);
    CHECK_FALSE(fixture.scheduler->failure());
    auto started = fixture.scheduler->start();
    REQUIRE_FALSE(started);
    CHECK(started.error().code == "jobu.scheduler.stopping");
    CHECK(started.error().category == ErrorCategory::Unavailable);
    auto cancelled = fixture.scheduler->cancel_run(run.id);
    REQUIRE_FALSE(cancelled);
    CHECK(cancelled.error().code == "jobu.scheduler.stopping");
    CHECK(fixture.executor.start_requests().empty());
    CHECK(fixture.executor.cancel_calls().empty());
    CHECK(jb::core::priv::EventLoopTestAccess::active_timer_count(*fixture.event_loop.loop) == 0U);
}

TEST_CASE("Scheduler shutdown suppresses queued completions and leaves durable running work for recovery",
          "[jobu][scheduler][event-loop][shutdown][completion][sqlite]")
{
    SchedulerFixture fixture;
    auto const       queue = fixture.create_queue("mixed", 2);
    fixture.create_job(queue, JobType::Cli, at_seconds(90));
    fixture.create_job(queue, JobType::Http, at_seconds(90));
    fixture.create_job(queue, JobType::Cli, at_seconds(200));
    fixture.executor.set_available(JobType::Cli, true);
    fixture.executor.set_available(JobType::Http, true);
    REQUIRE(fixture.scheduler->start());
    auto const keys = fixture.executor.pending_keys();
    REQUIRE(keys.size() == 2U);
    fixture.scheduler->request_rescan();
    REQUIRE(jb::core::priv::EventLoopTestAccess::active_timer_count(*fixture.event_loop.loop) == 1U);
    REQUIRE(fixture.event_loop.loop->post([&]() {
        for (auto const& key : keys) {
            REQUIRE(fixture.executor.complete(key, success(key)));
        }
        fixture.scheduler->request_rescan();
    }));

    fixture.scheduler->shutdown();
    CHECK(fixture.event_loop.loop->process_events(EventFlag::All) == ProcessEventsResult::Stopped);
    CHECK(fixture.executor.pending_keys().empty());
    CHECK(fixture.executor.start_requests().size() == 2U);
    CHECK(fixture.executor.cancel_calls().empty());
    CHECK(jb::core::priv::EventLoopTestAccess::active_timer_count(*fixture.event_loop.loop) == 0U);
    for (auto const& key : keys) {
        auto run = fixture.runs.find_by_id(key.run_id);
        REQUIRE(run);
        REQUIRE(run->has_value());
        CHECK(run->value().state == RunState::Running);
    }
}

TEST_CASE("Scheduler destruction invalidates outstanding executor completions",
          "[jobu][scheduler][event-loop][shutdown][lifetime][sqlite]")
{
    SchedulerFixture fixture;
    auto const       queue = fixture.create_queue();
    fixture.create_job(queue, JobType::Cli, at_seconds(90));
    fixture.executor.set_available(JobType::Cli, true);
    REQUIRE(fixture.scheduler->start());
    auto const key = fixture.executor.pending_keys().front();
    fixture.scheduler.reset();

    REQUIRE(fixture.executor.complete(key, success(key)));
    auto run = fixture.runs.find_by_id(key.run_id);
    REQUIRE(run);
    REQUIRE(run->has_value());
    CHECK(run->value().state == RunState::Running);
    CHECK(jb::core::priv::EventLoopTestAccess::active_timer_count(*fixture.event_loop.loop) == 0U);
}

TEST_CASE("Scheduler failure slots can shut down and deliver another completion without persistence",
          "[jobu][scheduler][event-loop][shutdown][failure][sqlite]")
{
    SchedulerFixture fixture;
    auto const       queue = fixture.create_queue("parallel", 2);
    fixture.create_job(queue, JobType::Cli, at_seconds(90));
    fixture.create_job(queue, JobType::Cli, at_seconds(90));
    fixture.executor.set_available(JobType::Cli, true);
    REQUIRE(fixture.scheduler->start());
    auto const keys = fixture.executor.pending_keys();
    REQUIRE(keys.size() == 2U);
    execute(fixture.database,
            "CREATE TRIGGER fail_first_completion BEFORE UPDATE OF state ON jobu_runs "
            "WHEN NEW.state = 'succeeded' BEGIN SELECT RAISE(ABORT, 'secret backend diagnostic'); END");
    auto  failures  = std::vector<Error>{};
    auto* scheduler = fixture.scheduler.get();
    scheduler->failed.connect(scheduler, [&](Error const& error) {
        failures.push_back(error);
        CHECK(scheduler->state() == SchedulerState::Failed);
        CHECK(scheduler->failure() == error);
        scheduler->shutdown();
        scheduler->shutdown();
        execute(fixture.database, "DROP TRIGGER fail_first_completion");
        REQUIRE(fixture.executor.complete(keys[1], success(keys[1])));
    });

    REQUIRE(fixture.executor.complete(keys[0], success(keys[0])));
    REQUIRE(failures.size() == 1U);
    CHECK(failures.front().code == "db.constraint");
    CHECK(failures.front().detail.find("secret") == std::string::npos);
    CHECK(failures.front().message.find("secret") == std::string::npos);
    CHECK(scheduler->state() == SchedulerState::Failed);
    CHECK(scheduler->failure() == failures.front());
    auto restarted = scheduler->start();
    REQUIRE_FALSE(restarted);
    CHECK(restarted.error().code == "jobu.scheduler.stopping");
    auto cancelled = scheduler->cancel_run(keys.front().run_id);
    REQUIRE_FALSE(cancelled);
    CHECK(cancelled.error().code == "jobu.scheduler.stopping");
    CHECK(fixture.executor.cancel_calls().empty());
    for (auto const& key : keys) {
        auto run = fixture.runs.find_by_id(key.run_id);
        REQUIRE(run);
        REQUIRE(run->has_value());
        CHECK(run->value().state == RunState::Running);
    }
}

TEST_CASE("Scheduler does not dispatch again or rearm after reentrant shutdown during start",
          "[jobu][scheduler][event-loop][shutdown][reentrancy][sqlite]")
{
    SchedulerFixture  fixture;
    NotifyingExecutor executor;
    auto const        queue = fixture.create_queue("mixed", 3);
    fixture.create_job(queue, JobType::Cli, at_seconds(90));
    fixture.create_job(queue, JobType::Http, at_seconds(90));
    fixture.create_job(queue, JobType::Cli, at_seconds(200));
    executor.fake.set_available(JobType::Cli, true);
    executor.fake.set_available(JobType::Http, true);
    Scheduler scheduler{fixture.database,
                        fixture.registry,
                        fixture.cron,
                        fixture.generator,
                        fixture.time,
                        executor,
                        fixture.secrets};
    executor.started = [&]() { scheduler.shutdown(); };

    auto started = scheduler.start();
    REQUIRE_FALSE(started);
    CHECK(started.error().code == "jobu.scheduler.stopping");
    CHECK(scheduler.state() == SchedulerState::Shutdown);
    CHECK_FALSE(scheduler.failure());
    CHECK(executor.fake.start_requests().size() == 1U);
    CHECK(executor.fake.cancel_calls().empty());
    CHECK(jb::core::priv::EventLoopTestAccess::active_timer_count(*fixture.event_loop.loop) == 0U);
    REQUIRE(executor.fake.pending_keys().size() == 1U);
    auto const key = executor.fake.pending_keys().front();
    REQUIRE(executor.fake.complete(key, success(key)));
    auto run = fixture.runs.find_by_id(key.run_id);
    REQUIRE(run);
    REQUIRE(run->has_value());
    CHECK(run->value().state == RunState::Running);
}

TEST_CASE("Scheduler cancellation storage failure closes completion acceptance after rollback",
          "[jobu][scheduler][event-loop][shutdown][cancellation][sqlite]")
{
    SchedulerFixture fixture;
    auto const       queue = fixture.create_queue();
    fixture.create_job(queue, JobType::Cli, at_seconds(90));
    auto const pending = fixture.create_job(queue, JobType::Cli, at_seconds(200));
    fixture.executor.set_available(JobType::Cli, true);
    REQUIRE(fixture.scheduler->start());
    auto const key     = fixture.executor.pending_keys().front();
    auto       unknown = fixture.scheduler->cancel_run(id(250));
    REQUIRE_FALSE(unknown);
    CHECK(unknown.error().code == "jobu.run.not_found");
    CHECK(fixture.scheduler->state() == SchedulerState::Running);
    execute(fixture.database,
            "CREATE TRIGGER fail_cancellation BEFORE UPDATE OF state ON jobu_runs "
            "WHEN NEW.state = 'cancelled' BEGIN SELECT RAISE(ABORT, 'injected cancellation failure'); END");
    auto  failures  = std::vector<Error>{};
    auto* scheduler = fixture.scheduler.get();
    scheduler->failed.connect(scheduler, [&](Error const& error) {
        failures.push_back(error);
        scheduler->shutdown();
        REQUIRE(fixture.executor.complete(key, success(key)));
    });

    auto cancelled = scheduler->cancel_run(pending.id);
    REQUIRE_FALSE(cancelled);
    CHECK(cancelled.error().code == "db.constraint");
    REQUIRE(failures.size() == 1U);
    CHECK(cancelled.error() == failures.front());
    CHECK(scheduler->state() == SchedulerState::Failed);
    auto unchanged = fixture.runs.find_by_id(pending.id);
    REQUIRE(unchanged);
    REQUIRE(unchanged->has_value());
    CHECK(unchanged->value().state == RunState::Scheduled);
    auto running = fixture.runs.find_by_id(key.run_id);
    REQUIRE(running);
    REQUIRE(running->has_value());
    CHECK(running->value().state == RunState::Running);
}

TEST_CASE("Scheduler immediate completion failure stops the remaining mixed dispatch opportunities",
          "[jobu][scheduler][event-loop][shutdown][reentrancy][failure][sqlite]")
{
    SchedulerFixture fixture;
    auto const       queue = fixture.create_queue("mixed", 3);
    auto const       cli   = fixture.create_job(queue, JobType::Cli, at_seconds(90));
    auto const       http  = fixture.create_job(queue, JobType::Http, at_seconds(90));
    fixture.executor.set_available(JobType::Cli, true);
    fixture.executor.set_available(JobType::Http, true);
    fixture.executor.set_start_error(Error{.category = ErrorCategory::Unavailable,
                                           .code     = "test.executor.start_failed",
                                           .message  = "Configured start failure"});
    execute(fixture.database,
            "CREATE TRIGGER fail_immediate_completion BEFORE UPDATE OF state ON jobu_runs "
            "WHEN NEW.state = 'failed' BEGIN SELECT RAISE(ABORT, 'injected immediate completion failure'); END");
    auto  failures  = std::vector<Error>{};
    auto* scheduler = fixture.scheduler.get();
    scheduler->failed.connect(scheduler, [&](Error const& error) {
        failures.push_back(error);
        scheduler->shutdown();
    });

    auto started = scheduler->start();
    REQUIRE_FALSE(started);
    REQUIRE(failures.size() == 1U);
    CHECK(started.error() == failures.front());
    CHECK(started.error().code == "db.constraint");
    CHECK(scheduler->state() == SchedulerState::Failed);
    CHECK(scheduler->failure() == failures.front());
    REQUIRE(fixture.executor.start_requests().size() == 1U);
    CHECK(fixture.executor.start_requests().front().key.run_id == cli.id);
    CHECK(jb::core::priv::EventLoopTestAccess::active_timer_count(*fixture.event_loop.loop) == 0U);
    for (auto const run_id : {cli.id, http.id}) {
        auto run = fixture.runs.find_by_id(run_id);
        REQUIRE(run);
        REQUIRE(run->has_value());
        CHECK(run->value().state == (run_id == cli.id ? RunState::Running : RunState::Scheduled));
    }
}
