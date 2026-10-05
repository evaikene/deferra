#include "retention.hpp"

#include "management.hpp"
#include "object.hpp"
#include "query.hpp"
#include "support/catch_utils.hpp" // IWYU pragma: keep for Catch::StringMaker specializations
#include "support/fake_cron_engine.hpp"
#include "support/fake_event_loop_backend.hpp"
#include "support/fake_time_source.hpp"
#include "support/fault_database_driver.hpp"
#include "support/recovery_fixture.hpp"
#include "support/storage_fault_helpers.hpp"
#include "transaction.hpp"
#include "uuid.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

using namespace jb::core;
using namespace jb::jobu;
using namespace jb::test;
using namespace std::chrono_literals;
using LoopAccess = jb::core::priv::EventLoopTestAccess;

namespace {

struct ServiceFixture {
    ServiceFixture()
        : storage{[this](std::unique_ptr<jb::db::Driver> driver) {
            return std::make_unique<FaultDatabaseDriver>(std::move(driver), faults);
        }}
    {
        time.set_utc(UtcTimePoint{30s});
        faults->classify = [](std::string_view sql) {
            return sql.starts_with("DELETE FROM jobu_runs") ? std::string{"run.delete"} : std::string{"query"};
        };
    }

    auto queue(std::uint32_t                       suffix,
               std::optional<std::chrono::seconds> retention = std::nullopt,
               QueueState                          state     = QueueState::Active) -> Queue
    {
        auto value              = recovery_queue(recovery_id(suffix), state);
        value.history_retention = retention;
        storage.insert_queue(value);
        return value;
    }

    auto terminal(Queue const& queue_value, std::uint32_t suffix) -> RecoveryRunFixture
    {
        auto job  = storage.make_job(recovery_id(100 + suffix), queue_value.id);
        job.state = JobState::Succeeded;
        auto run  = storage.make_run(recovery_id(200 + suffix), job, RunState::Succeeded);
        storage.insert_job(job);
        storage.insert_run(run);
        return run;
    }

    void create_service()
    {
        service = std::make_unique<RetentionService>(storage.database, storage.registry, time, options);
    }

    void observe()
    {
        service->batch_completed.connect(&receiver,
                                         [this](RetentionPurgeCounts const& counts) { batches.push_back(counts); });
        service->failed.connect(&receiver, [this](Error const& error) { failures.push_back(error); });
        service->sweep_completed.connect(&receiver,
                                         [this](RetentionPurgeCounts const& counts) { sweeps.push_back(counts); });
    }

    void fire_next() const
    {
        auto deadline = LoopAccess::next_timer_deadline(*loop.loop);
        REQUIRE(deadline);
        // Retention UTC is independent of timer time. Make only the earliest timer due without
        // advancing real time; the next positive-delay registration remains for another turn.
        LoopAccess::fire_next_timer(*loop.loop);
    }

    void fire_and_check_delay(Duration expected) const
    {
        auto before = Clock::now();
        auto count  = batches.size();
        fire_next();
        auto after = Clock::now();
        REQUIRE(batches.size() == count + 1);
        REQUIRE(LoopAccess::active_timer_count(*loop.loop) == 1);
        auto deadline = LoopAccess::next_timer_deadline(*loop.loop);
        REQUIRE(deadline);
        CHECK(*deadline >= before + expected);
        CHECK(*deadline <= after + expected);
    }

    auto count(std::string_view table) -> std::int64_t
    {
        jb::db::Query query{storage.database};
        REQUIRE(query.exec(std::string{"SELECT COUNT(*) FROM "} + std::string{table}));
        auto next = query.next();
        REQUIRE(next);
        REQUIRE(*next);
        REQUIRE(std::holds_alternative<std::int64_t>(query.value(0)));
        return std::get<std::int64_t>(query.value(0));
    }

    jb::core::priv::FakeEventLoop          loop{jb::core::priv::make_fake_event_loop()};
    jb::core::priv::ScopedCurrentEventLoop current{loop.loop.get()};
    std::shared_ptr<DatabaseFaultState>    faults{std::make_shared<DatabaseFaultState>()};
    RecoveryFixture                        storage;
    FakeTimeSource                         time;
    RetentionOptions                       options{.default_retention = 10s};
    Object                                 receiver;
    std::vector<RetentionPurgeCounts>      batches;
    std::vector<RetentionPurgeCounts>      sweeps;
    std::vector<Error>                     failures;
    std::unique_ptr<RetentionService>      service;
};

} // namespace

