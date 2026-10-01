#include "execution_telemetry.hpp"

#include "execution_telemetry_priv.hpp"
#include "object.hpp"
#include "run_repository_priv.hpp"
#include "support/fake_event_loop_backend.hpp"
#include "support/fake_time_source.hpp"
#include "support/sequence_uuid_generator.hpp"
#include "support/storage_fault_helpers.hpp"
#include "support/telemetry_fixture.hpp"
#include "transaction.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace jb::core;
using namespace jb::jobu;
using namespace jb::jobu::detail;
using namespace jb::test;
using namespace std::chrono_literals;
using LoopAccess = jb::core::priv::EventLoopTestAccess;

namespace {

struct TelemetryFixture : TelemetryStorageFixture {
    TelemetryFixture()
    {
        time.set_utc(UtcTimePoint{10s});
        time.set_monotonic(TimePoint{100s});
    }

    void create(TelemetryOptions options = {})
    {
        service = std::make_unique<ExecutionTelemetry>(storage.database, storage.registry, time, generator, options);
        service->failed.connect(&receiver, [this](Error const& error) { failures.push_back(error); });
    }

    auto sample() const -> TelemetrySample
    {
        auto value = TelemetryAccess::sample(*service);
        REQUIRE(value);
        return *value;
    }

    jb::core::priv::FakeEventLoop          loop{jb::core::priv::make_fake_event_loop()};
    jb::core::priv::ScopedCurrentEventLoop current{loop.loop.get()};
    FakeTimeSource                         time;
    SequenceUuidGenerator                  generator{{epoch}};
    Object                                 receiver;
    std::vector<Error>                     failures;
    std::unique_ptr<ExecutionTelemetry>    service;
};

auto clock_failure() -> TelemetryFailure
{
    return {
        .error = {.category = ErrorCategory::Internal,
                  .code     = "jobu.telemetry.clock_regression",
                  .message  = "private-backend-marker",
                  .detail   = "private-backend-marker"}
    };
}

} // namespace

TEST_CASE("Telemetry activation validates copied options without SQL or signals", "[jobu][telemetry][service]")
{
    auto             invalid = GENERATE(0, 1, 2, 3, 4, 5);
    TelemetryFixture f;
    TelemetryOptions options;
    switch (invalid) {
        case 0:
            options.checkpoint_interval = 0s;
            break;
        case 1:
            options.checkpoint_interval = -1s;
            break;
        case 2:
            options.checkpoint_interval = 86401s;
            break;
        case 3:
            options.checkpoint_interval = std::chrono::seconds::max();
            break;
        case 4:
            options.batch_size = 0;
            break;
        case 5:
            options.batch_size = 1001;
            break;
        default:
            FAIL("Unexpected invalid option");
    }
    f.create(options);
    options = {}; // Construction copied the invalid values.
    f.faults->calls.clear();
    auto started = f.service->start();
    REQUIRE_FALSE(started);
    CHECK(started.error().category == ErrorCategory::InvalidArgument);
    CHECK(started.error().code == "jobu.telemetry.invalid_options");
    CHECK(f.faults->calls.empty());
    CHECK(f.failures.empty());
    CHECK(LoopAccess::active_timer_count(*f.loop.loop) == 0);
}

TEST_CASE("Telemetry activation is idempotent and samples without observing rows", "[jobu][telemetry][service]")
{
    TelemetryFixture f;
    f.create({.checkpoint_interval = 86400s, .batch_size = 1000});
    auto before = storage_snapshot(f.storage.database);
    f.faults->calls.clear();
    REQUIRE(f.service->start());
    REQUIRE(f.service->start()); // The finite generator contains only one epoch.
    auto sample = f.sample();
    CHECK(sample->epoch == f.epoch);
    CHECK(sample->tick_us == 0);
    CHECK(sample->utc_now == UtcTimePoint{10s});
    CHECK(f.faults->calls.empty());
    CHECK(f.failures.empty());
    CHECK(LoopAccess::active_timer_count(*f.loop.loop) == 0);
    CHECK(storage_snapshot(f.storage.database) == before);
}

