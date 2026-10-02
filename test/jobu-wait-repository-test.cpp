#include "wait_repository_priv.hpp"

#include "query.hpp"
#include "support/storage_fault_helpers.hpp"
#include "support/telemetry_fixture.hpp"
#include "transaction.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <string_view>

using namespace jb::core;
using namespace jb::jobu;
using namespace jb::jobu::detail;
using namespace jb::test;
using namespace std::chrono_literals;

namespace {

auto boundary(TelemetryStorageFixture const& fixture, std::int64_t tick, UtcTimePoint utc = {}) -> WaitSample
{
    return {.utc_now = utc, .epoch = fixture.epoch, .tick_us = tick};
}

void require_no_transaction_calls(DatabaseFaultState const& faults)
{
    CHECK(std::ranges::none_of(faults.calls, [](DatabaseCall const& call) {
        return call.operation == DatabaseOperation::Begin || call.operation == DatabaseOperation::Commit ||
               call.operation == DatabaseOperation::Rollback;
    }));
}

} // namespace

TEST_CASE("Warnings claim strictly above policy without checkpointing the open tail", "[jobu][telemetry][warning]")
{
    TelemetryStorageFixture f;
    f.seed("complete", 3000);
    f.seed_open(f.epoch, 100);
    auto transaction = jb::db::Transaction::begin(f.storage.database);
    REQUIRE(transaction);
    auto equality = f.repository.claim_warning(f.run_id, boundary(f, 7100), 10ms);
    REQUIRE(equality);
    CHECK(equality->known_wait == 10000us);
    CHECK(equality->until_warning == 1us);
    CHECK_FALSE(equality->claimed);
    auto crossed = f.repository.claim_warning(f.run_id, boundary(f, 7101), 10ms);
    REQUIRE(crossed);
    CHECK(crossed->known_wait == 10001us);
    CHECK(crossed->claimed);
    auto repeat = f.repository.claim_warning(f.run_id, boundary(f, 7102), 1ms);
    REQUIRE(repeat);
    CHECK_FALSE(repeat->claimed);
    CHECK_FALSE(repeat->until_warning);
    CHECK(f.repository.read(f.run_id)->runnable_wait_us == 3000);
    CHECK(f.repository.read(f.run_id)->open_tick_us == 100);
    REQUIRE(transaction->rollback());
    CHECK_FALSE(f.repository.read(f.run_id)->delay_warned);
}

TEST_CASE("Disabled or unreachable warning thresholds preserve measurement", "[jobu][telemetry][warning]")
{
    auto threshold = GENERATE(0ms, std::chrono::milliseconds{std::numeric_limits<std::int64_t>::max()});
    TelemetryStorageFixture f;
    f.seed("partial", std::numeric_limits<std::int64_t>::max() - 1);
    f.seed_open(f.epoch, 0);
    auto transaction = jb::db::Transaction::begin(f.storage.database);
    REQUIRE(transaction);
    auto result = f.repository.claim_warning(f.run_id, boundary(f, 1), threshold);
    REQUIRE(result);
    CHECK(result->known_wait.count() == std::numeric_limits<std::int64_t>::max());
    CHECK_FALSE(result->claimed);
    CHECK_FALSE(result->until_warning);
    auto overflow = f.repository.claim_warning(f.run_id, boundary(f, 2), threshold);
    REQUIRE_FALSE(overflow);
    CHECK(overflow.error().error.code == "jobu.telemetry.counter_overflow");
    REQUIRE(transaction->rollback());
}