TEST_CASE("Retention rejects invalid options without SQL or signals", "[jobu][retention][service]")
{
    auto           invalid = GENERATE(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10);
    ServiceFixture f;
    switch (invalid) {
        case 0:
            f.options.default_retention = -1s;
            break;
        case 1:
            f.options.sweep_interval = 0s;
            break;
        case 2:
            f.options.sweep_interval = -1s;
            break;
        case 3:
            f.options.inter_batch_delay = 0ms;
            break;
        case 4:
            f.options.inter_batch_delay = -1ms;
            break;
        case 5:
            f.options.batch_size = 0;
            break;
        case 6:
            f.options.batch_size = 1001;
            break;
        case 7:
            f.options.sweep_interval = std::chrono::seconds::max();
            break;
        case 8:
            f.options.inter_batch_delay = std::chrono::milliseconds::max();
            break;
        case 9:
            f.options.sweep_interval = std::chrono::duration_cast<std::chrono::seconds>(Duration::max());
            break;
        case 10:
            f.options.inter_batch_delay = std::chrono::duration_cast<std::chrono::milliseconds>(Duration::max());
            break;
        default:
            FAIL("Unexpected test option");
    }
    f.create_service();
    f.observe();
    f.faults->calls.clear();
    auto started = f.service->start();
    REQUIRE_FALSE(started);
    CHECK(started.error().category == ErrorCategory::InvalidArgument);
    CHECK(started.error().code == "jobu.retention.invalid_options");
    CHECK(f.faults->calls.empty());
    CHECK(f.failures.empty());
    CHECK(f.batches.empty());
    CHECK(f.sweeps.empty());
    CHECK(LoopAccess::active_timer_count(*f.loop.loop) == 0);
}

TEST_CASE("Retention checks both service and timer event-loop affinity", "[jobu][retention][service]")
{
    ServiceFixture                         f;
    Object                                 parent;
    jb::core::priv::ScopedCurrentEventLoop no_loop{nullptr};
    SECTION("neither object has an event loop")
    {
        f.create_service();
    }
    SECTION("the service inherits its parent's loop but its embedded timer has none")
    {
        f.service =
            std::make_unique<RetentionService>(f.storage.database, f.storage.registry, f.time, f.options, &parent);
    }
    f.faults->calls.clear();
    auto started = f.service->start();
    REQUIRE_FALSE(started);
    CHECK(started.error().code == "jobu.retention.event_loop_unavailable");
    CHECK(f.faults->calls.empty());
    CHECK(LoopAccess::active_timer_count(*f.loop.loop) == 0);
    f.service.reset(); // Remove the parent's child before its stack lifetime ends.
}

TEST_CASE("Retention startup is asynchronous and repeated start preserves its timer", "[jobu][retention][service]")
{
    ServiceFixture f;
    f.terminal(f.queue(1), 1);
    f.create_service();
    f.observe();
    f.faults->calls.clear();
    auto before = Clock::now();
    REQUIRE(f.service->start());
    auto after = Clock::now();
    CHECK(f.faults->calls.empty());
    CHECK(f.batches.empty());
    auto deadline = LoopAccess::next_timer_deadline(*f.loop.loop);
    REQUIRE(deadline);
    CHECK(*deadline >= before + f.options.inter_batch_delay);
    CHECK(*deadline <= after + f.options.inter_batch_delay);
    REQUIRE(f.service->start());
    CHECK(LoopAccess::next_timer_deadline(*f.loop.loop) == deadline);
    f.fire_and_check_delay(f.options.inter_batch_delay);
    CHECK(f.batches.front().runs == 1);
    CHECK(f.count("jobu_runs") == 0);
}

TEST_CASE("Retention rejects start on another thread before doing SQL", "[jobu][retention][service]")
{
    ServiceFixture f;
    f.create_service();
    f.faults->calls.clear();
    std::optional<Result<void, Error>> started;
    std::thread                        caller{[&] { started = f.service->start(); }};
    caller.join();
    REQUIRE(started);
    REQUIRE_FALSE(*started);
    CHECK(started->error().code == "jobu.retention.event_loop_unavailable");
    CHECK(f.faults->calls.empty());
    CHECK(LoopAccess::active_timer_count(*f.loop.loop) == 0);
}

