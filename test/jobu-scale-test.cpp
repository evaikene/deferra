#include "execution_telemetry.hpp"

#include "attempt_repository_priv.hpp"
#include "byte_buffer.hpp"
#include "domain_storage_priv.hpp"
#include "execution_telemetry_priv.hpp"
#include "idempotency_codec_priv.hpp"
#include "idempotency_repository_priv.hpp"
#include "job_repository_priv.hpp"
#include "management.hpp"
#include "query.hpp"
#include "queue_repository_priv.hpp"
#include "recovery.hpp"
#include "retention.hpp"
#include "retention_owner_repository_priv.hpp"
#include "retention_repository_priv.hpp"
#include "run_eligibility_priv.hpp"
#include "run_repository_priv.hpp"
#include "scheduler_core_priv.hpp"
#include "scheduler_repository_priv.hpp"
#include "secret_provider_priv.hpp"
#include "statistics_service.hpp"
#include "transaction.hpp"
#include "wait_repository_priv.hpp"

#include "support/catch_utils.hpp" // IWYU pragma: keep for Catch::StringMaker specializations
#include "support/fake_attempt_executor.hpp"
#include "support/fake_cron_engine.hpp"
#include "support/fake_event_loop_backend.hpp"
#include "support/fake_time_source.hpp"
#include "support/jobu_scale_fixture.hpp"
#include "support/storage_fault_helpers.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

using namespace jb::core;
using namespace jb::jobu;
using namespace jb::jobu::detail;
using namespace jb::test;
using namespace std::chrono_literals;
using LoopAccess = jb::core::priv::EventLoopTestAccess;

namespace {

auto environment(std::string const& name) -> std::string
{
    auto const* value = std::getenv(name.c_str());
    return value ? value : "";
}

auto profile() -> JobuScaleProfile
{
    auto const name = environment("JOBU_SCALE_PROFILE");
    REQUIRE((name.empty() || name == "small" || name == "medium" || name == "full"));
    if (name == "full") {
        return {.queues = 100, .pending = 10'000, .terminal = 100'000};
    }
    if (name == "medium") {
        return {.queues = 100, .pending = 1'000, .terminal = 10'000};
    }
    return {};
}

auto fixture_path() -> std::filesystem::path
{
    auto path = environment("JOBU_SCALE_DATABASE");
    REQUIRE_FALSE(path.empty());
    REQUIRE(std::filesystem::path{path}.is_absolute());
    return path;
}

struct ScaleRuntime {
    explicit ScaleRuntime(std::optional<std::filesystem::path> const& path = std::nullopt)
        : data{path}
        , secrets{data.storage.database}
    {
        time.set_utc(UtcTimePoint{100s});
        time.set_monotonic(TimePoint{1000s});
        executor.set_available(JobType::Cli, true);
        executor.set_available(JobType::Http, true);
        telemetry = std::make_unique<ExecutionTelemetry>(data.storage.database, data.storage.registry, time, generator);
        REQUIRE(TelemetryAccess::register_executor(*telemetry,
                                                   data.storage.database,
                                                   data.storage.registry,
                                                   time,
                                                   executor));
    }

    void activate()
    {
        REQUIRE(telemetry->start());
        core = std::make_unique<SchedulerCore>(
            data.storage.database,
            data.storage.registry,
            cron,
            generator,
            time,
            executor,
            secrets,
            SchedulerCoreOptions{.cli_concurrency = 1, .http_concurrency = 1, .telemetry = telemetry.get()});
        telemetry->delayed.connect(&receiver, [this](DelayedRun const& warning) {
            CHECK_FALSE(data.observation.transaction_open);
            ++warnings;
            last_warning = warning.run_id;
        });
    }

    void open_pending()
    {
        auto timing =
            TelemetryAccess::mutation_boundary(telemetry.get(), data.storage.database, data.storage.registry, time);
        REQUIRE(timing);
        auto transaction = jb::db::Transaction::begin(data.storage.database);
        REQUIRE(transaction);
        REQUIRE(timing->reconcile_scope({.kind = EligibilityScope::Kind::All}));
        REQUIRE(transaction->commit());
    }

    void stop()
    {
        core = nullptr;
        telemetry->request_stop();
        REQUIRE(telemetry->finish_stop());
    }

    jb::core::priv::FakeEventLoop          loop{jb::core::priv::make_fake_event_loop()};
    jb::core::priv::ScopedCurrentEventLoop current{loop.loop.get()};
    JobuScaleFixture                       data;
    FakeTimeSource                         time;
    FakeCronEngine                         cron;
    ScaleUuidGenerator                     generator;
    FakeAttemptExecutor                    executor;
    DatabaseSecretProvider                 secrets;
    Object                                 receiver;
    std::size_t                            warnings{0};
    Uuid                                   last_warning;
    std::unique_ptr<ExecutionTelemetry>    telemetry;
    std::unique_ptr<SchedulerCore>         core;
};

auto request(StatisticsGroupBy dimension = StatisticsGroupBy::None) -> StatisticsRequest
{
    return {
        .planned  = {.from = UtcTimePoint{9s}, .to = UtcTimePoint{11s}},
        .group_by = dimension,
        .limit    = 1
    };
}

void assert_coverage(StatisticsGroup const& group)
{
    auto const& coverage = group.runnable_wait_coverage;
    CHECK(coverage.complete + coverage.partial + coverage.unmeasured + coverage.unfinished == group.runs.total);
    REQUIRE(group.runnable_wait_ms);
    CHECK(group.runnable_wait_ms->samples == coverage.complete);
}

void heartbeat(ScaleRuntime& fixture)
{
    bool delivered = false;
    REQUIRE(fixture.loop.loop->post([&] {
        CHECK_FALSE(fixture.data.observation.transaction_open);
        delivered = true;
    }));
    CHECK(fixture.loop.loop->process_events(EventFlag::Tasks) != ProcessEventsResult::Failed);
    REQUIRE(delivered);
}

} // namespace

