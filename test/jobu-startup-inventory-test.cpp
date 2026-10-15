#include "startup_inventory_priv.hpp"

#include "database.hpp"
#include "record.hpp"
#include "support/fake_database_driver.hpp"
#include "support/recovery_fixture.hpp"
#include "support/storage_fault_helpers.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
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

auto count_row(std::string state, jb::db::Value count) -> jb::db::Record
{
    return jb::db::Record{
        {{"inventory_state", std::move(state)}, {"inventory_count", std::move(count)}}
    };
}

struct CountFixture {
    CountFixture()
    {
        for (int i = 0; i < 3; ++i) {
            state->query_plans.push_back({.execution_info = {.produces_records = true}});
        }
        REQUIRE(database.open());
    }

    std::shared_ptr<FakeDatabaseDriverState> state{std::make_shared<FakeDatabaseDriverState>()};
    jb::db::Database                         database{std::make_unique<FakeDatabaseDriver>(state)};
};

void check_empty(StartupInventory const& inventory)
{
    CHECK(inventory.queues_active == 0);
    CHECK(inventory.queues_suspending == 0);
    CHECK(inventory.queues_suspended == 0);
    CHECK(inventory.jobs_active == 0);
    CHECK(inventory.jobs_suspending == 0);
    CHECK(inventory.jobs_suspended == 0);
    CHECK(inventory.runs_scheduled == 0);
    CHECK(inventory.runs_retry_wait == 0);
}

} // namespace

TEST_CASE("Startup inventory reads an empty database without changing storage", "[jobu][inventory]")
{
    RecoveryFixture fixture;
    auto const      before    = storage_snapshot(fixture.database);
    auto            inventory = read_startup_inventory(fixture.database);
    REQUIRE(inventory);
    check_empty(*inventory);
    CHECK(storage_snapshot(fixture.database) == before);
    fixture.reopen(); // No query or transaction may survive the snapshot call.
}

TEST_CASE("Startup inventory counts one queue and two future definitions and runs", "[jobu][inventory]")
{
    RecoveryFixture fixture;
    auto            queue = recovery_queue(recovery_id(1));
    fixture.insert_queue(queue);
    for (std::uint32_t i = 0; i < 2; ++i) {
        auto job     = fixture.make_job(recovery_id(10 + i), queue.id);
        job.schedule = OnceSchedule{UtcTimePoint{24h}};
        fixture.insert_job(job);
        auto run            = fixture.make_run(recovery_id(100 + i), job);
        run.run.planned_at  = UtcTimePoint{24h};
        run.run.runnable_at = UtcTimePoint{24h};
        fixture.insert_run(run);
    }
    auto const before    = storage_snapshot(fixture.database);
    auto       inventory = read_startup_inventory(fixture.database);
    REQUIRE(inventory);
    CHECK(inventory->queues_active == 1);
    CHECK(inventory->jobs_active == 2);
    CHECK(inventory->runs_scheduled == 2);
    CHECK(inventory->runs_retry_wait == 0);
    CHECK(storage_snapshot(fixture.database) == before);
}