TEST_CASE("Retention accepts the maximum parent bound and saturated retention policy", "[jobu][retention][service]")
{
    ServiceFixture f;
    f.options.batch_size        = 1000;
    f.options.default_retention = std::chrono::seconds::max();
    auto retained               = f.terminal(f.queue(1), 1);
    f.create_service();
    f.observe();
    REQUIRE(f.service->start());
    f.fire_and_check_delay(f.options.inter_batch_delay);
    CHECK(f.batches.front().runs == 0);
    f.storage.require_run(retained);
}

TEST_CASE("Retention yields one bounded batch per callback and visits the next queue", "[jobu][retention][service]")
{
    ServiceFixture f;
    f.options.batch_size = 1;
    auto first           = f.queue(1);
    auto second          = f.queue(2);
    f.terminal(first, 1);
    f.terminal(first, 2);
    f.terminal(second, 3);
    f.terminal(second, 4);
    f.create_service();
    f.observe();
    REQUIRE(f.service->start());
    f.fire_and_check_delay(f.options.inter_batch_delay);
    CHECK(f.count("jobu_runs") == 3);

    // An unrelated owner-thread task runs between the two maintenance transactions.
    bool visited = false;
    REQUIRE(f.loop.loop->post([&] {
        visited = true;
        CHECK(f.count("jobu_runs") == 3);
    }));
    REQUIRE(f.loop.loop->process_events(EventFlag::Tasks) != ProcessEventsResult::Failed);
    REQUIRE(visited);
    f.fire_and_check_delay(f.options.inter_batch_delay);
    CHECK(f.count("jobu_runs") == 2);
    CHECK(f.batches[0].runs == 1);
    CHECK(f.batches[1].runs == 1);
    CHECK(f.failures.empty());
}

TEST_CASE("Retention carries zero-count phases to completion and leaves no SQL resources in signals",
          "[jobu][retention][service]")
{
    ServiceFixture f;
    f.create_service();
    f.observe();
    f.service->batch_completed.connect(&f.receiver, [&](RetentionPurgeCounts const&) {
        // Closing succeeds only when all queries and transaction guards have been released.
        REQUIRE(f.storage.database.close());
        REQUIRE(f.storage.database.open());
        auto begun = jb::db::Transaction::begin(f.storage.database);
        REQUIRE(begun);
        REQUIRE(begun->rollback());
    });
    REQUIRE(f.service->start());
    for (int i = 0; i < 3; ++i) {
        f.fire_and_check_delay(f.options.inter_batch_delay);
    }
    f.fire_and_check_delay(f.options.sweep_interval);
    REQUIRE(f.batches.size() == 4);
    for (auto const& batch : f.batches) {
        CHECK(batch.runs + batch.idempotency_records + batch.jobs + batch.queues == 0);
    }
    // A new sweep resumes at history and yields, even after the preceding sweep did no deletions.
    f.fire_and_check_delay(f.options.inter_batch_delay);
    CHECK(f.failures.empty());
}

TEST_CASE("Retention samples UTC at the first batch and keeps it through the sweep", "[jobu][retention][service]")
{
    ServiceFixture f;
    f.terminal(f.queue(1), 1);
    f.terminal(f.queue(2), 2);
    f.create_service();
    f.observe();
    REQUIRE(f.service->start());

    f.time.set_utc(UtcTimePoint{22s}); // Both completions equal the first sweep's strict cutoff.
    f.fire_and_check_delay(f.options.inter_batch_delay);
    f.time.set_utc(UtcTimePoint{100s});
    f.fire_and_check_delay(f.options.inter_batch_delay);
    CHECK(f.count("jobu_runs") == 2);
    for (int i = 0; i < 3; ++i) {
        f.fire_and_check_delay(f.options.inter_batch_delay);
    }
    f.fire_and_check_delay(f.options.sweep_interval);

    f.time.set_utc(UtcTimePoint{21s}); // A backward wall correction postpones expiry in the next sweep.
    for (int i = 0; i < 5; ++i) {
        f.fire_and_check_delay(f.options.inter_batch_delay);
    }
    f.fire_and_check_delay(f.options.sweep_interval);
    CHECK(f.count("jobu_runs") == 2);

    f.time.set_utc(UtcTimePoint{23s});
    f.fire_and_check_delay(f.options.inter_batch_delay);
    f.fire_and_check_delay(f.options.inter_batch_delay);
    CHECK(f.count("jobu_runs") == 0);
}