TEST_CASE("Wait intervals add, rebase and close without changing quality or warning ownership", "[jobu][telemetry]")
{
    auto const*             quality = GENERATE("unmeasured", "complete", "partial");
    auto                    state   = GENERATE(RunState::Scheduled, RunState::RetryWait);
    TelemetryStorageFixture f{state};
    f.seed(quality, 0, std::string_view{quality} != "unmeasured");
    auto expected_quality = std::string_view{quality} == "complete" ? WaitQuality::Complete : WaitQuality::Partial;
    auto transaction      = jb::db::Transaction::begin(f.storage.database);
    REQUIRE(transaction);
    f.faults->calls.clear();

    auto opened = f.repository.open_interval(f.run_id, boundary(f, 10));
    REQUIRE(opened);
    CHECK(opened->quality == expected_quality);
    CHECK(opened->runnable_wait_us == 0);
    CHECK(opened->open_tick_us == 10);
    REQUIRE(f.repository.open_interval(f.run_id, boundary(f, 20)));
    CHECK(f.repository.read(f.run_id)->open_tick_us == 10);

    // UTC jumps do not enter the delta, and repeated observations do not double count.
    auto rebased = f.repository.rebase(f.run_id, boundary(f, 50, UtcTimePoint{100h}));
    REQUIRE(rebased);
    CHECK(rebased->runnable_wait_us == 40);
    CHECK(rebased->open_tick_us == 50);
    REQUIRE(f.repository.rebase(f.run_id, boundary(f, 50)));
    rebased = f.repository.rebase(f.run_id, boundary(f, 70, UtcTimePoint{-100h}));
    REQUIRE(rebased);
    CHECK(rebased->runnable_wait_us == 60);

    auto closed = f.repository.settle(f.run_id, boundary(f, 90));
    REQUIRE(closed);
    CHECK(closed->runnable_wait_us == 80);
    CHECK_FALSE(closed->open_epoch);
    CHECK_FALSE(closed->open_tick_us);
    REQUIRE(f.repository.settle(f.run_id, boundary(f, 100)));
    REQUIRE(f.repository.rebase(f.run_id, boundary(f, 100)));

    REQUIRE(f.repository.open_interval(f.run_id, boundary(f, 110)));
    closed = f.repository.settle(f.run_id, boundary(f, 130));
    REQUIRE(closed);
    CHECK(closed->runnable_wait_us == 100);
    CHECK(closed->quality == expected_quality);
    CHECK(closed->delay_warned == (std::string_view{quality} != "unmeasured"));
    require_no_transaction_calls(*f.faults);
    REQUIRE(transaction->commit());
    f.storage.reopen(); // No query survives any repository call.
    CHECK(f.repository.read(f.run_id)->runnable_wait_us == 100);
}

TEST_CASE("Closed unmeasured rows remain unmeasured until observation", "[jobu][telemetry]")
{
    TelemetryStorageFixture f;
    auto                    transaction = jb::db::Transaction::begin(f.storage.database);
    REQUIRE(transaction);
    REQUIRE(f.repository.settle(f.run_id, boundary(f, 100)));
    auto row = f.repository.rebase(f.run_id, boundary(f, 200));
    REQUIRE(row);
    CHECK(row->quality == WaitQuality::Unmeasured);
    CHECK(row->runnable_wait_us == 0);
    CHECK_FALSE(row->open_epoch);
    REQUIRE(transaction->commit());
}

TEST_CASE("Only pending runs can open an interval", "[jobu][telemetry]")
{
    auto state =
        GENERATE(RunState::Running, RunState::Succeeded, RunState::Failed, RunState::Interrupted, RunState::Cancelled);
    TelemetryStorageFixture f{state};
    f.seed("complete", 23);
    auto before      = storage_snapshot(f.storage.database);
    auto transaction = jb::db::Transaction::begin(f.storage.database);
    REQUIRE(transaction);
    REQUIRE(f.repository.settle(f.run_id, boundary(f, 10)));
    auto opened = f.repository.open_interval(f.run_id, boundary(f, 10));
    REQUIRE_FALSE(opened);
    CHECK(opened.error().error.code == "jobu.telemetry.invalid_state");
    CHECK(opened.error().origin == StorageFailureOrigin::PersistedData);
    REQUIRE(transaction->rollback());
    CHECK(storage_snapshot(f.storage.database) == before);

    // The schema cannot express a cross-table run-state constraint; the repository must check it.
    f.seed_open(f.epoch, 10);
    auto invalid = f.repository.read(f.run_id);
    REQUIRE_FALSE(invalid);
    CHECK(invalid.error().origin == StorageFailureOrigin::PersistedData);
}