TEST_CASE("Eligibility retry counts seek the run's attempts and preserve failed or interrupted history",
          "[scale][eligibility][plan]")
{
    JobuScaleFixture fixture;
    fixture.seed({.queues = 3, .pending = 3, .terminal = 600});
    auto job   = fixture.storage.make_job(recovery_id(90'000'000), JobuScaleFixture::queue_id(0));
    auto retry = fixture.storage.make_run(recovery_id(90'000'001), job, RunState::RetryWait, 2);
    retry.attempts.back().attempt.outcome = AttemptOutcome::Interrupted;
    fixture.storage.insert_job(job);
    fixture.storage.insert_run(retry);

    auto page = list_eligibility(fixture.storage.database, {.kind = EligibilityScope::Kind::All});
    REQUIRE(page);
    CHECK(page->size() == 4);
    auto queue_page =
        list_eligibility(fixture.storage.database, {.kind = EligibilityScope::Kind::Queue, .id = job.queue_id});
    REQUIRE(queue_page);
    CHECK(queue_page->size() == 2);
    auto job_page = list_eligibility(fixture.storage.database, {.kind = EligibilityScope::Kind::Job, .id = job.id});
    REQUIRE(job_page);
    REQUIRE(job_page->size() == 1);
    CHECK(job_page->front().state == RunState::RetryWait);
    auto tail = list_eligibility(fixture.storage.database,
                                 {.kind = EligibilityScope::Kind::All},
                                 JobuScaleFixture::pending_id(2));
    REQUIRE(tail);
    REQUIRE(tail->size() == 1);
    CHECK(tail->front().id == retry.run.id);

    SchedulerRepository scheduler{fixture.storage.database, fixture.storage.registry};
    auto                candidates = scheduler.list_runnable(job.queue_id, JobType::Cli, UtcTimePoint{100s}, 200);
    REQUIRE(candidates);
    CHECK(candidates->size() == 2);

    // Explain the actual unscoped production statement. Both attempt projections must
    // seek the selected run, rather than revisit the unrelated Completed history.
    std::string sql;
    for (auto const& [statement, name] : fixture.observation.statements) {
        if (name == "eligibility" && statement.find(":scope") == std::string::npos &&
            statement.find(":after") == std::string::npos) {
            sql = statement;
        }
    }

    std::string candidate_sql;
    for (auto const& [statement, name] : fixture.observation.statements) {
        if (name == "scheduler.projection") {
            candidate_sql = statement;
        }
    }
    REQUIRE_FALSE(candidate_sql.empty());
    auto bind_candidate = [&](jb::db::Query& query) {
        REQUIRE(query.bind_value(":queue_id", uuid_to_storage(job.queue_id)));
        REQUIRE(query.bind_value(":type", jb::db::make_text("cli")));
        REQUIRE(query.bind_value(":now", std::int64_t{100'000'000}));
        REQUIRE(query.bind_value(":limit", std::int64_t{200}));
    };
    {
        jb::db::Query plan{fixture.storage.database};
        REQUIRE(plan.prepare("EXPLAIN QUERY PLAN " + candidate_sql));
        bind_candidate(plan);
        REQUIRE(plan.exec());
        std::size_t history_seeks = 0;
        for (;;) {
            auto row = plan.next();
            REQUIRE(row);
            if (!*row) {
                break;
            }
            auto detail = read_text(plan.record(), "detail");
            REQUIRE(detail);
            if (detail->starts_with("SEARCH completed_attempts ") ||
                detail->starts_with("SEARCH retry_history_attempts ")) {
                ++history_seeks;
                INFO(*detail);
                CHECK(detail->find("(run_id=?)") != std::string::npos);
            }
        }
        CHECK(history_seeks == 2);
    }
    {
        jb::db::Query projection{fixture.storage.database};
        REQUIRE(projection.prepare(candidate_sql));
        bind_candidate(projection);
        REQUIRE(projection.exec());
        for (;;) {
            auto row = projection.next();
            REQUIRE(row);
            if (!*row) {
                break;
            }
            auto id = read_uuid(projection.record(), "run_id");
            REQUIRE(id);
            auto const expected = *id == retry.run.id ? 2 : 0;
            for (auto const* column : {"completed_attempt_count", "retry_history_attempt_count"}) {
                REQUIRE(projection.value(column));
                CHECK(std::get<std::int64_t>(*projection.value(column)) == expected);
            }
        }
    }
    REQUIRE_FALSE(sql.empty());
    {
        jb::db::Query plan{fixture.storage.database};
        REQUIRE(plan.exec("EXPLAIN QUERY PLAN " + sql));
        std::size_t attempt_seeks = 0;
        for (;;) {
            auto row = plan.next();
            REQUIRE(row);
            if (!*row) {
                break;
            }
            auto detail = read_text(plan.record(), "detail");
            REQUIRE(detail);
            if (detail->starts_with("SEARCH a ")) {
                ++attempt_seeks;
                INFO(*detail);
                CHECK(detail->find("(run_id=?)") != std::string::npos);
            }
        }
        CHECK(attempt_seeks == 2);
    }
    {
        jb::db::Query projection{fixture.storage.database};
        REQUIRE(projection.exec(sql));
        for (;;) {
            auto row = projection.next();
            REQUIRE(row);
            if (!*row) {
                break;
            }
            auto id = read_uuid(projection.record(), "id");
            REQUIRE(id);
            auto const expected = *id == retry.run.id ? 2 : 0;
            REQUIRE(projection.value("retry_history_count"));
            CHECK(std::get<std::int64_t>(*projection.value("retry_history_count")) == expected);
        }
    }

    // A successful attempt must still make RetryWait corrupt; the cheaper projection
    // must not weaken the existing equality check between total and retry history.
    fixture.execute("UPDATE jobu_attempts SET outcome='succeeded' WHERE attempt_number=2 AND outcome='interrupted'");
    auto invalid = list_eligibility(fixture.storage.database, {.kind = EligibilityScope::Kind::Job, .id = job.id});
    REQUIRE_FALSE(invalid);
    CHECK(invalid.error().origin == StorageFailureOrigin::PersistedData);
    CHECK(invalid.error().error.code == "jobu.telemetry.invalid_state");
    CHECK_FALSE(scheduler.list_runnable(job.queue_id, JobType::Cli, UtcTimePoint{100s}, 200));
}

