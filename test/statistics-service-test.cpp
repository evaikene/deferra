#include "statistics_service.hpp"

#include "domain_storage_priv.hpp"
#include "query.hpp"
#include "support/fake_time_source.hpp"
#include "support/fault_database_driver.hpp"
#include "support/recovery_fixture.hpp"
#include "support/sequence_uuid_generator.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

using namespace jb::core;
using namespace jb::db;
using namespace jb::jobu;
using namespace jb::test;
using namespace std::chrono_literals;

namespace {

struct Fixture {
    RecoveryFixture       storage;
    FakeTimeSource        time;
    SequenceUuidGenerator tokens{
        {recovery_id(101), recovery_id(102), recovery_id(103), recovery_id(104)}
    };
    StatisticsService  statistics{storage.database, tokens, time};
    std::vector<Error> failures;

    explicit Fixture(std::function<std::unique_ptr<Driver>(std::unique_ptr<Driver>)> wrap_driver = {})
        : storage{std::move(wrap_driver)}
    {
        time.set_utc(UtcTimePoint{100s});
        statistics.failed.connect(&statistics, [this](Error const& error) { failures.push_back(error); });
    }

    auto job(std::uint32_t queue_suffix, std::uint32_t job_suffix, JobType type = JobType::Cli) -> JobDefinition
    {
        auto queue = recovery_queue(recovery_id(queue_suffix));
        storage.insert_queue(queue);
        auto result = storage.make_job(recovery_id(job_suffix), queue.id, type);
        storage.insert_job(result);
        return result;
    }