TEST_CASE("Telemetry rejects missing event-loop and wrong-thread activation without SQL", "[jobu][telemetry][service]")
{
    TelemetryFixture f;
    SECTION("no event loop at construction")
    {
        jb::core::priv::ScopedCurrentEventLoop no_loop{nullptr};
        f.create();
    }
    SECTION("activation from another thread")
    {
        f.create();
        std::optional<Error> error;
        std::thread          worker{[&] {
            auto result = f.service->start();
            if (!result) {
                error = result.error();
            }
        }};
        worker.join();
        REQUIRE(error);
        CHECK(error->code == "jobu.telemetry.invalid_state");
        CHECK(f.faults->calls.empty());
        return;
    }
    auto started = f.service->start();
    REQUIRE_FALSE(started);
    CHECK(started.error().code == "jobu.telemetry.invalid_state");
    CHECK(f.faults->calls.empty());
}

TEST_CASE("Invalid epoch generation never creates an active telemetry owner", "[jobu][telemetry][service]")
{
    auto             nil = GENERATE(false, true);
    TelemetryFixture f;
    f.generator = SequenceUuidGenerator{nil ? std::vector<Uuid>{Uuid{}} : std::vector<Uuid>{}};
    f.create();
    auto started = f.service->start();
    REQUIRE_FALSE(started);
    CHECK(started.error().code == (nil ? "jobu.telemetry.invalid_state" : "test.uuid.sequence_exhausted"));
    REQUIRE_FALSE(TelemetryAccess::sample(*f.service));
    CHECK(f.faults->calls.empty());
    CHECK(f.failures.empty());
}

TEST_CASE("Samples fence earlier boundaries, foreign owners and forged values before SQL", "[jobu][telemetry][service]")
{
    TelemetryFixture f;
    f.create();
    REQUIRE(f.service->start());
    auto old     = f.sample();
    auto current = f.sample(); // Equal ticks still represent distinct logical boundaries.
    CHECK(old->tick_us == current->tick_us);
    CHECK(old != current);
    ExecutionTelemetry    other{f.storage.database, f.storage.registry, f.time, f.generator};
    SequenceUuidGenerator other_ids{{recovery_id(101)}};
    ExecutionTelemetry    active_other{f.storage.database, f.storage.registry, f.time, other_ids};
    REQUIRE(active_other.start());
    auto foreign = TelemetryAccess::sample(active_other);
    REQUIRE(foreign);

    auto forged = std::make_shared<WaitSample const>(*current);
    for (auto const& invalid : {old, *foreign, forged, TelemetrySample{}}) {
        auto result = TelemetryAccess::open_interval(*f.service, f.run_id, invalid);
        REQUIRE_FALSE(result);
        CHECK(result.error().error.code == "jobu.telemetry.invalid_state");
        CHECK(result.error().origin == StorageFailureOrigin::Operation);
    }
    REQUIRE_FALSE(TelemetryAccess::initial_measurement(other));
    CHECK(f.faults->calls.empty());
    CHECK(f.failures.empty());

    auto transaction = jb::db::Transaction::begin(f.storage.database);
    REQUIRE(transaction);
    REQUIRE(TelemetryAccess::open_interval(*f.service, f.run_id, current));
    REQUIRE(TelemetryAccess::settle(*f.service, f.run_id, current));
    REQUIRE(TelemetryAccess::open_interval(*f.service, f.run_id, current));
    REQUIRE(transaction->commit());
    CHECK(f.repository.read(f.run_id)->quality == WaitQuality::Partial);
}