TEST_CASE("Foreign epochs and regressing ticks never alter a persisted tail", "[jobu][telemetry]")
{
    auto const*             operation = GENERATE("open", "settle", "rebase");
    auto const*             fault     = GENERATE("foreign", "regression", "negative", "nil");
    TelemetryStorageFixture f;
    f.seed("partial", 19, true);
    f.seed_open(f.epoch, 100);
    auto sample = boundary(f, 200);
    if (std::string_view{fault} == "foreign") {
        sample.epoch = recovery_id(101);
    }
    else if (std::string_view{fault} == "regression") {
        sample.tick_us = 99;
    }
    else if (std::string_view{fault} == "negative") {
        sample.tick_us = -1;
    }
    else {
        sample.epoch = Uuid{};
    }
    auto before      = storage_snapshot(f.storage.database);
    auto transaction = jb::db::Transaction::begin(f.storage.database);
    REQUIRE(transaction);
    auto method = &WaitRepository::open_interval;
    if (std::string_view{operation} == "settle") {
        method = &WaitRepository::settle;
    }
    else if (std::string_view{operation} == "rebase") {
        method = &WaitRepository::rebase;
    }
    auto result = (f.repository.*method)(f.run_id, sample);
    REQUIRE_FALSE(result);
    auto invalid_epoch = std::string_view{fault} == "foreign" || std::string_view{fault} == "nil";
    CHECK(result.error().error.code ==
          (invalid_epoch ? "jobu.telemetry.invalid_state" : "jobu.telemetry.clock_regression"));
    CHECK(result.error().origin == (std::string_view{fault} == "foreign" ? StorageFailureOrigin::PersistedData
                                                                         : StorageFailureOrigin::Operation));
    REQUIRE(transaction->rollback());
    CHECK(storage_snapshot(f.storage.database) == before);
}

TEST_CASE("Wait addition accepts the durable maximum and rejects overflow", "[jobu][telemetry]")
{
    auto                    close = GENERATE(false, true);
    TelemetryStorageFixture f;
    f.seed("complete", std::numeric_limits<std::int64_t>::max() - 1);
    f.seed_open(f.epoch, 10);
    auto transaction = jb::db::Transaction::begin(f.storage.database);
    REQUIRE(transaction);
    auto maximum = f.repository.rebase(f.run_id, boundary(f, 11));
    REQUIRE(maximum);
    CHECK(maximum->runnable_wait_us == std::numeric_limits<std::int64_t>::max());
    REQUIRE(f.repository.rebase(f.run_id, boundary(f, 11)));
    auto overflow =
        close ? f.repository.settle(f.run_id, boundary(f, 12)) : f.repository.rebase(f.run_id, boundary(f, 12));
    REQUIRE_FALSE(overflow);
    CHECK(overflow.error().error.code == "jobu.telemetry.counter_overflow");
    CHECK(f.repository.read(f.run_id)->open_tick_us == 11);
    REQUIRE(transaction->rollback());
    CHECK(f.repository.read(f.run_id)->runnable_wait_us == std::numeric_limits<std::int64_t>::max() - 1);
}

TEST_CASE("Wait decoding rejects missing and malformed durable rows", "[jobu][telemetry]")
{
    auto const* corruption = GENERATE(
        "DELETE FROM jobu_run_timing",
        "UPDATE jobu_run_timing SET runnable_wait_us = -1",
        "UPDATE jobu_run_timing SET runnable_wait_us = 'bad'",
        "UPDATE jobu_run_timing SET measurement_status = 'unknown'",
        "UPDATE jobu_run_timing SET delay_warned = 2",
        "UPDATE jobu_run_timing SET measurement_status = 'complete', open_epoch = zeroblob(15), open_tick_us = 0",
        "UPDATE jobu_run_timing SET measurement_status = 'complete', open_tick_us = 0",
        "UPDATE jobu_run_timing SET measurement_status = 'complete', open_epoch = zeroblob(16)",
        "UPDATE jobu_run_timing SET measurement_status = 'complete', open_epoch = zeroblob(16), open_tick_us = 0",
        "UPDATE jobu_run_timing SET runnable_wait_us = 1",
        "UPDATE jobu_run_timing SET delay_warned = 1");
    TelemetryStorageFixture f;
    {
        jb::db::Query query{f.storage.database};
        REQUIRE(query.exec("PRAGMA ignore_check_constraints = ON"));
        // Deliberate corrupt input: bypass CHECKs without weakening production schema validation.
        REQUIRE(query.exec(corruption));
    }
    auto invalid = f.repository.read(f.run_id);
    REQUIRE_FALSE(invalid);
    CHECK(invalid.error().error.code == "jobu.telemetry.invalid_state");
    CHECK(invalid.error().origin == StorageFailureOrigin::PersistedData);
    f.storage.reopen();
}

