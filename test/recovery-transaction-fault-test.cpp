#include "recovery.hpp"

#include "byte_buffer.hpp"
#include "job_repository_priv.hpp"
#include "json.hpp"
#include "queue_repository_priv.hpp"
#include "recovery_priv.hpp"
#include "run_repository_priv.hpp"
#include "support/catch_utils.hpp" // IWYU pragma: keep for Catch::StringMaker specializations
#include "support/fake_cron_engine.hpp"
#include "support/fake_time_source.hpp"
#include "support/fault_database_driver.hpp"
#include "support/recovery_fixture.hpp"
#include "support/sequence_uuid_generator.hpp"
#include "support/storage_fault_helpers.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
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

using Operation = DatabaseOperation;
using Phase     = DatabaseFaultPhase;

enum class Scenario : std::uint8_t {
    Terminal,
    Retry,
    Exhausted,
    RecurringSuspension,
    MissingSuccessor,
    JobSuspension,
    QueueSuspension
};

auto has_run(Scenario scenario) -> bool
{
    return scenario <= Scenario::RecurringSuspension;
}

auto boundary(std::string_view sql) -> std::string
{
    if (sql.starts_with("SELECT id FROM jobu_runs WHERE 1 = 1")) {
        return sql.find("AND state = :state") == std::string_view::npos ? "scan.runs" : "scan.running";
    }
    if (sql.starts_with("SELECT id FROM jobu_jobs WHERE 1 = 1")) {
        return "scan.jobs";
    }
    if (sql.starts_with("SELECT id FROM jobu_queues WHERE 1 = 1")) {
        return "scan.queues";
    }
    if (sql.starts_with("SELECT run_id, attempt_number FROM jobu_attempt_output")) {
        return "scan.output";
    }
    if (sql.starts_with("SELECT run_id, attempt_number FROM jobu_attempts")) {
        return "scan.attempts";
    }
    if (sql.starts_with("UPDATE jobu_attempts SET completed_at_us")) {
        return "repair.attempt";
    }
    if (sql.starts_with("INSERT INTO jobu_attempt_output")) {
        return "repair.output";
    }
    if (sql.starts_with("UPDATE jobu_runs SET state = 'interrupted'")) {
        return "repair.terminal";
    }
    if (sql.starts_with("UPDATE jobu_runs SET state = 'retry_wait'")) {
        return "repair.retry";
    }
    if (sql.starts_with("INSERT INTO jobu_runs")) {
        return "repair.successor";
    }
    if (sql.starts_with("UPDATE jobu_jobs SET state = :next_state")) {
        return "repair.job_suspension";
    }
    if (sql.starts_with("UPDATE jobu_queues SET state = :next_state")) {
        return "repair.queue_suspension";
    }
    return "read.context";
}

void check_report(RecoveryReport const& actual, RecoveryReport const& expected = {})
{
    CHECK(actual.interrupted_attempts == expected.interrupted_attempts);
    CHECK(actual.retrying_runs == expected.retrying_runs);
    CHECK(actual.terminal_runs == expected.terminal_runs);
    CHECK(actual.inserted_successors == expected.inserted_successors);
    CHECK(actual.suspended_jobs == expected.suspended_jobs);
    CHECK(actual.suspended_queues == expected.suspended_queues);
}

struct Fixture {
    std::shared_ptr<DatabaseFaultState> faults = std::make_shared<DatabaseFaultState>();
    RecoveryFixture                     storage{[this](std::unique_ptr<Driver> driver) {
        return std::make_unique<FaultDatabaseDriver>(std::move(driver), faults);
    }};
    FakeCronEngine                      cron;
    FakeTimeSource                      time;
    SequenceUuidGenerator               generator{
        {recovery_id(1000), recovery_id(1001), recovery_id(1002)}
    };
    Queue                             queue = recovery_queue(recovery_id(1));
    JobDefinition                     job;
    std::optional<RecoveryRunFixture> original;