TEST_CASE("Active creation selects Complete while late instrumentation remains Partial", "[jobu][telemetry][service]")
{
    TelemetryFixture f;
    f.create();
    REQUIRE(f.service->start());
    auto initial = TelemetryAccess::initial_measurement(*f.service);
    REQUIRE(initial);
    CHECK(*initial == InitialRunMeasurement::Complete);
    auto job        = f.storage.make_job(f.job_id, f.queue_id);
    auto run        = f.storage.make_run(recovery_id(4), job, RunState::Scheduled, 0, RunOrigin::Manual).run;
    run.planned_at  = UtcTimePoint{100h};
    run.runnable_at = run.planned_at;

    RunRepository runs{f.storage.database, f.storage.registry};
    auto          transaction = jb::db::Transaction::begin(f.storage.database);
    REQUIRE(transaction);
    REQUIRE(runs.insert_manual(run, *initial));
    auto birth = f.repository.read(run.id);
    REQUIRE(birth);
    CHECK(birth->quality == WaitQuality::Complete);
    CHECK(birth->runnable_wait_us == 0);
    CHECK_FALSE(birth->open_epoch); // Future-dated from-birth coverage need not open an interval.

    auto sample = f.sample();
    REQUIRE(TelemetryAccess::open_interval(*f.service, f.run_id, sample));
    REQUIRE(transaction->commit());
    CHECK(f.repository.read(f.run_id)->quality == WaitQuality::Partial);
}

TEST_CASE("Owner samples ignore wall-clock jumps and count only monotonic intervals", "[jobu][telemetry][service]")
{
    TelemetryFixture f;
    f.seed("complete");
    f.create();
    REQUIRE(f.service->start());
    auto transaction = jb::db::Transaction::begin(f.storage.database);
    REQUIRE(transaction);
    REQUIRE(TelemetryAccess::open_interval(*f.service, f.run_id, f.sample()));
    f.time.advance(17us);
    f.time.set_utc(UtcTimePoint{-100h});
    REQUIRE(TelemetryAccess::rebase(*f.service, f.run_id, f.sample()));
    f.time.advance(13us);
    f.time.set_utc(UtcTimePoint{100h});
    auto settled = TelemetryAccess::settle(*f.service, f.run_id, f.sample());
    REQUIRE(settled);
    CHECK(settled->runnable_wait_us == 30);
    CHECK(settled->quality == WaitQuality::Complete);
    REQUIRE(transaction->commit());
}

TEST_CASE("Monotonic regression is detected before rounding and invalidates the previous sample",
          "[jobu][telemetry][service]")
{
    TelemetryFixture f;
    f.create();
    REQUIRE(f.service->start());
    f.time.set_monotonic(TimePoint{100s} + Duration{2});
    auto previous = f.sample();
    f.time.set_monotonic(TimePoint{100s} + Duration{1});
    auto failed = TelemetryAccess::sample(*f.service);
    REQUIRE_FALSE(failed);
    CHECK(failed.error().error.code == "jobu.telemetry.clock_regression");
    REQUIRE_FALSE(TelemetryAccess::open_interval(*f.service, f.run_id, previous));
    CHECK(f.faults->calls.empty());
    CHECK(f.failures.empty());
    TelemetryAccess::report_failure(*f.service, failed.error(), StorageOperation::Dispatch);
    REQUIRE(f.failures.size() == 1);
    CHECK(f.failures.front().code == "jobu.telemetry.clock_regression");
    CHECK(f.failures.front().detail == "operation=dispatch reason=operation_failed");
    REQUIRE_FALSE(f.service->start());
}