TEST_CASE("Unlimited inherited retention still visits finite overrides and deleted owners",
          "[jobu][retention][service]")
{
    ServiceFixture f;
    f.options.default_retention = 0s;
    auto retained               = f.terminal(f.queue(1), 1);
    f.terminal(f.queue(2, 10s), 2);
    f.create_service();
    f.observe();

    // Real management operations establish a valid queue creation replay and deletion tombstone.
    FakeCronEngine    cron;
    UuidV7Generator   generator{f.time};
    ManagementService management{f.storage.database, f.storage.registry, cron, generator, f.time};
    auto              queue = management.create_queue({.name = "deleted", .idempotency_key = "queue"});
    REQUIRE(queue);
    REQUIRE(management.suspend_queue(queue->id));
    REQUIRE(management.delete_queue(queue->id));
    REQUIRE(f.service->start());
    for (int i = 0; i < 6; ++i) {
        f.fire_and_check_delay(f.options.inter_batch_delay);
    }
    f.fire_and_check_delay(f.options.sweep_interval);
    CHECK(f.count("jobu_runs") == 1);
    f.storage.require_run(retained);
    CHECK(f.count("jobu_queues") == 2);
    CHECK(f.batches.back().queues == 1);
    CHECK(f.batches.back().idempotency_records == 1);
    CHECK(f.count("jobu_idempotency") == 0);
}

TEST_CASE("Stop and destruction cancel pending retention callbacks without SQL", "[jobu][retention][service]")
{
    ServiceFixture f;
    auto           retained = f.terminal(f.queue(1), 1);
    f.create_service();
    f.observe();
    REQUIRE(f.service->start());
    f.faults->calls.clear();
    SECTION("idempotent stop")
    {
        f.service->stop();
        f.service->stop();
    }
    SECTION("destruction with an armed timer")
    {
        f.service.reset();
    }
    CHECK(f.faults->calls.empty());
    CHECK(LoopAccess::active_timer_count(*f.loop.loop) == 0);
    LoopAccess::fire_timers(*f.loop.loop, TimePoint::max());
    CHECK(f.faults->calls.empty());
    CHECK(f.batches.empty());
    f.storage.require_run(retained);
}

TEST_CASE("Retention tolerates a synchronous receiver stopping or destroying it", "[jobu][retention][service]")
{
    ServiceFixture f;
    f.terminal(f.queue(1), 1);
    f.terminal(f.queue(2), 2);
    f.create_service();
    f.observe();
    SECTION("stop from a committed batch")
    {
        f.service->batch_completed.connect(&f.receiver, [&](RetentionPurgeCounts const&) { f.service->stop(); });
    }
    SECTION("destroy from a committed batch")
    {
        f.service->batch_completed.connect(&f.receiver, [&](RetentionPurgeCounts const& counts) {
            f.service.reset();
            CHECK(counts.runs == 1); // The borrowed count lives in the callback's owning result, not Private.
        });
    }
    REQUIRE(f.service->start());
    f.fire_next();
    CHECK(f.batches.size() == 1);
    CHECK(f.count("jobu_runs") == 1);
    CHECK(LoopAccess::active_timer_count(*f.loop.loop) == 0);
    auto calls = f.faults->calls;
    LoopAccess::fire_timers(*f.loop.loop, TimePoint::max());
    CHECK(f.faults->calls == calls);
}

TEST_CASE("A reentrant retention restart fences the old callback and resets its sweep", "[jobu][retention][service]")
{
    ServiceFixture f;
    f.terminal(f.queue(1), 1);
    auto retained = f.terminal(f.queue(2), 2);
    f.create_service();
    f.observe();
    f.service->batch_completed.connect(&f.receiver, [&](RetentionPurgeCounts const&) {
        if (f.batches.size() == 1) {
            f.service->stop();
            f.time.set_utc(UtcTimePoint{22s});
            REQUIRE(f.service->start());
        }
    });
    REQUIRE(f.service->start());
    f.fire_and_check_delay(f.options.inter_batch_delay);
    f.fire_and_check_delay(f.options.inter_batch_delay);
    f.fire_and_check_delay(f.options.inter_batch_delay);
    REQUIRE(f.batches.size() == 3);
    CHECK(f.batches[0].runs == 1);
    CHECK(f.batches[1].runs == 0); // Restart revisits the now-empty first queue.
    CHECK(f.batches[2].runs == 0); // New sweep UTC, rather than the stopped sweep's later cutoff.
    f.storage.require_run(retained);
}