    explicit Fixture(Scenario scenario = Scenario::Terminal)
    {
        faults->classify = boundary;
        time.set_utc(UtcTimePoint{120s});
        if (scenario == Scenario::Retry || scenario == Scenario::Exhausted) {
            queue.recovery_policy = RecoveryPolicy::RetryInterrupted;
        }
        if (scenario == Scenario::RecurringSuspension || scenario == Scenario::QueueSuspension) {
            queue.state = QueueState::Suspending;
        }
        storage.insert_queue(queue);
        job                                           = storage.make_job(recovery_id(2), queue.id);
        job.attributes.at("retry.initial_delay").data = Duration{5s};
        if (scenario == Scenario::RecurringSuspension || scenario == Scenario::JobSuspension) {
            job.state = JobState::Suspending;
        }
        if (scenario == Scenario::RecurringSuspension || scenario == Scenario::MissingSuccessor) {
            job.schedule = CronSchedule{.expression = "* * * * *", .timezone = "UTC"};
            cron.set_occurrences(std::get<CronSchedule>(job.schedule), {UtcTimePoint{180s}, UtcTimePoint{300s}});
        }
        storage.insert_job(job);
        if (has_run(scenario)) {
            original =
                storage.make_run(recovery_id(3), job, RunState::Running, scenario == Scenario::Exhausted ? 2 : 0);
            storage.insert_run(*original);
        }
        faults->calls.clear();
    }

    auto recover(std::function<bool()> const& stop = {}) -> Result<RecoveryReport, Error>
    {
        return detail::recover_startup(storage.database,
                                       storage.registry,
                                       cron,
                                       generator,
                                       time,
                                       {.scan_batch_size = 1},
                                       stop);
    }

    void arm(DatabaseCall call, std::string code = "db.io")
    {
        faults->faults.push_back({.at = std::move(call), .error = fault_error(std::move(code))});
    }

    auto commits() const -> std::size_t
    {
        return static_cast<std::size_t>(std::ranges::count(
            faults->calls,
            DatabaseCall{.boundary = "connection", .operation = Operation::Commit, .phase = Phase::AfterSuccess}));
    }

    void check_interrupted(RecoveryRunFixture expected, bool retry)
    {
        auto result = parse_json(R"({"reason":"daemon_interrupted","outcome_unknown":true})");
        REQUIRE(result);
        auto& last                = expected.attempts.back();
        last.attempt.state        = AttemptState::Completed;
        last.attempt.outcome      = AttemptOutcome::Interrupted;
        last.attempt.completed_at = UtcTimePoint{120s};
        last.attempt.result       = *result;
        last.output               = jb::jobu::detail::AttemptOutput{.stdout_bytes = ByteBuffer{},
                                                                    .stderr_bytes = ByteBuffer{},
                                                                    .capture_lost = true};
        expected.run.state        = retry ? RunState::RetryWait : RunState::Interrupted;
        if (retry) {
            expected.run.runnable_at = UtcTimePoint{125s};
        }
        else {
            expected.run.completed_at = UtcTimePoint{120s};
            expected.run.result       = *result;
        }
        storage.require_run(expected);
    }

    void check_repaired(Scenario scenario)
    {
        if (original) {
            check_interrupted(*original, scenario == Scenario::Retry);
        }
        if (scenario == Scenario::RecurringSuspension || scenario == Scenario::MissingSuccessor) {
            RunRepository runs{storage.database, storage.registry};
            auto          successor = runs.find_schedule_owned(job.id);
            REQUIRE(successor);
            REQUIRE(*successor);
            CHECK((*successor)->state == RunState::Scheduled);
            CHECK((*successor)->planned_at == UtcTimePoint{180s});
        }
        JobRepository   jobs{storage.database, storage.registry};
        QueueRepository queues{storage.database, storage.registry};
        auto            owner  = jobs.find_by_id(job.id, false);
        auto            parent = queues.find_by_id(queue.id, false);
        REQUIRE(owner);
        REQUIRE(*owner);
        REQUIRE(parent);
        REQUIRE(*parent);
        bool const job_drained = job.state == JobState::Suspending;
        CHECK((*owner)->state == (job_drained ? JobState::Suspended : job.state));
        CHECK((*owner)->revision == job.revision + (job_drained ? 1 : 0));
        CHECK((*parent)->state == (queue.state == QueueState::Suspending ? QueueState::Suspended : queue.state));
    }

    void check_idempotent()
    {
        auto before = storage_snapshot(storage.database);
        auto again  = recover();
        REQUIRE(again);
        check_report(*again);
        CHECK(storage_snapshot(storage.database) == before);
    }
};