TEST_CASE("Scale observation reaches tails and checkpoint pages leave the loop available", "[scale][telemetry]")
{
    ScaleRuntime fixture;
    auto const   size = profile();
    fixture.data.seed(size);
    fixture.activate();
    measure_scale("cycle.open", size.pending, [&] { REQUIRE(fixture.core->process_cycle()); });
    REQUIRE(fixture.executor.pending_keys().size() == 2);
    CHECK(fixture.data.count("state = 'scheduled'") == static_cast<std::int64_t>(size.pending - 2));

    fixture.time.advance(10ms);
    REQUIRE(fixture.core->process_cycle());
    CHECK(fixture.warnings == 0);
    fixture.time.advance(1us);
    measure_scale("cycle.warning", size.pending, [&] { REQUIRE(fixture.core->process_cycle()); });
    CHECK(fixture.warnings == size.pending - 2);
    CHECK(fixture.last_warning == JobuScaleFixture::pending_id(size.pending - 1));
    CHECK(fixture.data.scalar("SELECT COUNT(*) FROM jobu_run_timing WHERE delay_warned=1") ==
          static_cast<std::int64_t>(fixture.warnings));
    for (auto repetition = 0; repetition < 3; ++repetition) {
        measure_scale("cycle.claimed", size.pending, [&] { REQUIRE(fixture.core->process_cycle()); });
    }
    CHECK(fixture.warnings == size.pending - 2);
    CHECK(fixture.data.observation.maximum_page_rows["eligibility"] <= 201);

    // A busy queue mutation consumes the affected set synchronously. Its cost is recorded separately.
    ManagementService management{fixture.data.storage.database,
                                 fixture.data.storage.registry,
                                 fixture.cron,
                                 fixture.generator,
                                 fixture.time,
                                 {.telemetry = fixture.telemetry.get()}};
    measure_scale("queue.suspend", size.pending / 2, [&] {
        REQUIRE(management.suspend_queue(JobuScaleFixture::queue_id(0)));
    });
    measure_scale("queue.resume", size.pending / 2, [&] {
        REQUIRE(management.resume_queue(JobuScaleFixture::queue_id(0)));
    });
    fixture.time.advance(30s);
    for (std::size_t page = 0; page <= size.pending / 200; ++page) {
        fixture.data.observation.reset_counts();
        measure_scale("checkpoint.page", 200, [&] { LoopAccess::fire_next_timer(*fixture.loop.loop); });
        CHECK(fixture.data.observation.executes["timing.write"] <= 200);
        CHECK_FALSE(fixture.data.observation.transaction_open);
        CHECK(LoopAccess::active_timer_count(*fixture.loop.loop) == 1);
        heartbeat(fixture);
        fixture.time.advance(1ms);
    }
    CHECK(fixture.data.scalar("SELECT COUNT(*) FROM jobu_run_timing WHERE open_epoch IS NOT NULL "
                              "AND runnable_wait_us > 0") == static_cast<std::int64_t>(size.pending - 2));
    CHECK(fixture.data.observation.maximum_page_rows["open.page"] <= 201);
    fixture.data.query_plans();
    fixture.stop();
}