TEST_CASE("Queued retention signals own their counts and respect receiver destruction", "[jobu][retention][service]")
{
    ServiceFixture f;
    f.terminal(f.queue(1), 1);
    f.queue(2);
    f.create_service();
    auto                              receiver = std::make_unique<Object>();
    std::vector<RetentionPurgeCounts> queued;
    f.service->batch_completed.connect(
        receiver.get(),
        [&](RetentionPurgeCounts const& counts) { queued.push_back(counts); },
        ConnectionType::Queued);
    REQUIRE(f.service->start());
    f.fire_next();
    f.fire_next();
    CHECK(queued.empty());
    SECTION("owning values survive subsequent callbacks and stop")
    {
        f.service->stop();
        REQUIRE(f.loop.loop->process_events(EventFlag::Events) != ProcessEventsResult::Failed);
        REQUIRE(queued.size() == 2);
        CHECK(queued[0].runs == 1);
        CHECK(queued[1].runs == 0);
    }
    SECTION("a destroyed receiver cancels undrained deliveries")
    {
        receiver.reset();
        REQUIRE(f.loop.loop->process_events(EventFlag::Events) != ProcessEventsResult::Failed);
        CHECK(queued.empty());
    }
}

TEST_CASE("Failed retention latches sanitized mutation errors after cleanup and never retries",
          "[jobu][retention][service][fault]")
{
    auto           fault = GENERATE(DatabaseCall{.boundary = "connection", .operation = DatabaseOperation::Begin},
                                    DatabaseCall{.boundary = "query", .operation = DatabaseOperation::Prepare},
                                    DatabaseCall{.boundary = "query", .operation = DatabaseOperation::Finish},
                                    DatabaseCall{.boundary  = "run.delete",
                                                 .operation = DatabaseOperation::Execute,
                                                 .phase     = DatabaseFaultPhase::AfterSuccess},
                                    DatabaseCall{.boundary = "connection", .operation = DatabaseOperation::Commit});
    ServiceFixture f;
    f.terminal(f.queue(1), 1);
    f.create_service();
    f.observe();
    auto before = storage_snapshot(f.storage.database);
    f.service->failed.connect(&f.receiver, [&](Error const& error) {
        check_safe_error(error, "db.io");
        CHECK(error.detail == "operation=mutation reason=state_operation_failed");
        REQUIRE(f.storage.database.close());
        REQUIRE(f.storage.database.open());
        CHECK(storage_snapshot(f.storage.database) == before);
    });
    REQUIRE(f.service->start());
    f.faults->faults.push_back({.at = fault, .error = fault_error()});
    f.fire_next();
    require_consumed_faults(*f.faults);
    REQUIRE(f.failures.size() == 1);
    CHECK(f.batches.empty());
    CHECK(LoopAccess::active_timer_count(*f.loop.loop) == 0);
    auto calls = f.faults->calls;
    f.service->stop();
    auto restart = f.service->start();
    REQUIRE_FALSE(restart);
    check_safe_error(restart.error(), "db.io");
    LoopAccess::fire_timers(*f.loop.loop, TimePoint::max());
    CHECK(f.faults->calls == calls);
    CHECK(f.failures.size() == 1);
}