auto repair_faults(Scenario scenario) -> std::vector<DatabaseCall>
{
    std::vector<DatabaseCall> result{
        {.boundary = "connection", .operation = Operation::Begin },
        {.boundary = "connection", .operation = Operation::Commit}
    };
    std::vector<std::string> writes;
    if (has_run(scenario)) {
        writes = {"repair.attempt", "repair.output", scenario == Scenario::Retry ? "repair.retry" : "repair.terminal"};
    }
    if (scenario == Scenario::RecurringSuspension || scenario == Scenario::MissingSuccessor) {
        writes.emplace_back("repair.successor");
    }
    if (scenario == Scenario::RecurringSuspension || scenario == Scenario::JobSuspension) {
        writes.emplace_back("repair.job_suspension");
    }
    if (scenario == Scenario::RecurringSuspension || scenario == Scenario::QueueSuspension) {
        writes.emplace_back("repair.queue_suspension");
    }
    for (auto const& write : writes) {
        for (auto operation : {Operation::Prepare, Operation::Bind, Operation::Execute}) {
            result.push_back({.boundary = write, .operation = operation});
        }
        result.push_back({.boundary = write, .operation = Operation::Execute, .phase = Phase::AfterSuccess});
    }
    return result;
}

constexpr auto scenarios = {Scenario::Terminal,
                            Scenario::Retry,
                            Scenario::Exhausted,
                            Scenario::RecurringSuspension,
                            Scenario::MissingSuccessor,
                            Scenario::JobSuspension,
                            Scenario::QueueSuspension};

} // namespace

TEST_CASE("Recovery repair failures roll back every column and converge after reopen",
          "[jobu][recovery][fault][sqlite]")
{
    for (auto scenario : scenarios) {
        for (auto const& fault : repair_faults(scenario)) {
            DYNAMIC_SECTION(static_cast<int>(scenario)
                            << ' ' << fault.boundary << ' ' << static_cast<int>(fault.operation) << ' '
                            << static_cast<int>(fault.phase))
            {
                Fixture fixture{scenario};
                auto    before = storage_snapshot(fixture.storage.database);
                fixture.arm(fault);
                auto failed = fixture.recover();
                REQUIRE_FALSE(failed);
                check_safe_error(failed.error(), "db.io");
                require_consumed_faults(*fixture.faults);
                fixture.storage.reopen();
                CHECK(storage_snapshot(fixture.storage.database) == before);

                REQUIRE(fixture.recover());
                fixture.check_repaired(scenario);
                fixture.check_idempotent();
            }
        }
    }
}

TEST_CASE("Recovery scan errors fail safely before any repair", "[jobu][recovery][fault][sqlite]")
{
    for (auto const* scan : {"scan.runs", "scan.attempts", "scan.output", "scan.jobs", "scan.queues", "read.context"}) {
        for (auto operation : {Operation::Prepare, Operation::Execute, Operation::Fetch}) {
            DYNAMIC_SECTION(scan << ' ' << static_cast<int>(operation))
            {
                Fixture fixture;
                auto    before = storage_snapshot(fixture.storage.database);
                fixture.arm({.boundary = scan, .operation = operation});
                auto failed = fixture.recover();
                REQUIRE_FALSE(failed);
                check_safe_error(failed.error(), "db.io");
                require_consumed_faults(*fixture.faults);
                CHECK(fixture.commits() == 0U);
                fixture.storage.reopen();
                CHECK(storage_snapshot(fixture.storage.database) == before);
                REQUIRE(fixture.recover());
            }
        }
    }
}