TEST_CASE("Elapsed conversion checks native subtraction limits and accepts representable extremes",
          "[jobu][telemetry][service]")
{
    TelemetryFixture f;
    SECTION("subtraction would overflow before conversion")
    {
        f.time.set_monotonic(TimePoint::min());
        f.create();
        REQUIRE(f.service->start());
        f.time.set_monotonic(TimePoint::max());
        auto sample = TelemetryAccess::sample(*f.service);
        REQUIRE_FALSE(sample);
        CHECK(sample.error().error.code == "jobu.telemetry.counter_overflow");
    }
    SECTION("largest native nonnegative duration")
    {
        f.time.set_monotonic(TimePoint{});
        f.create();
        REQUIRE(f.service->start());
        f.time.set_monotonic(TimePoint::max());
        auto sample = f.sample();
        CHECK(sample->tick_us == std::chrono::duration_cast<std::chrono::microseconds>(Duration::max()).count());
    }
    SECTION("negative origin with a valid crossing")
    {
        f.time.set_monotonic(TimePoint{-5s});
        f.create();
        REQUIRE(f.service->start());
        f.time.set_monotonic(TimePoint{5s});
        CHECK(f.sample()->tick_us == 10'000'000);
    }
    CHECK(f.faults->calls.empty());
    CHECK(f.failures.empty());
}

TEST_CASE("Stopping invalidates all accounting without SQL or persistence", "[jobu][telemetry][service]")
{
    TelemetryFixture f;
    f.create();
    REQUIRE(f.service->start());
    auto sample = f.sample();
    {
        auto transaction = jb::db::Transaction::begin(f.storage.database);
        REQUIRE(transaction);
        REQUIRE(TelemetryAccess::open_interval(*f.service, f.run_id, sample));
        REQUIRE(transaction->commit());
    }
    auto before = storage_snapshot(f.storage.database);
    f.faults->calls.clear();
    f.service->request_stop();
    f.service->request_stop();
    REQUIRE_FALSE(TelemetryAccess::sample(*f.service));
    REQUIRE_FALSE(TelemetryAccess::initial_measurement(*f.service));
    REQUIRE_FALSE(TelemetryAccess::settle(*f.service, f.run_id, sample));
    REQUIRE_FALSE(f.service->start());
    f.service.reset();
    CHECK(f.faults->calls.empty());
    CHECK(f.failures.empty());
    CHECK(storage_snapshot(f.storage.database) == before);
    CHECK(sample->epoch == f.epoch); // Samples own values, not the destroyed Private block.
}

TEST_CASE("Accounting errors remain silent until rollback and the explicit failure boundary",
          "[jobu][telemetry][service][fault]")
{
    TelemetryFixture f;
    f.seed("complete");
    f.create();
    REQUIRE(f.service->start());
    auto before      = storage_snapshot(f.storage.database);
    f.faults->faults = {
        {.at    = {.boundary  = "timing.write",
                   .operation = DatabaseOperation::Execute,
                   .phase     = DatabaseFaultPhase::AfterSuccess},
         .error = fault_error()}
    };
    std::optional<TelemetryFailure> failure;
    {
        auto transaction = jb::db::Transaction::begin(f.storage.database);
        REQUIRE(transaction);
        auto result = TelemetryAccess::open_interval(*f.service, f.run_id, f.sample());
        REQUIRE_FALSE(result);
        failure = result.error();
        CHECK(f.failures.empty());
        REQUIRE(transaction->rollback());
    }
    CHECK(storage_snapshot(f.storage.database) == before);
    f.service->failed.connect(&f.receiver, [&](Error const& error) {
        check_safe_error(error, "db.io");
        f.storage.reopen(); // This would fail if any query or transaction guard survived.
        f.service->request_stop();
    });
    TelemetryAccess::report_failure(*f.service, *failure, StorageOperation::Completion);
    REQUIRE(f.failures.size() == 1);
    CHECK(f.failures.front().detail == "operation=completion reason=state_operation_failed");
    f.faults->calls.clear();
    TelemetryAccess::report_failure(*f.service, clock_failure(), StorageOperation::Mutation);
    REQUIRE_FALSE(TelemetryAccess::sample(*f.service));
    auto restarted = f.service->start();
    REQUIRE_FALSE(restarted);
    CHECK(restarted.error().code == "db.io");
    CHECK(f.failures.size() == 1);
    CHECK(f.faults->calls.empty());
}