TEST_CASE("Retention preserves original fatal errors through poisoned rollback and uncertain commit",
          "[jobu][retention][service][fault]")
{
    ServiceFixture f;
    f.terminal(f.queue(1), 1);
    f.create_service();
    f.observe();
    auto before  = storage_snapshot(f.storage.database);
    bool durable = false;
    SECTION("rollback poisoning cannot replace the original write failure")
    {
        f.faults->faults.push_back({
            .at    = {.boundary  = "run.delete",
                      .operation = DatabaseOperation::Execute,
                      .phase     = DatabaseFaultPhase::AfterSuccess},
            .error = fault_error()
        });
        f.faults->faults.push_back({
            .at    = {.boundary = "connection", .operation = DatabaseOperation::Rollback},
            .error = fault_error("db.rollback_failed")
        });
    }
    SECTION("lost acknowledgement cannot be treated as an uncommitted deletion")
    {
        durable = true;
        f.faults->faults.push_back({
            .at    = {.boundary  = "connection",
                      .operation = DatabaseOperation::Commit,
                      .phase     = DatabaseFaultPhase::AfterSuccess},
            .error = fault_error()
        });
    }
    REQUIRE(f.service->start());
    f.fire_next();
    require_consumed_faults(*f.faults);
    REQUIRE(f.failures.size() == 1);
    check_safe_error(f.failures.front(), "db.io");
    CHECK(f.storage.database.is_poisoned());
    CHECK(f.batches.empty());
    auto calls = f.faults->calls;
    f.service->stop();
    REQUIRE_FALSE(f.service->start());
    LoopAccess::fire_timers(*f.loop.loop, TimePoint::max());
    CHECK(f.faults->calls == calls);
    f.storage.reopen();
    CHECK((storage_snapshot(f.storage.database) != before) == durable);
    CHECK(f.count("jobu_runs") == (durable ? 0 : 1));
}

TEST_CASE("Retention relationship failures retain persisted-data origin even when rollback poisons",
          "[jobu][retention][service][fault]")
{
    auto           poison = GENERATE(false, true);
    ServiceFixture f;
    f.terminal(f.queue(1), 1);
    {
        jb::db::Query query{f.storage.database};
        REQUIRE(query.exec("CREATE TRIGGER skip_run BEFORE DELETE ON jobu_runs BEGIN SELECT RAISE(IGNORE); END"));
    }
    f.create_service();
    f.observe();
    if (poison) {
        f.faults->faults.push_back({
            .at    = {.boundary = "connection", .operation = DatabaseOperation::Rollback},
            .error = fault_error("db.rollback_failed")
        });
    }
    REQUIRE(f.service->start());
    f.fire_next();
    REQUIRE(f.failures.size() == 1);
    CHECK(f.failures.front().code == "jobu.retention.invalid_relationship");
    CHECK(f.failures.front().detail == "operation=mutation reason=durable_invariant");
    CHECK(f.storage.database.is_poisoned() == poison);
    CHECK(f.batches.empty());
    CHECK(LoopAccess::active_timer_count(*f.loop.loop) == 0);
}

TEST_CASE("A failed-signal receiver can destroy retention after transaction cleanup",
          "[jobu][retention][service][fault]")
{
    ServiceFixture f;
    f.queue(1);
    f.create_service();
    f.service->failed.connect(&f.receiver, [&](Error const& error) {
        f.service.reset();
        check_safe_error(error, "db.io");
        REQUIRE(f.storage.database.close());
        REQUIRE(f.storage.database.open());
    });
    REQUIRE(f.service->start());
    f.faults->faults.push_back({
        .at    = {.boundary = "connection", .operation = DatabaseOperation::Commit},
        .error = fault_error()
    });
    f.fire_next();
    require_consumed_faults(*f.faults);
    CHECK_FALSE(f.service);
    CHECK(LoopAccess::active_timer_count(*f.loop.loop) == 0);
}

TEST_CASE("Retention observes an already poisoned database without starting SQL", "[jobu][retention][service][fault]")
{
    ServiceFixture f;
    f.create_service();
    f.observe();
    REQUIRE(f.storage.database.transaction());
    f.faults->faults.push_back({
        .at    = {.boundary = "connection", .operation = DatabaseOperation::Rollback},
        .error = fault_error("db.rollback_failed")
    });
    REQUIRE_FALSE(f.storage.database.rollback());
    REQUIRE(f.storage.database.is_poisoned());
    f.faults->calls.clear();
    REQUIRE(f.service->start());
    f.fire_next();
    CHECK(f.faults->calls.empty());
    REQUIRE(f.failures.size() == 1);
    check_safe_error(f.failures.front(), "db.rollback_failed");
    CHECK(LoopAccess::active_timer_count(*f.loop.loop) == 0);
}