TEST_CASE("Startup inventory separates live definitions from pending state and retained history", "[jobu][inventory]")
{
    RecoveryFixture fixture;
    auto const queue_states = {QueueState::Active, QueueState::Suspending, QueueState::Suspended, QueueState::Deleted};
    std::uint32_t suffix    = 1;
    for (auto state : queue_states) {
        fixture.insert_queue(recovery_queue(recovery_id(suffix++), state));
    }

    // All definition states exist, while HTTP pending work remains counted without any executor capability input.
    auto const job_states = {JobState::Active,
                             JobState::Suspending,
                             JobState::Suspended,
                             JobState::Deleted,
                             JobState::Succeeded,
                             JobState::Failed,
                             JobState::Cancelled};
    suffix                = 10;
    for (auto state : job_states) {
        auto job  = fixture.make_job(recovery_id(suffix++), recovery_id(3), JobType::Http);
        job.state = state;
        if (state == JobState::Deleted) {
            job.deleted_at = UtcTimePoint{120s};
        }
        fixture.insert_job(job);
        if (state == JobState::Active || state == JobState::Suspending || state == JobState::Suspended) {
            auto run            = fixture.make_run(recovery_id(suffix + 100), job);
            run.run.runnable_at = UtcTimePoint{24h};
            fixture.insert_run(run);
        }
    }

    // A manual retry barrier and terminal history coexist with the active definition's scheduled work.
    auto owner = fixture.make_job(recovery_id(10), recovery_id(3), JobType::Http);
    fixture.insert_run(fixture.make_run(recovery_id(200), owner, RunState::RetryWait, 1, RunOrigin::Manual));
    suffix = 300;
    for (auto state : {RunState::Succeeded, RunState::Failed, RunState::Interrupted, RunState::Cancelled}) {
        fixture.insert_run(fixture.make_run(recovery_id(suffix++), owner, state, 0, RunOrigin::Manual));
    }

    auto const before    = storage_snapshot(fixture.database);
    auto       inventory = read_startup_inventory(fixture.database);
    REQUIRE(inventory);
    CHECK(inventory->queues_active == 1);
    CHECK(inventory->queues_suspending == 1);
    CHECK(inventory->queues_suspended == 1);
    CHECK(inventory->jobs_active == 1);
    CHECK(inventory->jobs_suspending == 1);
    CHECK(inventory->jobs_suspended == 1);
    CHECK(inventory->runs_scheduled == 3);
    CHECK(inventory->runs_retry_wait == 1);
    CHECK(storage_snapshot(fixture.database) == before);
}

TEST_CASE("Startup inventory rejects work still Running after recovery", "[jobu][inventory][invariant]")
{
    RecoveryFixture fixture;
    auto            queue = recovery_queue(recovery_id(1));
    fixture.insert_queue(queue);
    auto job = fixture.make_job(recovery_id(10), queue.id);
    fixture.insert_job(job);
    fixture.insert_run(fixture.make_run(recovery_id(100), job, RunState::Running));
    auto const before    = storage_snapshot(fixture.database);
    auto       inventory = read_startup_inventory(fixture.database);
    REQUIRE_FALSE(inventory);
    CHECK(inventory.error().code == "jobu.storage.inventory_invariant");
    CHECK(inventory.error().detail == "reason=running_after_recovery");
    CHECK(storage_snapshot(fixture.database) == before);
    fixture.reopen();
}

TEST_CASE("Startup inventory executes only bounded metadata projections in one transaction", "[jobu][inventory]")
{
    auto                     faults = std::make_shared<DatabaseFaultState>();
    RecoveryFixture          fixture{[&](std::unique_ptr<jb::db::Driver> driver) {
        return std::make_unique<FaultDatabaseDriver>(std::move(driver), faults);
    }};
    auto const               before = storage_snapshot(fixture.database);
    std::vector<std::string> statements;
    faults->classify = [&](std::string_view sql) {
        statements.emplace_back(sql);
        return "inventory";
    };
    faults->calls.clear();
    REQUIRE(read_startup_inventory(fixture.database));
    REQUIRE(statements.size() == 3);
    for (auto const& sql : statements) {
        CHECK(sql.starts_with("SELECT state AS inventory_state, COUNT(*) AS inventory_count FROM jobu_"));
        CHECK(sql.ends_with("GROUP BY state"));
        CHECK(sql.find("payload") == std::string::npos);
        CHECK(sql.find("output") == std::string::npos);
        CHECK(sql.find("secret") == std::string::npos);
    }
    for (auto operation : {DatabaseOperation::Begin, DatabaseOperation::Commit}) {
        CHECK(std::ranges::count(faults->calls, DatabaseCall{"connection", operation}) == 1);
    }
    CHECK(std::ranges::count(faults->calls, DatabaseCall{"inventory", DatabaseOperation::Finish}) == 3);
    faults->classify = {};
    CHECK(storage_snapshot(fixture.database) == before);
}