TEST_CASE("Failure arguments survive synchronous owner destruction", "[jobu][telemetry][service]")
{
    TelemetryFixture f;
    f.create();
    REQUIRE(f.service->start());
    auto sample = f.sample();
    f.service->failed.connect(&f.receiver, [&](Error const& error) {
        f.service.reset();
        check_safe_error(error, "jobu.telemetry.clock_regression");
        f.storage.reopen();
    });
    TelemetryAccess::report_failure(*f.service, clock_failure(), StorageOperation::Dispatch);
    CHECK_FALSE(f.service);
    REQUIRE(f.failures.size() == 1);
    CHECK(sample->epoch == f.epoch);
}

TEST_CASE("Queued failure values own their data and respect receiver destruction", "[jobu][telemetry][service]")
{
    auto             destroy_receiver = GENERATE(false, true);
    TelemetryFixture f;
    f.create();
    REQUIRE(f.service->start());
    auto               receiver = std::make_unique<Object>();
    std::vector<Error> queued;
    f.service->failed.connect(
        receiver.get(),
        [&](Error const& error) { queued.push_back(error); },
        ConnectionType::Queued);
    TelemetryAccess::report_failure(*f.service, clock_failure(), StorageOperation::Mutation);
    f.service->request_stop();
    if (destroy_receiver) {
        receiver.reset();
    }
    REQUIRE(f.loop.loop->process_events(EventFlag::Events) != ProcessEventsResult::Failed);
    CHECK(queued.size() == (destroy_receiver ? 0 : 1));
    if (!destroy_receiver) {
        check_safe_error(queued.front(), "jobu.telemetry.clock_regression");
    }
}

TEST_CASE("Original accounting failure retains origin when rollback poisons the connection",
          "[jobu][telemetry][service][fault]")
{
    TelemetryFixture f;
    f.seed("complete");
    f.seed_open(recovery_id(101), 0);
    f.create();
    REQUIRE(f.service->start());
    f.faults->faults = {
        {.at    = {.boundary = "connection", .operation = DatabaseOperation::Rollback},
         .error = fault_error("db.rollback_failed")}
    };
    std::optional<TelemetryFailure> failure;
    {
        auto transaction = jb::db::Transaction::begin(f.storage.database);
        REQUIRE(transaction);
        auto result = TelemetryAccess::settle(*f.service, f.run_id, f.sample());
        REQUIRE_FALSE(result);
        failure = result.error();
        CHECK(failure->origin == StorageFailureOrigin::PersistedData);
        REQUIRE_FALSE(transaction->rollback());
    }
    REQUIRE(f.storage.database.is_poisoned());
    TelemetryAccess::report_failure(*f.service, *failure, StorageOperation::Mutation);
    REQUIRE(f.failures.size() == 1);
    CHECK(f.failures.front().code == "jobu.telemetry.invalid_state");
    CHECK(f.failures.front().detail == "operation=mutation reason=durable_invariant");
    f.faults->calls.clear();
    REQUIRE_FALSE(TelemetryAccess::sample(*f.service));
    CHECK(f.faults->calls.empty());
}

TEST_CASE("Uncertain caller commit is fatal and never causes accounting retry", "[jobu][telemetry][service][fault]")
{
    TelemetryFixture f;
    f.create();
    REQUIRE(f.service->start());
    f.faults->faults = {
        {.at    = {.boundary  = "connection",
                   .operation = DatabaseOperation::Commit,
                   .phase     = DatabaseFaultPhase::AfterSuccess},
         .error = fault_error()}
    };
    std::optional<TelemetryFailure> failure;
    {
        auto transaction = jb::db::Transaction::begin(f.storage.database);
        REQUIRE(transaction);
        REQUIRE(TelemetryAccess::open_interval(*f.service, f.run_id, f.sample()));
        auto committed = transaction->commit();
        REQUIRE_FALSE(committed);
        failure = TelemetryFailure{.error = committed.error()};
        REQUIRE_FALSE(transaction->rollback());
    }
    TelemetryAccess::report_failure(*f.service, *failure, StorageOperation::Dispatch);
    REQUIRE(f.failures.size() == 1);
    check_safe_error(f.failures.front(), "db.io");
    f.faults->calls.clear();
    REQUIRE_FALSE(TelemetryAccess::sample(*f.service));
    CHECK(f.faults->calls.empty());
    f.storage.reopen();
    auto durable = f.repository.read(f.run_id);
    REQUIRE(durable);
    CHECK(durable->open_epoch == f.epoch);
    CHECK(durable->quality == WaitQuality::Partial);
}