TEST_CASE("Retention summaries aggregate committed batches and reset for the next sweep", "[jobu][retention][service]")
{
    ServiceFixture f;
    f.options.batch_size = 1;
    auto first           = f.queue(1);
    auto second          = f.queue(2);
    f.terminal(first, 1);
    f.terminal(first, 2);
    f.terminal(second, 3);
    f.terminal(second, 4);
    f.create_service();
    f.observe();
    f.service->sweep_completed.connect(&f.receiver, [&](RetentionPurgeCounts const&) {
        // A completed report, like a batch report, owns no live query or transaction.
        REQUIRE(f.storage.database.close());
        REQUIRE(f.storage.database.open());
    });
    REQUIRE(f.service->start());
    for (std::size_t expected = 1; expected <= 3; ++expected) {
        for (int step = 0; f.sweeps.size() < expected && step < 20; ++step) {
            f.fire_next();
        }
        REQUIRE(f.sweeps.size() == expected);
        CHECK(f.sweeps.back().runs == (expected < 3 ? 2U : 0U));
        CHECK(f.sweeps.back().idempotency_records == 0);
        CHECK(f.sweeps.back().jobs == 0);
        CHECK(f.sweeps.back().queues == 0);
    }
    CHECK(f.count("jobu_runs") == 0);
}

TEST_CASE("Stopping a partial sweep discards its diagnostic totals", "[jobu][retention][service]")
{
    ServiceFixture f;
    f.terminal(f.queue(1), 1);
    f.terminal(f.queue(2), 2);
    f.create_service();
    f.observe();
    REQUIRE(f.service->start());
    f.fire_next();
    REQUIRE(f.batches.back().runs == 1);
    f.service->stop();
    CHECK(f.sweeps.empty());
    REQUIRE(f.service->start());
    for (int step = 0; f.sweeps.empty() && step < 20; ++step) {
        f.fire_next();
    }
    REQUIRE(f.sweeps.size() == 1);
    CHECK(f.sweeps.front().runs == 1);
}

TEST_CASE("Sweep summary receivers can stop destroy or restart retention", "[jobu][retention][service]")
{
    ServiceFixture f;
    f.terminal(f.queue(1), 1);
    f.create_service();
    f.observe();
    bool notified = false;
    bool restart  = false;
    f.service->sweep_completed.connect(&f.receiver, [&](RetentionPurgeCounts const& counts) {
        CHECK(counts.runs == 1);
        notified = true;
    });
    SECTION("stop")
    {
        f.service->sweep_completed.connect(&f.receiver, [&](RetentionPurgeCounts const&) { f.service->stop(); });
    }
    SECTION("destroy")
    {
        f.service->sweep_completed.connect(&f.receiver, [&](RetentionPurgeCounts const&) { f.service.reset(); });
    }
    SECTION("restart")
    {
        restart = true;
        f.service->sweep_completed.connect(&f.receiver, [&](RetentionPurgeCounts const&) {
            f.service->stop();
            REQUIRE(f.service->start());
        });
    }
    REQUIRE(f.service->start());
    for (int step = 0; !notified && step < 20; ++step) {
        f.fire_next();
    }
    REQUIRE(notified);
    CHECK(LoopAccess::active_timer_count(*f.loop.loop) == (restart ? 1 : 0));
    if (restart) {
        auto deadline = LoopAccess::next_timer_deadline(*f.loop.loop);
        REQUIRE(deadline);
        CHECK(*deadline < Clock::now() + f.options.sweep_interval);
    }
}

TEST_CASE("Queued sweep summaries own totals and respect receiver destruction", "[jobu][retention][service]")
{
    ServiceFixture f;
    f.terminal(f.queue(1), 1);
    f.create_service();
    f.observe();
    auto                              receiver = std::make_unique<Object>();
    std::vector<RetentionPurgeCounts> delivered;
    f.service->sweep_completed.connect(
        receiver.get(),
        [&](RetentionPurgeCounts const& counts) { delivered.push_back(counts); },
        ConnectionType::Queued);
    REQUIRE(f.service->start());
    for (int step = 0; f.sweeps.empty() && step < 20; ++step) {
        f.fire_next();
    }
    REQUIRE(f.sweeps.size() == 1);
    CHECK(delivered.empty());
    f.service->stop();
    SECTION("retained report")
    {}
    SECTION("destroyed receiver")
    {
        receiver.reset();
    }
    REQUIRE(f.loop.loop->process_events(EventFlag::Events) != ProcessEventsResult::Failed);
    REQUIRE(delivered.size() == (receiver ? 1 : 0));
    if (receiver) {
        CHECK(delivered.front().runs == 1);
    }
}