TEST_CASE("Scale retention visits skewed queues and statistics retain live coverage", "[scale][retention][statistics]")
{
    ScaleRuntime fixture;
    auto const   size = profile();
    fixture.data.seed(size);
    StatisticsService statistics{fixture.data.storage.database,
                                 fixture.generator,
                                 fixture.time,
                                 {.runnable_wait_available = true}};
    measure_scale("statistics.system", size.pending + size.terminal, [&] {
        auto page = statistics.read(request(), StatisticsScope::System);
        REQUIRE(page);
        REQUIRE(page->groups.size() == 1);
        CHECK(page->groups[0].runs.total == size.pending + size.terminal);
        CHECK(page->groups[0].attempts.total == size.terminal);
        assert_coverage(page->groups[0]);
    });
    auto first = statistics.read(request(StatisticsGroupBy::Queue), StatisticsScope::System);
    REQUIRE(first);
    REQUIRE(first->next_cursor);

    auto unlimited = std::int64_t{0};
    for (std::size_t index = 0; index < size.terminal; ++index) {
        unlimited += JobuScaleFixture::terminal_queue(index, size) % 3 == 2 ? 1 : 0;
    }
    RetentionService retention{
        fixture.data.storage.database,
        fixture.data.storage.registry,
        fixture.time,
        {.default_retention = 10s, .sweep_interval = 1s, .batch_size = 100}
    };
    std::size_t purged = 0;
    std::size_t sweeps = 0;
    retention.batch_completed.connect(&fixture.receiver, [&](RetentionPurgeCounts const& counts) {
        CHECK_FALSE(fixture.data.observation.transaction_open);
        CHECK(counts.runs <= 100);
        purged += counts.runs;
    });
    retention.sweep_completed.connect(&fixture.receiver, [&](RetentionPurgeCounts const&) { ++sweeps; });
    retention.failed.connect(&fixture.receiver, [](Error const& error) { FAIL(error.code); });
    REQUIRE(retention.start());
    measure_scale("purge.sweep", size.terminal, [&] {
        while (sweeps == 0) {
            measure_scale("purge.page", 100, [&] { LoopAccess::fire_next_timer(*fixture.loop.loop); });
            heartbeat(fixture);
        }
    });
    REQUIRE(purged > 0);
    // A full history batch advances to another queue, even while the first still has a large remainder.
    {
        jb::db::Query remaining{fixture.data.storage.database};
        REQUIRE(remaining.prepare("SELECT COUNT(*) FROM jobu_runs WHERE queue_id=:queue AND state='succeeded'"));
        REQUIRE(remaining.bind_value(":queue", uuid_to_storage(JobuScaleFixture::queue_id(0))));
        REQUIRE(remaining.exec());
        auto row = remaining.next();
        REQUIRE(row);
        REQUIRE(*row);
        CHECK(std::get<std::int64_t>(remaining.value(0)) > 0);
    }
    auto next = statistics.read(CursorRequest{*first->next_cursor}, StatisticsScope::System);
    REQUIRE(next);
    CHECK(next->window.from == first->window.from);
    CHECK(next->window.to == first->window.to);
    for (auto const& group : next->groups) {
        assert_coverage(group);
    }

    // Parent deletion remains bounded; repeated sweeps must eventually drain every finite queue.
    auto const expected = size.terminal - static_cast<std::size_t>(unlimited);
    auto const bound    = 2 + (size.terminal / 100);
    measure_scale("purge.drain", expected, [&] {
        for (std::size_t sweep = 0; purged < expected && sweep < bound; ++sweep) {
            auto const before = sweeps;
            while (sweeps == before) {
                LoopAccess::fire_next_timer(*fixture.loop.loop);
                heartbeat(fixture);
            }
        }
    });
    retention.stop();
    CHECK(fixture.data.observation.maximum_page_rows["owner.page"] <= 101);
    CHECK(fixture.data.observation.maximum_page_rows["history.page"] <= 101);
    auto const owner_visits =
        fixture.data.observation.fetches["owner.page"] - fixture.data.observation.executes["owner.page"];
    scale_measurement("purge.owner_visits", 0, owner_visits);
    scale_measurement("purge.sweeps", 0, sweeps);
    CHECK(owner_visits == 0); // Terminal definitions have no replay keys after their history retires.
    CHECK(purged == expected);
    CHECK(fixture.data.count("state='succeeded'") == unlimited);
    CHECK(fixture.data.count("state='scheduled'") == static_cast<std::int64_t>(size.pending));
    CHECK(fixture.data.scalar("SELECT COUNT(*) FROM jobu_idempotency WHERE method='job.run_now'") == 0);
    CHECK(fixture.data.scalar("SELECT COUNT(*) FROM jobu_idempotency WHERE method='job.create'") == 16);
    auto page = statistics.read(request(), StatisticsScope::System);
    REQUIRE(page);
    CHECK(page->groups[0].runs.total == size.pending + static_cast<std::size_t>(unlimited));
    assert_coverage(page->groups[0]);
    fixture.data.query_plans();
}