TEST_CASE("Recovery failures after a committed unit retain progress under both policies",
          "[jobu][recovery][fault][sqlite]")
{
    for (auto scenario : {Scenario::Terminal, Scenario::Retry}) {
        for (auto const* stage : {"scan.running", "repair.output"}) {
            DYNAMIC_SECTION(static_cast<int>(scenario) << ' ' << stage)
            {
                Fixture fixture{scenario};
                auto    second_job = fixture.storage.make_job(recovery_id(4), fixture.queue.id);
                second_job.attributes.at("retry.initial_delay").data = Duration{5s};
                fixture.storage.insert_job(second_job);
                auto second = fixture.storage.make_run(recovery_id(5), second_job, RunState::Running);
                fixture.storage.insert_run(second);
                fixture.faults->calls.clear();
                bool armed  = false;
                auto failed = fixture.recover([&] {
                    // Arm by acknowledged repair-unit progress, never a global SQL call ordinal.
                    if (!armed && fixture.commits() == 1U) {
                        fixture.arm({.boundary = stage, .operation = Operation::Execute});
                        armed = true;
                    }
                    return false;
                });
                REQUIRE_FALSE(failed);
                REQUIRE(armed);
                check_safe_error(failed.error(), "db.io");
                require_consumed_faults(*fixture.faults);
                fixture.storage.reopen();
                fixture.check_interrupted(*fixture.original, scenario == Scenario::Retry);
                fixture.storage.require_run(second);

                auto resumed = fixture.recover();
                REQUIRE(resumed);
                check_report(*resumed,
                             {.interrupted_attempts = 1,
                              .retrying_runs        = scenario == Scenario::Retry ? 1U : 0U,
                              .terminal_runs        = scenario == Scenario::Terminal ? 1U : 0U});
                fixture.check_interrupted(*fixture.original, scenario == Scenario::Retry);
                fixture.check_interrupted(second, scenario == Scenario::Retry);
                fixture.check_idempotent();
            }
        }
    }
}

TEST_CASE("Recovery final validation failure preserves already committed repairs", "[jobu][recovery][fault][sqlite]")
{
    Fixture fixture{Scenario::QueueSuspension};
    bool    armed  = false;
    auto    failed = fixture.recover([&] {
        if (!armed && fixture.commits() == 1U) {
            fixture.arm({.boundary = "scan.runs", .operation = Operation::Fetch});
            armed = true;
        }
        return false;
    });
    REQUIRE_FALSE(failed);
    check_safe_error(failed.error(), "db.io");
    require_consumed_faults(*fixture.faults);
    fixture.storage.reopen();
    fixture.check_repaired(Scenario::QueueSuspension);
    fixture.check_idempotent();
}

TEST_CASE("Recovery lost commit acknowledgement preserves the committed repair", "[jobu][recovery][fault][sqlite]")
{
    for (auto scenario : scenarios) {
        DYNAMIC_SECTION(static_cast<int>(scenario))
        {
            Fixture fixture{scenario};
            fixture.arm({.boundary = "connection", .operation = Operation::Commit, .phase = Phase::AfterSuccess});
            auto failed = fixture.recover();
            REQUIRE_FALSE(failed);
            check_safe_error(failed.error(), "db.io");
            require_consumed_faults(*fixture.faults);
            fixture.storage.reopen();
            fixture.check_repaired(scenario);
            fixture.check_idempotent();
        }
    }
}

TEST_CASE("Recovery rollback failure poisons the connection and outranks cancellation",
          "[jobu][recovery][fault][sqlite]")
{
    for (bool cancellation : {false, true}) {
        DYNAMIC_SECTION("cancellation " << cancellation)
        {
            Fixture fixture{Scenario::RecurringSuspension};
            auto    before = storage_snapshot(fixture.storage.database);
            if (!cancellation) {
                fixture.arm({.boundary = "repair.output", .operation = Operation::Execute});
            }
            fixture.arm({.boundary = "connection", .operation = Operation::Rollback}, "db.rollback_failed");
            auto failed = fixture.recover([&] {
                // Cancel at the precommit poll after the last repair write, leaving real changes to roll back.
                return cancellation &&
                       std::ranges::find(fixture.faults->calls,
                                         DatabaseCall{.boundary  = "repair.job_suspension",
                                                      .operation = Operation::Execute,
                                                      .phase     = Phase::AfterSuccess}) != fixture.faults->calls.end();
            });
            REQUIRE_FALSE(failed);
            check_safe_error(failed.error(), cancellation ? "db.rollback_failed" : "db.io");
            require_consumed_faults(*fixture.faults);
            CHECK(fixture.storage.database.is_poisoned());
            auto calls  = fixture.faults->calls.size();
            auto reused = fixture.storage.database.transaction();
            REQUIRE_FALSE(reused);
            CHECK(reused.error().code == "db.connection_failed");
            CHECK(fixture.faults->calls.size() == calls);
            fixture.storage.reopen();
            CHECK(storage_snapshot(fixture.storage.database) == before);
            REQUIRE(fixture.recover());
            fixture.check_repaired(Scenario::RecurringSuspension);
            fixture.check_idempotent();
        }
    }
}