TEST_CASE("Startup inventory accepts exact large counts and checks combined totals", "[jobu][inventory][invariant]")
{
    auto const   overflow = GENERATE(false, true);
    CountFixture fixture;
    auto const   maximum                  = std::numeric_limits<std::int64_t>::max();
    fixture.state->query_plans[0].records = {count_row("active", maximum),
                                             count_row("suspending", maximum),
                                             count_row("suspended", std::int64_t{overflow ? 2 : 1})};
    auto inventory                        = read_startup_inventory(fixture.database);
    if (overflow) {
        REQUIRE_FALSE(inventory);
        CHECK(inventory.error().detail == "reason=count_overflow");
    }
    else {
        REQUIRE(inventory);
        CHECK(inventory->queues_active == static_cast<std::uint64_t>(maximum));
        CHECK(inventory->queues_active + inventory->queues_suspending + inventory->queues_suspended ==
              std::numeric_limits<std::uint64_t>::max());
    }
}

TEST_CASE("Startup inventory rejects malformed count rows without coercion", "[jobu][inventory][invariant]")
{
    std::string_view const     scenario = GENERATE("missing_count",
                                                   "missing_state",
                                                   "negative",
                                                   "null",
                                                   "text",
                                                   "real",
                                                   "invalid_state_type",
                                                   "unexpected_state",
                                                   "duplicate_state");
    CountFixture               fixture;
    std::vector<jb::db::Field> fields{
        {"inventory_state", std::string{"active"}}
    };
    if (std::string_view{scenario} != "missing_count") {
        jb::db::Value count{std::int64_t{1}};
        if (scenario == "negative") {
            count = std::int64_t{-1};
        }
        if (scenario == "null") {
            count = jb::db::Null{};
        }
        if (scenario == "text") {
            count = std::string{"1"};
        }
        if (scenario == "real") {
            count = 1.0;
        }
        fields.emplace_back("inventory_count", std::move(count));
    }
    if (scenario == "missing_state") {
        fields.erase(fields.begin());
    }
    if (scenario == "invalid_state_type") {
        fields[0] = {"inventory_state", std::int64_t{1}};
    }
    if (scenario == "unexpected_state") {
        fields[0] = {"inventory_state", std::string{"private-backend-marker"}};
    }
    fixture.state->query_plans[0].records.emplace_back(std::move(fields));
    if (std::string_view{scenario} == "duplicate_state") {
        fixture.state->query_plans[0].records.push_back(count_row("active", std::int64_t{2}));
    }
    auto inventory = read_startup_inventory(fixture.database);
    REQUIRE_FALSE(inventory);
    check_safe_error(inventory.error(), "jobu.storage.inventory_invariant");
    CHECK(std::ranges::count(fixture.state->calls, "driver.rollback") == 1);
    CHECK(std::ranges::count(fixture.state->calls, "driver.commit") == 0);
}

TEST_CASE("Startup inventory sanitizes storage failures and releases transaction ownership", "[jobu][inventory][fault]")
{
    std::string_view const operation = GENERATE("begin", "create", "prepare", "execute", "fetch", "finish", "commit");
    CountFixture           fixture;
    auto const             error = fault_error();
    auto& plan = fixture.state->query_plans[1]; // Later-table failures must unwind the shared snapshot as well.
    if (operation == "begin") {
        fixture.state->begin_error = error;
    }
    if (operation == "create") {
        fixture.state->create_query_error = error;
    }
    if (operation == "prepare") {
        plan.prepare_error = error;
    }
    if (operation == "execute") {
        plan.exec_error = error;
    }
    if (operation == "fetch") {
        plan.next_error = error;
    }
    if (operation == "finish") {
        plan.finish_error = error;
    }
    if (operation == "commit") {
        fixture.state->commit_error = error;
    }
    auto inventory = read_startup_inventory(fixture.database);
    REQUIRE_FALSE(inventory);
    check_safe_error(inventory.error(), "db.io");
    CHECK(std::ranges::count(fixture.state->calls, "driver.rollback") ==
          (std::string_view{operation} == "begin" ? 0 : 1));
    REQUIRE(fixture.database.close()); // Would fail if a query or guarded transaction escaped.
}