TEST_CASE("Finished-once cleanup seeks replay owners and skips definitions with no remaining keys",
          "[scale][retention][owner-plan]")
{
    JobuScaleFixture fixture;
    fixture.seed({.queues = 3, .pending = 4, .terminal = 600});

    // History is gone, while 604 finished definitions remain. Only the four
    // original pending definitions have creation keys worth visiting.
    fixture.execute("DELETE FROM jobu_idempotency WHERE method='job.run_now'");
    fixture.execute("DELETE FROM jobu_runs");
    fixture.execute("UPDATE jobu_jobs SET state='succeeded'");
    IdempotencyRepository records{fixture.storage.database};
    auto                  original = records.find("job.create",
                                                  JobuScaleFixture::queue_id(0),
                                                  "scale-pending-" + JobuScaleFixture::pending_id(0).to_string());
    REQUIRE(original);
    REQUIRE(*original);
    auto duplicate = **original;
    duplicate.key  = "second-creation-key";
    REQUIRE(records.insert(duplicate));

    RetentionOwnerRepository owners{fixture.storage.database};
    {
        auto transaction = jb::db::Transaction::begin(fixture.storage.database);
        REQUIRE(transaction);
        auto first = owners.list_candidates(RetentionOwnerKind::FinishedOnce, 2, std::nullopt);
        REQUIRE(first);
        REQUIRE(first->size() == 2);
        CHECK(first->at(0) == recovery_id(10'000'001));
        CHECK(first->at(1) == recovery_id(10'000'003));
        auto next = owners.list_candidates(RetentionOwnerKind::FinishedOnce, 2, first->back());
        REQUIRE(next);
        REQUIRE(next->size() == 2);
        CHECK(next->at(0) == recovery_id(10'000'005));
        CHECK(next->at(1) == recovery_id(10'000'007));
        auto tail = owners.list_candidates(RetentionOwnerKind::FinishedOnce, 2, next->back());
        REQUIRE(tail);
        CHECK(tail->empty());
        REQUIRE(transaction->commit());
    }

    // Explain the actual owner statement with real bounds. The job table must
    // be probed by UUID from each replay index, not scanned as an owner range.
    std::string sql;
    for (auto const& [statement, name] : fixture.observation.statements) {
        if (name == "owner.page" && statement.find("schedule_kind = 'once'") != std::string::npos &&
            statement.find("> :after") == std::string::npos) {
            sql = statement;
        }
    }
    REQUIRE_FALSE(sql.empty());
    jb::db::Query plan{fixture.storage.database};
    REQUIRE(plan.prepare("EXPLAIN QUERY PLAN " + sql));
    REQUIRE(plan.bind_value(":after", uuid_to_storage(Uuid{})));
    REQUIRE(plan.bind_value(":limit", std::int64_t{2}));
    REQUIRE(plan.exec());
    std::size_t job_seeks      = 0;
    bool        resource_range = false;
    bool        scope_range    = false;
    for (;;) {
        auto row = plan.next();
        REQUIRE(row);
        if (!*row) {
            break;
        }
        auto detail = read_text(plan.record(), "detail");
        REQUIRE(detail);
        INFO(*detail);
        if (detail->starts_with("SEARCH jobu_jobs ")) {
            ++job_seeks;
            CHECK(detail->find("(id=?)") != std::string::npos);
        }
        resource_range |= detail->find("(resource_id>?)") != std::string::npos;
        scope_range    |= detail->find("(scope_id>?)") != std::string::npos;
        CHECK(detail->find("TEMP B-TREE") == std::string::npos);
    }
    CHECK(job_seeks == 2);
    CHECK(resource_range);
    CHECK(scope_range);
    REQUIRE(plan.finish());

    RetentionRepository retention{fixture.storage.database, fixture.storage.registry};
    SECTION("Retiring creation keys makes every later owner pass empty")
    {
        fixture.observation.reset_counts();
        auto        cursor  = RetentionSweepCursor{.phase = RetentionSweepPhase::OnceKeys};
        std::size_t retired = 0;
        for (std::size_t page = 0; cursor.phase == RetentionSweepPhase::OnceKeys && page < 4; ++page) {
            auto batch = retention.purge_next_batch(UtcTimePoint{100s}, 10s, 2, cursor);
            REQUIRE(batch);
            retired += batch->purged.idempotency_records;
            CHECK(batch->purged.jobs == 0);
            cursor = batch->next;
        }
        CHECK(cursor.phase == RetentionSweepPhase::DeletedJobs);
        CHECK(retired == 5);
        CHECK(fixture.scalar("SELECT COUNT(*) FROM jobu_idempotency") == 0);
        for (std::size_t sweep = 0; sweep < 3; ++sweep) {
            auto batch =
                retention.purge_next_batch(UtcTimePoint{100s}, 10s, 2, {.phase = RetentionSweepPhase::OnceKeys});
            REQUIRE(batch);
            CHECK(batch->next.phase == RetentionSweepPhase::DeletedJobs);
        }
        auto const visits = fixture.observation.fetches["owner.page"] - fixture.observation.executes["owner.page"];
        CHECK(visits == 4);
        CHECK(fixture.scalar("SELECT COUNT(*) FROM jobu_jobs WHERE state='succeeded'") == 604);
    }
    SECTION("An unknown record scoped to a finished definition is still validated")
    {
        auto unknown        = duplicate;
        unknown.method      = "unknown";
        unknown.scope_id    = recovery_id(20'001'199);
        unknown.resource_id = recovery_id(99'000'000);
        REQUIRE(records.insert(unknown));
        auto batch = retention.purge_next_batch(
            UtcTimePoint{100s},
            10s,
            2,
            {.phase = RetentionSweepPhase::OnceKeys, .after_owner = recovery_id(10'000'007)});
        REQUIRE_FALSE(batch);
        CHECK(batch.error().code == "jobu.retention.invalid_relationship");
        CHECK(fixture.scalar("SELECT COUNT(*) FROM jobu_idempotency") == 6);
        CHECK(fixture.scalar("SELECT COUNT(*) FROM jobu_jobs") == 604);
    }
}