TEST_CASE("Transaction helpers reuse one clock sample across accounting operations", "[jobu][telemetry][service]")
{
    struct CountingClock final : TimeSource {
        explicit CountingClock(TimeSource& source_value)
            : source{source_value}
        {}

        auto utc_now() const noexcept -> UtcTimePoint override
        {
            ++utc_calls;
            return source.utc_now();
        }

        auto monotonic_now() const noexcept -> TimePoint override
        {
            ++monotonic_calls;
            return source.monotonic_now();
        }

        TimeSource&         source;
        mutable std::size_t utc_calls{0};
        mutable std::size_t monotonic_calls{0};
    };

    TelemetryFixture   f;
    CountingClock      clock{f.time};
    ExecutionTelemetry service{f.storage.database, f.storage.registry, clock, f.generator};
    REQUIRE(service.start());
    CHECK(clock.utc_calls == 0);
    CHECK(clock.monotonic_calls == 1);
    auto sample = TelemetryAccess::sample(service);
    REQUIRE(sample);
    CHECK(clock.utc_calls == 1);
    CHECK(clock.monotonic_calls == 2);

    auto transaction = jb::db::Transaction::begin(f.storage.database);
    REQUIRE(transaction);
    REQUIRE(TelemetryAccess::open_interval(service, f.run_id, *sample));
    f.time.advance(1h); // Work later in the same boundary must not acquire a different tick.
    auto settled = TelemetryAccess::settle(service, f.run_id, *sample);
    REQUIRE(settled);
    CHECK(settled->runnable_wait_us == 0);
    REQUIRE(TelemetryAccess::open_interval(service, f.run_id, *sample));
    REQUIRE(TelemetryAccess::initial_measurement(service));
    CHECK(clock.utc_calls == 1);
    CHECK(clock.monotonic_calls == 2);
    REQUIRE(transaction->commit());
}

TEST_CASE("Poisoned connections reject accounting before any SQL or signal", "[jobu][telemetry][service][fault]")
{
    TelemetryFixture f;
    f.create();
    REQUIRE(f.service->start());
    auto sample = f.sample();
    {
        auto transaction = jb::db::Transaction::begin(f.storage.database);
        REQUIRE(transaction);
        f.faults->faults = {
            {.at    = {.boundary = "connection", .operation = DatabaseOperation::Rollback},
             .error = fault_error("db.rollback_failed")}
        };
        REQUIRE_FALSE(transaction->rollback());
    }
    REQUIRE(f.storage.database.is_poisoned());
    f.faults->calls.clear();
    auto rejected = TelemetryAccess::open_interval(*f.service, f.run_id, sample);
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().error.code == "db.rollback_failed");
    REQUIRE_FALSE(TelemetryAccess::sample(*f.service));
    REQUIRE_FALSE(TelemetryAccess::initial_measurement(*f.service));
    auto started = f.service->start();
    REQUIRE_FALSE(started);
    check_safe_error(started.error(), "db.rollback_failed");
    CHECK(f.failures.empty());
    CHECK(f.faults->calls.empty());
    TelemetryAccess::report_failure(*f.service, rejected.error(), StorageOperation::Completion);
    REQUIRE(f.failures.size() == 1);
    check_safe_error(f.failures.front(), "db.rollback_failed");
}