TEST_CASE("Timing projection never decodes payloads or attributes", "[jobu][telemetry]")
{
    TelemetryStorageFixture f;
    {
        jb::db::Query query{f.storage.database};
        REQUIRE(query.exec("UPDATE jobu_runs SET payload_json = 'not-json', attributes_json = 'not-json'"));
    }
    auto transaction = jb::db::Transaction::begin(f.storage.database);
    REQUIRE(transaction);
    REQUIRE(f.repository.open_interval(f.run_id, boundary(f, 10)));
    auto closed = f.repository.settle(f.run_id, boundary(f, 20));
    REQUIRE(closed);
    CHECK(closed->runnable_wait_us == 10);
    REQUIRE(transaction->commit());
}

TEST_CASE("Timing and domain state roll back together and retain the old open tail", "[jobu][telemetry]")
{
    TelemetryStorageFixture f;
    f.seed("complete");
    f.seed_open(f.epoch, 10);
    auto before = storage_snapshot(f.storage.database);
    {
        auto transaction = jb::db::Transaction::begin(f.storage.database);
        REQUIRE(transaction);
        REQUIRE(f.repository.settle(f.run_id, boundary(f, 20)));
        jb::db::Query query{f.storage.database};
        REQUIRE(query.exec("UPDATE jobu_queues SET state = 'suspended'"));
        REQUIRE(query.finish());
        REQUIRE(transaction->rollback());
    }
    CHECK(storage_snapshot(f.storage.database) == before);

    auto transaction = jb::db::Transaction::begin(f.storage.database);
    REQUIRE(transaction);
    auto settled = f.repository.settle(f.run_id, boundary(f, 50));
    REQUIRE(settled);
    CHECK(settled->runnable_wait_us == 40);
    REQUIRE(transaction->commit());
}

TEST_CASE("Repository faults leave transaction cleanup to the caller", "[jobu][telemetry][fault]")
{
    auto                    operation = GENERATE(DatabaseOperation::Prepare,
                                                 DatabaseOperation::Bind,
                                                 DatabaseOperation::Execute,
                                                 DatabaseOperation::Finish);
    TelemetryStorageFixture f;
    f.seed("complete");
    auto before      = storage_snapshot(f.storage.database);
    f.faults->faults = {
        {
         .at    = {.boundary = "timing.write", .operation = operation, .phase = DatabaseFaultPhase::AfterSuccess},
         .error = fault_error(),
         },
    };
    auto transaction = jb::db::Transaction::begin(f.storage.database);
    REQUIRE(transaction);
    f.faults->calls.clear();

    auto result = f.repository.open_interval(f.run_id, boundary(f, 10));
    REQUIRE_FALSE(result);
    CHECK(result.error().error.code == "db.io");
    CHECK(result.error().origin == StorageFailureOrigin::Operation);
    require_no_transaction_calls(*f.faults);
    require_consumed_faults(*f.faults);
    REQUIRE(transaction->rollback());
    CHECK(storage_snapshot(f.storage.database) == before);
}

TEST_CASE("Timing read errors preserve backend codes and release query state", "[jobu][telemetry][fault]")
{
    auto                    operation = GENERATE(DatabaseOperation::Prepare,
                                                 DatabaseOperation::Bind,
                                                 DatabaseOperation::Execute,
                                                 DatabaseOperation::Fetch,
                                                 DatabaseOperation::Finish);
    TelemetryStorageFixture f;
    auto                    before = storage_snapshot(f.storage.database);
    f.faults->faults               = {
        {.at = {.boundary = "timing.read", .operation = operation}, .error = fault_error()},
    };
    auto failed = f.repository.read(f.run_id);
    REQUIRE_FALSE(failed);
    CHECK(failed.error().error.code == "db.io");
    CHECK(failed.error().origin == StorageFailureOrigin::Operation);
    require_consumed_faults(*f.faults);
    CHECK(storage_snapshot(f.storage.database) == before);
    f.storage.reopen();
}