TEST_CASE("A large output cascade is atomic and its cost is outside the parent bound", "[scale][cascade]")
{
    JobuScaleFixture fixture;
    fixture.seed({.queues = 3, .pending = 3, .terminal = 3});
    AttemptRepository attempts{fixture.storage.database};
    auto const        run = JobuScaleFixture::terminal_id(0);
    // Thirty-two captures total 4 MiB. Contiguous failed attempts precede the final success.
    fixture.execute("UPDATE jobu_attempts SET outcome='failed' WHERE attempt_number=1");
    auto const result = JsonValue{.data = JsonValue::Object{}};
    auto const output =
        jb::jobu::detail::AttemptOutput{.stdout_bytes = ByteBuffer(std::size_t{128} * 1024, std::byte{0x61})};
    auto transaction = jb::db::Transaction::begin(fixture.storage.database);
    REQUIRE(transaction);
    for (AttemptNumber number = 1; number <= 32; ++number) {
        if (number > 1) {
            REQUIRE(
                attempts.insert_attempt({.run_id         = run,
                                         .attempt_number = number,
                                         .due_at         = UtcTimePoint{10s},
                                         .started_at     = UtcTimePoint{11s},
                                         .completed_at   = UtcTimePoint{12s},
                                         .state          = AttemptState::Completed,
                                         .outcome = number == 32 ? AttemptOutcome::Succeeded : AttemptOutcome::Failed,
                                         .result  = result}));
        }
        REQUIRE(attempts.insert_or_replace_output(run, number, output));
    }
    REQUIRE(transaction->commit());
    fixture.faults->faults.push_back({
        .at    = {.boundary  = "run.delete",
                  .operation = DatabaseOperation::Execute,
                  .phase     = DatabaseFaultPhase::AfterSuccess},
        .error = fault_error("db.io")
    });
    RetentionRepository retention{fixture.storage.database, fixture.storage.registry};
    REQUIRE_FALSE(retention.purge_next_batch(UtcTimePoint{100s}, 10s, 1, {}));
    CHECK(fixture.scalar("SELECT COUNT(*) FROM jobu_attempt_output") == 32);
    fixture.faults->faults.clear();
    measure_scale("purge.cascade", std::size_t{4} * 1024 * 1024, [&] {
        auto purged = retention.purge_next_batch(UtcTimePoint{100s}, 10s, 1, {});
        REQUIRE(purged);
        CHECK(purged->purged.runs == 1);
    });
    CHECK(fixture.scalar("SELECT COUNT(*) FROM jobu_attempt_output") == 0);
    CHECK(fixture.scalar("SELECT COUNT(*) FROM jobu_attempts") == 2);
}

TEST_CASE("Repeated weighted cycles give both runner families opportunities across every queue", "[scale][fairness]")
{
    ScaleRuntime fixture;
    // Fairness needs repeated capacity release, rather than a host-speed-dependent runtime assertion.
    auto         size = profile();
    size.pending      = size.queues * 10;
    size.terminal     = 0;
    fixture.data.seed(size);
    fixture.activate();
    std::set<Uuid> served;
    std::size_t    observed = 0;
    for (std::size_t round = 0; round < size.pending && served.size() < size.queues; ++round) {
        REQUIRE(fixture.core->process_cycle());
        auto const& starts = fixture.executor.start_requests();
        while (observed < starts.size()) {
            served.insert(starts[observed++].queue_id);
        }
        auto keys = fixture.executor.pending_keys();
        REQUIRE_FALSE(keys.empty());
        for (auto const& key : keys) {
            REQUIRE(fixture.executor.complete(
                key,
                {.key = key, .outcome = AttemptOutcome::Succeeded, .result = JsonValue{.data = JsonValue::Object{}}}));
        }
    }
    CHECK(served.size() == size.queues);
    std::size_t cli  = 0;
    std::size_t http = 0;
    for (auto const& start : fixture.executor.start_requests()) {
        cli  += start.type == JobType::Cli ? 1 : 0;
        http += start.type == JobType::Http ? 1 : 0;
    }
    CHECK(cli > 0);
    CHECK(http > 0);
    scale_measurement("fairness.starts_to_all_queues", 0, observed);
    fixture.stop();
}