    void execute(std::string_view sql, Uuid const& id)
    {
        Query query{storage.database};
        REQUIRE(query.prepare(sql));
        REQUIRE(query.bind_value(":id", jb::jobu::detail::uuid_to_storage(id)));
        REQUIRE(query.exec());
    }
};

auto window() -> UtcRange
{
    return {.from = UtcTimePoint{9s}, .to = UtcTimePoint{11s}};
}

struct QueryObservation {
    std::vector<std::string> plan_steps;
    std::size_t              rows{0};
    double                   mean_ms{0};
};

auto observe_query(Database&                                         database,
                   std::string const&                                sql,
                   std::vector<std::pair<std::string, Value>> const& bindings) -> QueryObservation
{
    auto bind_all = [&](Query& query) {
        for (auto const& [name, value] : bindings) {
            REQUIRE(query.bind_value(name, value));
        }
    };

    auto observation = QueryObservation{};
    {
        Query explain{database};
        REQUIRE(explain.prepare("EXPLAIN QUERY PLAN " + sql));
        bind_all(explain);
        REQUIRE(explain.exec());
        while (true) {
            auto next = explain.next();
            REQUIRE(next);
            if (!*next) {
                break;
            }
            auto detail = jb::jobu::detail::read_text(explain.record(), "detail");
            REQUIRE(detail);
            observation.plan_steps.push_back(std::move(*detail));
        }
    }

    // The same prepared SQL and bindings are drained repeatedly for a cost observation, never a timing assertion.
    Query measured{database};
    REQUIRE(measured.prepare(sql));
    bind_all(measured);
    auto const start = std::chrono::steady_clock::now();
    for (auto repetition = 0; repetition < 20; ++repetition) {
        REQUIRE(measured.exec());
        while (true) {
            auto next = measured.next();
            REQUIRE(next);
            if (!*next) {
                break;
            }
            if (repetition == 0) {
                ++observation.rows;
            }
        }
        REQUIRE(measured.finish());
    }
    auto const elapsed  = std::chrono::duration<double, std::milli>{std::chrono::steady_clock::now() - start};
    observation.mean_ms = elapsed.count() / 20.0;
    return observation;
}

auto includes_step(QueryObservation const& observation, std::string_view needle) -> bool
{
    for (auto const& step : observation.plan_steps) {
        if (step.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

void print_observation(std::string_view name, QueryObservation const& observation)
{
    std::cout << "statistics " << name << ": rows=" << observation.rows << " mean_ms=" << observation.mean_ms << '\n';
    for (auto const& step : observation.plan_steps) {
        std::cout << "  " << step << '\n';
    }
}

} // namespace

TEST_CASE("Statistics count a planned-run cohort and its retries once", "[jobu][statistics][sqlite]")
{
    Fixture fixture;
    auto    job            = fixture.job(1, 2);
    auto    run            = fixture.storage.make_run(recovery_id(10), job, RunState::Succeeded, 2);
    run.attempts[0].output = jb::jobu::detail::AttemptOutput{.stdout_truncated = true};
    run.attempts[1].output =
        jb::jobu::detail::AttemptOutput{.stdout_truncated = true, .stderr_truncated = true, .capture_lost = true};
    fixture.storage.insert_run(run);

    auto excluded           = fixture.storage.make_run(recovery_id(11), job, RunState::Succeeded);
    excluded.run.planned_at = UtcTimePoint{11s};
    fixture.storage.insert_run(excluded);

    auto page = fixture.statistics.read(StatisticsRequest{.planned = window()}, StatisticsScope::System);
    REQUIRE(page);
    REQUIRE(page->groups.size() == 1);
    CHECK_FALSE(page->next_cursor);
    auto const& group = page->groups.front();
    CHECK(std::holds_alternative<std::monostate>(group.key));
    CHECK(group.runs.total == 1);
    CHECK(group.runs.succeeded == 1);
    CHECK(group.runs.cli == 1);
    CHECK(group.runs.scheduled_origin == 1);
    CHECK(group.attempts.total == 3);
    CHECK(group.attempts.completed == 3);
    CHECK(group.attempts.failed == 2);
    CHECK(group.attempts.succeeded == 1);
    CHECK(group.attempts.retries == 2);
    CHECK(group.capture.truncated_attempts == 2);
    CHECK(group.capture.lost_attempts == 1);
    CHECK(group.schedule_lateness_ms.samples == 1);
    CHECK(group.schedule_lateness_ms.average == 1000.0);
    CHECK(group.execution_wall_duration_ms.samples == 3);
    CHECK(group.execution_wall_duration_ms.maximum == 1000.0);
    CHECK_FALSE(group.runnable_wait_ms);
    CHECK(page->measurement.timing == "wall_clock_derived");
    CHECK(page->measurement.runnable_wait == "unavailable");
    CHECK(page->measurement.capture == "persisted_output_flags");
}

TEST_CASE("Statistics projections use populated v2 schema indexes", "[jobu][statistics][sqlite][plan]")
{
    struct CapturedSql {
        std::string groups;
        std::string runs;
        std::string attempts;
    } captured;

    auto faults      = std::make_shared<DatabaseFaultState>();
    faults->classify = [&captured](std::string_view sql) -> std::string {
        if (sql.starts_with("SELECT DISTINCT r.queue_id AS group_key")) {
            captured.groups = sql;
        }
        else if (sql.starts_with("SELECT r.state AS run_state")) {
            captured.runs = sql;
        }
        else if (sql.starts_with("SELECT a.attempt_number AS attempt_number")) {
            captured.attempts = sql;
        }
        return "statistics.plan";
    };
    Fixture fixture{[faults](std::unique_ptr<Driver> driver) {
        return std::make_unique<FaultDatabaseDriver>(std::move(driver), faults);
    }};

    // Seed the production schema and indexes with several groups and attempts, including output join rows.
    for (std::uint32_t queue = 0; queue < 10; ++queue) {
        auto job = fixture.job(1'000 + queue, 2'000 + queue);
        for (std::uint32_t number = 0; number < 20; ++number) {
            auto run =
                fixture.storage.make_run(recovery_id(3'000 + (queue * 20) + number), job, RunState::Succeeded, 1);
            run.run.planned_at = UtcTimePoint{10s + std::chrono::seconds{number}};
            for (auto& attempt : run.attempts) {
                attempt.output = jb::jobu::detail::AttemptOutput{};
            }
            fixture.storage.insert_run(run);
        }
    }

    auto page = fixture.statistics.read(
        StatisticsRequest{
            .planned  = {.from = UtcTimePoint{9s}, .to = UtcTimePoint{31s}},
            .group_by = StatisticsGroupBy::Queue,
            .limit    = 1
    },
        StatisticsScope::System);
    REQUIRE(page);
    REQUIRE(page->groups.size() == 1);
    REQUIRE_FALSE(captured.groups.empty());
    REQUIRE_FALSE(captured.runs.empty());
    REQUIRE_FALSE(captured.attempts.empty());

    auto const window_bindings = std::vector<std::pair<std::string, Value>>{
        {":planned_from", std::int64_t{9'000'000} },
        {":planned_to",   std::int64_t{31'000'000}}
    };
    auto group_bindings = window_bindings;
    group_bindings.emplace_back(":limit", std::int64_t{2});
    auto aggregate_bindings = window_bindings;
    aggregate_bindings.emplace_back(":group_key", jb::jobu::detail::uuid_to_storage(recovery_id(1'000)));

    auto  groups   = observe_query(fixture.storage.database, captured.groups, group_bindings);
    auto  runs     = observe_query(fixture.storage.database, captured.runs, aggregate_bindings);
    auto  attempts = observe_query(fixture.storage.database, captured.attempts, aggregate_bindings);
    Query version{fixture.storage.database};
    REQUIRE(version.exec("SELECT sqlite_version() AS version"));
    REQUIRE(version.next());
    auto sqlite_version = jb::jobu::detail::read_text(version.record(), "version");
    REQUIRE(sqlite_version);
    std::cout << "statistics SQLite version=" << *sqlite_version << '\n';
    print_observation("groups", groups);
    print_observation("runs", runs);
    print_observation("attempts", attempts);

    CHECK(groups.rows == 2);
    CHECK(runs.rows == 20);
    CHECK(attempts.rows == 40);
    CHECK(includes_step(groups, "jobu_runs_"));
    CHECK(includes_step(runs, "jobu_runs_queue_planned_id_idx"));
    CHECK(includes_step(attempts, "jobu_runs_queue_planned_id_idx"));
    CHECK(includes_step(attempts, "jobu_attempts"));
    CHECK(includes_step(attempts, "jobu_attempt_output"));
}

TEST_CASE("Statistics group pages preserve snapshot owners and the resolved window", "[jobu][statistics][sqlite]")
{
    Fixture fixture;
    auto    first  = fixture.job(1, 2);
    auto    second = fixture.job(3, 4);
    auto    third  = fixture.job(5, 6);
    fixture.storage.insert_run(fixture.storage.make_run(recovery_id(10), first, RunState::Succeeded));
    fixture.storage.insert_run(fixture.storage.make_run(recovery_id(11), second, RunState::Succeeded));
    fixture.storage.insert_run(fixture.storage.make_run(recovery_id(12), third, RunState::Succeeded));

    // Current job ownership can change without altering the queue captured by an older run.
    Query move{fixture.storage.database};
    REQUIRE(move.prepare("UPDATE jobu_jobs SET queue_id = :queue WHERE id = :id"));
    REQUIRE(move.bind_value(":queue", jb::jobu::detail::uuid_to_storage(third.queue_id)));
    REQUIRE(move.bind_value(":id", jb::jobu::detail::uuid_to_storage(first.id)));
    REQUIRE(move.exec());
    fixture.execute("UPDATE jobu_jobs SET state = 'deleted', deleted_at_us = 13000000 WHERE id = :id", second.id);
    fixture.execute("UPDATE jobu_queues SET state = 'deleted', deleted_name = name, "
                    "name = 'deleted-statistics-owner', deleted_at_us = 13000000 WHERE id = :id",
                    second.queue_id);

    auto initial    = StatisticsRequest{.planned = window(), .group_by = StatisticsGroupBy::Queue, .limit = 1};
    auto first_page = fixture.statistics.read(initial, StatisticsScope::System);
    REQUIRE(first_page);
    REQUIRE(first_page->groups.size() == 1);
    CHECK(std::get<Uuid>(first_page->groups[0].key) == first.queue_id);
    REQUIRE(first_page->next_cursor);

    fixture.time.set_utc(UtcTimePoint{200s});
    auto wrong_scope = fixture.statistics.read(CursorRequest{*first_page->next_cursor}, StatisticsScope::Queue);
    REQUIRE_FALSE(wrong_scope);
    CHECK(wrong_scope.error().code == "jobu.statistics.invalid_cursor");
    auto second_page = fixture.statistics.read(CursorRequest{*first_page->next_cursor}, StatisticsScope::System);
    REQUIRE(second_page);
    CHECK(second_page->window.to == UtcTimePoint{11s});
    REQUIRE(second_page->groups.size() == 1);
    CHECK(std::get<Uuid>(second_page->groups[0].key) == second.queue_id);
    REQUIRE(second_page->next_cursor);
    auto retried_page = fixture.statistics.read(CursorRequest{*first_page->next_cursor}, StatisticsScope::System);
    REQUIRE(retried_page);
    CHECK(std::get<Uuid>(retried_page->groups[0].key) == second.queue_id);
    auto third_page = fixture.statistics.read(CursorRequest{*second_page->next_cursor}, StatisticsScope::System);
    REQUIRE(third_page);
    REQUIRE(third_page->groups.size() == 1);
    CHECK(std::get<Uuid>(third_page->groups[0].key) == third.queue_id);
    CHECK_FALSE(third_page->next_cursor);

    auto queue_page = fixture.statistics.read(StatisticsRequest{.queue_id = first.queue_id, .planned = window()},
                                              StatisticsScope::Queue);
    REQUIRE(queue_page);
    CHECK(queue_page->groups[0].runs.total == 1);

    fixture.time.set_monotonic(TimePoint{} + 6min);
    auto expired = fixture.statistics.read(CursorRequest{*first_page->next_cursor}, StatisticsScope::System);
    REQUIRE_FALSE(expired);
    CHECK(expired.error().code == "jobu.statistics.invalid_cursor");
}

TEST_CASE("Statistics validate windows and report empty or reversed measurements honestly",
          "[jobu][statistics][sqlite]")
{
    Fixture fixture;
    auto    empty = fixture.statistics.read(StatisticsRequest{}, StatisticsScope::System);
    REQUIRE(empty);
    REQUIRE(empty->groups.size() == 1);
    CHECK(empty->groups[0].runs.total == 0);
    CHECK(empty->groups[0].attempts.total == 0);
    CHECK(empty->groups[0].schedule_lateness_ms.samples == 0);
    CHECK_FALSE(empty->groups[0].schedule_lateness_ms.average);
    CHECK_FALSE(empty->groups[0].execution_wall_duration_ms.maximum);
    CHECK(empty->window.to == UtcTimePoint{100s});
    CHECK(empty->window.from == UtcTimePoint{100s - 24h});

    CHECK_FALSE(fixture.statistics.read(StatisticsRequest{.limit = 0}, StatisticsScope::System));
    CHECK_FALSE(fixture.statistics.read(StatisticsRequest{.limit = 201}, StatisticsScope::System));
    CHECK_FALSE(fixture.statistics.read(
        StatisticsRequest{
            .planned = {.from = UtcTimePoint{0s}, .to = UtcTimePoint{32 * 24h}}
    },
        StatisticsScope::System));
    CHECK(fixture.statistics.read(
        StatisticsRequest{
            .planned = {.from = UtcTimePoint{0s}, .to = UtcTimePoint{31 * 24h}}
    },
        StatisticsScope::System));
    CHECK_FALSE(fixture.statistics.read(StatisticsRequest{}, StatisticsScope::Queue));
    CHECK_FALSE(fixture.statistics.read(StatisticsRequest{.origin = RunOrigin::Submitted}, StatisticsScope::System));

    auto job                             = fixture.job(1, 2);
    auto run                             = fixture.storage.make_run(recovery_id(10), job, RunState::Succeeded);
    run.run.started_at                   = UtcTimePoint{9s};
    run.attempts[0].attempt.completed_at = UtcTimePoint{10s};
    fixture.storage.insert_run(run);
    auto reversed = fixture.statistics.read(StatisticsRequest{.planned = window()}, StatisticsScope::System);
    REQUIRE(reversed);
    CHECK(reversed->groups[0].runs.total == 1);
    CHECK(reversed->groups[0].attempts.completed == 1);
    CHECK(reversed->groups[0].schedule_lateness_ms.samples == 0);
    CHECK(reversed->groups[0].execution_wall_duration_ms.samples == 0);
}

TEST_CASE("Statistics group enum keys by documented wire order", "[jobu][statistics][sqlite]")
{
    Fixture fixture;
    auto    cli       = fixture.job(1, 2);
    auto    http      = fixture.job(3, 4, JobType::Http);
    auto    manual    = fixture.storage.make_run(recovery_id(10), cli, RunState::Succeeded, 0, RunOrigin::Manual);
    auto    scheduled = fixture.storage.make_run(recovery_id(11), http, RunState::Failed);
    fixture.storage.insert_run(manual);
    fixture.storage.insert_run(scheduled);

    auto types = fixture.statistics.read(StatisticsRequest{.planned = window(), .group_by = StatisticsGroupBy::Type},
                                         StatisticsScope::System);
    REQUIRE(types);
    REQUIRE(types->groups.size() == 2);
    CHECK(std::get<JobType>(types->groups[0].key) == JobType::Cli);
    CHECK(std::get<JobType>(types->groups[1].key) == JobType::Http);

    auto origins =
        fixture.statistics.read(StatisticsRequest{.planned = window(), .group_by = StatisticsGroupBy::Origin},
                                StatisticsScope::System);
    REQUIRE(origins);
    REQUIRE(origins->groups.size() == 2);
    CHECK(std::get<RunOrigin>(origins->groups[0].key) == RunOrigin::Manual);
    CHECK(std::get<RunOrigin>(origins->groups[1].key) == RunOrigin::Scheduled);

    auto failed = fixture.statistics.read(
        StatisticsRequest{.type = JobType::Http, .planned = window(), .group_by = StatisticsGroupBy::State},
        StatisticsScope::System);
    REQUIRE(failed);
    REQUIRE(failed->groups.size() == 1);
    CHECK(std::get<RunState>(failed->groups[0].key) == RunState::Failed);
    CHECK(failed->groups[0].runs.total == 1);
}

TEST_CASE("Statistics widen wall-clock subtraction across the signed epoch", "[jobu][statistics][sqlite]")
{
    Fixture    fixture;
    auto       job     = fixture.job(1, 2);
    auto       run     = fixture.storage.make_run(recovery_id(10), job, RunState::Succeeded);
    auto const span    = std::chrono::hours{24 * 365 * 200};
    run.run.planned_at = UtcTimePoint{} - span;
    run.run.started_at = UtcTimePoint{} + span;
    fixture.storage.insert_run(run);

    auto page = fixture.statistics.read(
        StatisticsRequest{
            .planned = {.from = run.run.planned_at - 1s, .to = run.run.planned_at + 1s}
    },
        StatisticsScope::System);
    REQUIRE(page);
    REQUIRE(page->groups.size() == 1);
    auto const& duration = page->groups[0].schedule_lateness_ms;
    CHECK(duration.samples == 1);
    REQUIRE(duration.average);
    CHECK(std::isfinite(*duration.average));
    CHECK(*duration.average > 1.0e12);
}

TEST_CASE("Malformed retained capture flags close statistics admission after query cleanup",
          "[jobu][statistics][sqlite]")
{
    Fixture fixture;
    auto    job            = fixture.job(1, 2);
    auto    run            = fixture.storage.make_run(recovery_id(10), job, RunState::Succeeded);
    run.attempts[0].output = jb::jobu::detail::AttemptOutput{};
    fixture.storage.insert_run(run);

    Query pragma{fixture.storage.database};
    REQUIRE(pragma.exec("PRAGMA ignore_check_constraints = ON"));
    fixture.execute("UPDATE jobu_attempt_output SET capture_lost = 2 WHERE run_id = :id", run.run.id);

    auto result = fixture.statistics.read(StatisticsRequest{.planned = window()}, StatisticsScope::System);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == "jobu.storage.invariant");
    REQUIRE(fixture.failures.size() == 1);
    CHECK(fixture.failures[0].code == "jobu.storage.invariant");
    auto stopped = fixture.statistics.read(StatisticsRequest{}, StatisticsScope::System);
    REQUIRE_FALSE(stopped);
    CHECK(stopped.error().code == "jobu.service.stopping");
}