TEST_CASE("Maintenance faults preserve prior pages and stop before any later write", "[scale][fault]")
{
    auto const   checkpoint = GENERATE(false, true);
    auto const   failure    = GENERATE(0, 1, 2, 3);
    ScaleRuntime fixture;
    fixture.data.seed({.queues = 3, .pending = 420, .terminal = 300});
    std::size_t notifications    = 0;
    std::size_t calls_at_failure = 0;
    auto        on_failure       = [&](Error const&) {
        ++notifications;
        if (failure == 3) {
            CHECK(fixture.data.storage.database.is_poisoned());
        }
        else {
            CHECK_FALSE(fixture.data.observation.transaction_open);
        }
        calls_at_failure = fixture.data.observation.commits;
    };
    std::unique_ptr<RetentionService> retention;
    if (checkpoint) {
        fixture.activate();
        fixture.open_pending();
        fixture.time.advance(2s);
        fixture.telemetry->failed.connect(&fixture.receiver, on_failure);
        LoopAccess::fire_next_timer(*fixture.loop.loop); // One page is durably acknowledged first.
    }
    else {
        retention = std::make_unique<RetentionService>(fixture.data.storage.database,
                                                       fixture.data.storage.registry,
                                                       fixture.time,
                                                       RetentionOptions{.default_retention = 10s});
        retention->failed.connect(&fixture.receiver, on_failure);
        REQUIRE(retention->start());
        LoopAccess::fire_next_timer(*fixture.loop.loop);
    }
    auto const  before = storage_snapshot(fixture.data.storage.database);
    auto const* write  = checkpoint ? "timing.write" : "run.delete";
    auto        add    = [&](std::string boundary, DatabaseOperation operation, DatabaseFaultPhase phase) {
        fixture.data.faults->faults.push_back({
            .at    = {.boundary = std::move(boundary), .operation = operation, .phase = phase},
            .error = fault_error("db.io")
        });
    };
    if (failure == 0 || failure == 3) {
        add(write, DatabaseOperation::Execute, DatabaseFaultPhase::AfterSuccess);
    }
    else {
        add("connection",
            DatabaseOperation::Commit,
            failure == 1 ? DatabaseFaultPhase::Before : DatabaseFaultPhase::AfterSuccess);
    }
    if (failure == 3) {
        add("connection", DatabaseOperation::Rollback, DatabaseFaultPhase::Before);
    }
    LoopAccess::fire_next_timer(*fixture.loop.loop);
    CHECK(notifications == 1);
    require_consumed_faults(*fixture.data.faults);
    fixture.data.faults->faults.clear();
    LoopAccess::fire_next_timer(*fixture.loop.loop);
    CHECK(fixture.data.observation.commits == calls_at_failure);
    fixture.core = nullptr;
    fixture.telemetry.reset();
    retention.reset();
    // An uncertain acknowledgement or rollback poison is inspected only after exclusive reopen.
    fixture.data.storage.reopen();
    if (failure != 2) {
        CHECK(storage_snapshot(fixture.data.storage.database) == before);
    }
    else if (checkpoint) {
        CHECK(fixture.data.scalar("SELECT COUNT(*) FROM jobu_run_timing WHERE runnable_wait_us=2000000") == 400);
    }
    else {
        CHECK(fixture.data.count("state='succeeded'") < 200);
    }
}

TEST_CASE("Paged history cleanup eventually releases a deleted queue and all its replay owners", "[scale][owners]")
{
    JobuScaleFixture fixture;
    fixture.seed({.queues = 3, .pending = 0, .terminal = 300});
    auto queue                  = recovery_queue(JobuScaleFixture::queue_id(0));
    // A creation replay is the original snapshot, before the fixture's later owner update.
    queue.updated_at            = queue.created_at;
    queue.concurrency_limit     = 1;
    queue.runnable_wait_warning = 10ms;
    queue.history_retention     = 10s;
    auto request                = encode_queue_create_idempotency_request(
        {.name = queue.name, .concurrency_limit = 1, .history_retention = 10s, .runnable_wait_warning = 10ms},
        fixture.storage.registry);
    auto replay = encode_queue_idempotency_result(queue, fixture.storage.registry);
    REQUIRE(request);
    REQUIRE(replay);
    REQUIRE(decode_queue_idempotency_result(*replay, fixture.storage.registry));
    IdempotencyRepository keys{fixture.storage.database};
    REQUIRE(keys.insert({.method       = "queue.create",
                         .key          = "scale-deleted-queue",
                         .request_json = *request,
                         .result_json  = *replay,
                         .resource_id  = queue.id,
                         .created_at   = UtcTimePoint{1s}}));
    JobRepository jobs{fixture.storage.database, fixture.storage.registry};
    RunRepository runs{fixture.storage.database, fixture.storage.registry};
    for (std::size_t index = 0; index < 100; ++index) {
        auto run = runs.find_by_id(JobuScaleFixture::terminal_id(index));
        REQUIRE(run);
        REQUIRE(run->has_value());
        auto owner = jobs.mark_deleted((**run).job_id, 1, 2, UtcTimePoint{13s});
        REQUIRE(owner);
        REQUIRE(*owner);
    }
    QueueRepository queues{fixture.storage.database, fixture.storage.registry};
    auto suspended = queues.set_state(queue.id, QueueState::Active, QueueState::Suspended, UtcTimePoint{13s});
    REQUIRE(suspended);
    REQUIRE(*suspended);
    auto deleted = queues.mark_deleted(queue.id, "deleted-" + queue.id.to_string(), queue.name, UtcTimePoint{13s});
    REQUIRE(deleted);
    REQUIRE(*deleted);
    RetentionRepository  retention{fixture.storage.database, fixture.storage.registry};
    RetentionSweepCursor cursor;
    std::size_t          deleted_jobs   = 0;
    std::size_t          deleted_queues = 0;
    for (std::size_t visit = 0; visit < 1000 && deleted_queues == 0; ++visit) {
        auto batch = retention.purge_next_batch(UtcTimePoint{100s}, 10s, 7, cursor);
        REQUIRE(batch);
        CHECK(batch->purged.runs <= 7);
        CHECK(batch->purged.jobs <= 7);
        cursor          = batch->next;
        deleted_jobs   += batch->purged.jobs;
        deleted_queues += batch->purged.queues;
    }
    CHECK(deleted_jobs == 100);
    CHECK(deleted_queues == 1);
    CHECK(fixture.scalar("SELECT COUNT(*) FROM jobu_idempotency") == 0);
    CHECK(fixture.scalar("SELECT COUNT(*) FROM jobu_queues") == 2);
    CHECK(fixture.scalar("SELECT COUNT(*) FROM pragma_foreign_key_check") == 0);
}

TEST_CASE("An explicit scale fixture can be exported for native integration", "[.][scale-seed]")
{
    auto const path = fixture_path();
    REQUIRE_FALSE(std::filesystem::exists(path));
    JobuScaleFixture fixture{path};
    auto const       size = profile();
    auto             url  = environment("JOBU_SCALE_HTTP_URL");
    fixture.seed(size, url.empty() ? "http://127.0.0.1/fixture" : url);
    FakeTimeSource time;
    time.set_utc(UtcTimePoint{100s});
    FakeCronEngine     cron;
    ScaleUuidGenerator generator;
    measure_scale("recovery.validate", size.pending + size.terminal, [&] {
        auto recovered = recover_startup(fixture.storage.database, fixture.storage.registry, cron, generator, time);
        REQUIRE(recovered);
        CHECK(recovered->repaired_timing_rows == 0);
    });
    CHECK(fixture.count("state='scheduled'") == static_cast<std::int64_t>(size.pending));
    CHECK(fixture.count("state='succeeded'") == static_cast<std::int64_t>(size.terminal));
    REQUIRE(fixture.storage.database.close());
}

TEST_CASE("Native subprocess stops at an acknowledged maintenance crash boundary", "[.][scale-crash]")
{
    ScaleRuntime fixture{fixture_path()};
    auto const   operation = environment("JOBU_SCALE_CRASH_OPERATION");
    auto const   phase     = environment("JOBU_SCALE_CRASH_PHASE");
    REQUIRE((operation == "checkpoint" || operation == "purge"));
    REQUIRE((phase == "write" || phase == "commit"));
    if (operation == "checkpoint") {
        fixture.activate();
        fixture.open_pending();
        fixture.data.execute("UPDATE jobu_run_timing SET delay_warned=1 WHERE run_id=(SELECT MIN(id) FROM jobu_runs)");
        fixture.time.advance(2s);
    }

    bool wrote                   = false;
    fixture.data.faults->observe = [&](DatabaseCall const& call) {
        fixture.data.observation.observe(call);
        if (call.phase != DatabaseFaultPhase::AfterSuccess) {
            return;
        }
        if (call.operation == DatabaseOperation::Execute &&
            call.boundary == (operation == "checkpoint" ? "timing.write" : "run.delete")) {
            wrote = true;
            if (phase != "write") {
                return;
            }
        }
        else if (!wrote || phase != "commit" || call.operation != DatabaseOperation::Commit) {
            return;
        }
        std::cout << "SCALE_MILESTONE\t" << operation << '\t' << phase << '\t' << ::getpid() << '\n';
        std::cout.flush();
        REQUIRE(std::raise(SIGSTOP) == 0);
        FAIL("Crash worker resumed instead of being killed");
    };
    if (operation == "checkpoint") {
        LoopAccess::fire_next_timer(*fixture.loop.loop);
    }
    else {
        RetentionRepository retention{fixture.data.storage.database, fixture.data.storage.registry};
        REQUIRE(retention.purge_next_batch(UtcTimePoint{100s}, 10s, 100, {}));
    }
    FAIL("Selected crash boundary was not reached");
}

TEST_CASE("Native maintenance crash reopens through production recovery", "[.][scale-restart]")
{
    JobuScaleFixture fixture{fixture_path()};
    auto const       size       = profile();
    auto const       checkpoint = environment("JOBU_SCALE_CRASH_OPERATION") == "checkpoint";
    auto const       committed  = environment("JOBU_SCALE_CRASH_PHASE") == "commit";
    FakeTimeSource   time;
    time.set_utc(UtcTimePoint{200s});
    FakeCronEngine     cron;
    ScaleUuidGenerator generator;
    auto recovery = recover_startup(fixture.storage.database, fixture.storage.registry, cron, generator, time);
    REQUIRE(recovery);
    CHECK(fixture.count("state='scheduled'") == static_cast<std::int64_t>(size.pending));
    CHECK(fixture.count("state='succeeded'") ==
          static_cast<std::int64_t>(size.terminal - (!checkpoint && committed ? 100 : 0)));
    CHECK(fixture.scalar("SELECT COUNT(*) FROM jobu_idempotency WHERE method='job.run_now'") ==
          (!checkpoint && committed ? 0 : 100));
    CHECK(fixture.scalar("SELECT COUNT(*) FROM jobu_idempotency WHERE method='job.create'") == 16);
    CHECK(fixture.scalar("SELECT COUNT(*) FROM jobu_run_timing WHERE open_epoch IS NOT NULL") == 0);
    if (checkpoint) {
        CHECK(recovery->repaired_timing_rows == size.pending);
        CHECK(fixture.scalar("SELECT COUNT(*) FROM jobu_run_timing t JOIN jobu_runs r ON r.id=t.run_id "
                             "WHERE r.state='scheduled' AND t.measurement_status='partial'") ==
              static_cast<std::int64_t>(size.pending));
        CHECK(fixture.scalar("SELECT COUNT(*) FROM jobu_run_timing WHERE runnable_wait_us=2000000") ==
              (committed ? 200 : 0));
        CHECK(fixture.scalar("SELECT COUNT(*) FROM jobu_run_timing WHERE delay_warned=1") == 1);
    }
    auto repeated = recover_startup(fixture.storage.database, fixture.storage.registry, cron, generator, time);
    REQUIRE(repeated);
    CHECK(repeated->repaired_timing_rows == 0);
    CHECK(fixture.scalar("SELECT COUNT(*) FROM pragma_foreign_key_check") == 0);
    REQUIRE(fixture.storage.database.close());
}
