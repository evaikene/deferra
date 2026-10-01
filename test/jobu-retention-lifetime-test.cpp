#include "idempotency_repository_priv.hpp"
#include "retention_repository_priv.hpp"

#include "domain_storage_priv.hpp"
#include "management.hpp"
#include "query.hpp"
#include "run_repository_priv.hpp"
#include "secret_repository_priv.hpp"
#include "storage_failure_priv.hpp"
#include "support/catch_utils.hpp" // IWYU pragma: keep for Catch::StringMaker specializations
#include "support/fake_cron_engine.hpp"
#include "support/fake_time_source.hpp"
#include "support/fault_database_driver.hpp"
#include "support/recovery_fixture.hpp"
#include "support/sequence_uuid_generator.hpp"
#include "support/storage_fault_helpers.hpp"
#include "transaction.hpp"
#include "value.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <fmt/format.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

using namespace jb::core;
using namespace jb::db;
using namespace jb::jobu;
using namespace jb::jobu::detail;
using namespace jb::test;
using namespace std::chrono_literals;

namespace {

auto identities() -> std::vector<Uuid>
{
    auto ids = std::vector<Uuid>{};
    for (std::uint32_t i = 1; i <= 100; ++i) {
        ids.push_back(recovery_id(i));
    }
    return ids;
}

struct LifetimeFixture {
    LifetimeFixture()
    {
        time.set_utc(UtcTimePoint{10s});
        cron.set_occurrences({.expression = "* * * * *", .timezone = "UTC"}, {UtcTimePoint{100s}, UtcTimePoint{200s}});
        faults->classify = [sql = statements](std::string_view text) {
            sql->emplace_back(text);
            if (text.starts_with("SELECT method AS idempotency_method")) {
                return std::string{"replay.page"};
            }
            if (text.starts_with("SELECT id AS owner_id")) {
                return std::string{"owner.page"};
            }
            if (text.starts_with("DELETE FROM jobu_idempotency")) {
                return std::string{"replay.delete"};
            }
            if (text.starts_with("DELETE FROM jobu_jobs")) {
                return std::string{"job.delete"};
            }
            if (text.starts_with("DELETE FROM jobu_queues")) {
                return std::string{"queue.delete"};
            }
            if (text.starts_with("DELETE FROM jobu_runs")) {
                return std::string{"run.delete"};
            }
            return std::string{"other"};
        };
    }

    auto queue(std::string name = "queue") -> Queue
    {
        auto value = management.create_queue({.name = name, .history_retention = 1s, .idempotency_key = name});
        REQUIRE(value);
        return *value;
    }

    auto request(Queue const& queue, std::string key, bool recurring = false) const -> CreateJobRequest
    {
        auto payload = parse_json(R"({"command":"/inert"})");
        REQUIRE(payload);
        return {.queue    = queue.id,
                .schedule = recurring
                              ? JobCreationSchedule{CronScheduleInput{.expression = "* * * * *", .timezone = "UTC"}}
                              : JobCreationSchedule{OnceSchedule{UtcTimePoint{100s}}},
                .payload  = *payload,
                .idempotency_key = std::move(key)};
    }

    auto job(Queue const& queue, std::string key = "job", bool recurring = false) -> JobDefinition
    {
        auto value = management.create_job(request(queue, std::move(key), recurring));
        REQUIRE(value);
        return *value;
    }

    auto scheduled(JobDefinition const& job) -> JobRun
    {
        auto run = runs.find_schedule_owned(job.id);
        REQUIRE(run);
        REQUIRE(run->has_value());
        return **run;
    }

    auto manual(JobDefinition const& job, std::string key = "manual") -> JobRun
    {
        auto run = management.run_now({.job_id = job.id, .idempotency_key = std::move(key)});
        REQUIRE(run);
        return *run;
    }

    void sql(std::string_view text)
    {
        Query query{storage.database};
        REQUIRE(query.exec(text));
    }

    void complete(JobRun const& run, UtcTimePoint at = UtcTimePoint{20s})
    {
        // Model a committed runner result. The saved replay remains its original Scheduled snapshot.
        Query query{storage.database};
        REQUIRE(query.prepare("UPDATE jobu_runs SET state = 'succeeded', started_at_us = 10000000, "
                              "completed_at_us = :at, result_json = '{}' WHERE id = :id"));
        REQUIRE(query.bind_value(":at",
                                 std::chrono::duration_cast<std::chrono::microseconds>(at.time_since_epoch()).count()));
        REQUIRE(query.bind_value(":id", uuid_to_storage(run.id)));
        REQUIRE(query.exec());
    }

    void finish(JobDefinition const& job)
    {
        sql(fmt::format("UPDATE jobu_jobs SET state = 'succeeded', updated_at_us = 20000000 WHERE id = X'{}'",
                        blob_hex(job.id)));
    }

    void remove_runs(JobDefinition const& job)
    {
        sql(fmt::format("DELETE FROM jobu_runs WHERE job_id = X'{}'", blob_hex(job.id)));
    }

    void delete_job(JobDefinition const& job)
    {
        auto suspended = management.suspend_job(job.id);
        REQUIRE(suspended);
        REQUIRE(management.delete_job({.job_id = job.id, .expected_revision = suspended->revision}));
    }

    void delete_queue(Queue const& queue)
    {
        REQUIRE(management.suspend_queue(queue.id));
        REQUIRE(management.delete_queue(queue.id));
    }

    auto record(std::string_view method, Uuid scope, std::string_view key) -> IdempotencyRecord
    {
        auto found = records.find(method, scope, key);
        REQUIRE(found);
        REQUIRE(found->has_value());
        return **found;
    }

    auto visit(RetentionSweepCursor cursor = {}, UtcTimePoint now = UtcTimePoint{50s}, std::size_t limit = 100)
        -> RetentionBatchResult
    {
        auto result = retention.purge_next_batch(now, 1s, limit, cursor);
        REQUIRE(result);
        return *result;
    }

    auto sweep(UtcTimePoint now = UtcTimePoint{50s}, std::size_t limit = 100) -> RetentionPurgeCounts
    {
        auto cursor = RetentionSweepCursor{};
        auto counts = RetentionPurgeCounts{};
        for (std::size_t visits = 0; visits < 10000; ++visits) {
            auto batch                  = visit(cursor, now, limit);
            counts.runs                += batch.purged.runs;
            counts.idempotency_records += batch.purged.idempotency_records;
            counts.jobs                += batch.purged.jobs;
            counts.queues              += batch.purged.queues;
            if (batch.sweep_complete) {
                CHECK(batch.next.phase == RetentionSweepPhase::History);
                CHECK_FALSE(batch.next.after_queue);
                CHECK_FALSE(batch.next.after_owner);
                return counts;
            }
            cursor = batch.next;
        }
        FAIL("Retention sweep did not finish");
        return {};
    }

    auto settle_sweeps(UtcTimePoint now, std::size_t limit) -> RetentionPurgeCounts
    {
        // Fair history visits intentionally leave a full queue's leftovers for
        // later sweeps. Keep running until one entire sweep makes no deletions.
        auto total = RetentionPurgeCounts{};
        for (std::size_t i = 0; i < 1000; ++i) {
            auto batch                 = sweep(now, limit);
            total.runs                += batch.runs;
            total.idempotency_records += batch.idempotency_records;
            total.jobs                += batch.jobs;
            total.queues              += batch.queues;
            if (batch.runs + batch.idempotency_records + batch.jobs + batch.queues == 0) {
                return total;
            }
        }
        FAIL("Retention did not settle");
        return {};
    }

    auto count(std::string_view table) -> std::int64_t
    {
        Query query{storage.database};
        REQUIRE(query.exec("SELECT COUNT(*) AS count FROM " + std::string{table}));
        REQUIRE(query.next());
        auto const* value = query.value("count");
        REQUIRE(value);
        auto const* count = std::get_if<std::int64_t>(value);
        REQUIRE(count);
        return *count;
    }

    static auto blob_hex(Uuid const& id) -> std::string
    {
        auto value = id.to_string();
        std::erase(value, '-');
        return value;
    }

    std::shared_ptr<DatabaseFaultState>       faults{std::make_shared<DatabaseFaultState>()};
    std::shared_ptr<std::vector<std::string>> statements{std::make_shared<std::vector<std::string>>()};
    RecoveryFixture                           storage{[state = faults](std::unique_ptr<Driver> driver) {
        return std::make_unique<FaultDatabaseDriver>(std::move(driver), state);
    }};
    FakeTimeSource                            time;
    FakeCronEngine                            cron;
    SequenceUuidGenerator                     generator{identities()};
    ManagementService                         management{storage.database, storage.registry, cron, generator, time};
    RunRepository                             runs{storage.database, storage.registry};
    IdempotencyRepository                     records{storage.database};
    RetentionRepository                       retention{storage.database, storage.registry};
};

void check_relationship_failure(auto const& result)
{
    REQUIRE_FALSE(result);
    check_safe_error(result.error(), "jobu.retention.invalid_relationship");
    CHECK(classify_storage_failure(result.error(), StorageOperation::Mutation, StorageFailureOrigin::PersistedData) ==
          StorageFailureDisposition::Fatal);
}

} // anonymous namespace

TEST_CASE("Retention keeps live and suspended recurring creation and far-future once replay",
          "[jobu][retention][lifetime]")
{
    LifetimeFixture f;
    auto            queue     = f.queue();
    auto            recurring = f.job(queue, "cron", true);
    REQUIRE(f.management.suspend_job(recurring.id));
    auto request     = f.request(queue, "future");
    request.schedule = OnceSchedule{UtcTimePoint{std::chrono::hours{24 * 365}}};
    auto once        = f.management.create_job(request);
    REQUIRE(once);
    auto cron_record = f.record("job.create", queue.id, "cron");
    auto once_record = f.record("job.create", queue.id, "future");
    // The legacy nullable timestamp must never override live resource ownership.
    f.sql("UPDATE jobu_idempotency SET expires_at_us = 1");
    auto before = storage_snapshot(f.storage.database);

    CHECK(f.sweep(UtcTimePoint{std::chrono::hours{24 * 90}}).idempotency_records == 0);
    CHECK(storage_snapshot(f.storage.database) == before);
    auto cron_replay = f.management.create_job(f.request(queue, "cron", true));
    auto once_replay = f.management.create_job(request);
    auto queue_replay =
        f.management.create_queue({.name = queue.name, .history_retention = 1s, .idempotency_key = queue.name});
    REQUIRE(cron_replay);
    REQUIRE(once_replay);
    REQUIRE(queue_replay);
    CHECK(cron_replay->id == recurring.id);
    CHECK(once_replay->id == once->id);
    CHECK(queue_replay->id == queue.id);
    CHECK(f.record("job.create", queue.id, "cron").result_json == cron_record.result_json);
    CHECK(f.record("job.create", queue.id, "future").result_json == once_record.result_json);
}

TEST_CASE("Manual replay expires with its run and once creation waits for the last retained run",
          "[jobu][retention][lifetime]")
{
    LifetimeFixture f;
    auto            queue     = f.queue();
    auto            job       = f.job(queue);
    auto            scheduled = f.scheduled(job);
    auto            manual    = f.manual(job);
    f.complete(manual);
    f.complete(scheduled, UtcTimePoint{40s});
    f.finish(job);
    auto replay = f.management.run_now({.job_id = job.id, .idempotency_key = "manual"});
    REQUIRE(replay);
    CHECK(replay->id == manual.id);
    CHECK(replay->state == RunState::Scheduled);

    auto first = f.sweep(UtcTimePoint{30s}, 1);
    CHECK(first.runs == 1);
    CHECK(first.idempotency_records == 1);
    CHECK(f.record("job.create", queue.id, "job").resource_id == job.id);
    auto after_manual = f.management.run_now({.job_id = job.id, .idempotency_key = "manual"});
    REQUIRE_FALSE(after_manual);
    CHECK(after_manual.error().code == "jobu.run.manual_conflict");

    auto last = f.sweep(UtcTimePoint{50s}, 1);
    CHECK(last.runs == 1);
    CHECK(last.idempotency_records == 1);
    CHECK(last.jobs == 0);
    CHECK(f.count("jobu_jobs") == 1);
    auto kept = f.management.get_job(job.id);
    REQUIRE(kept);
    CHECK(kept->state == JobState::Succeeded);
    auto fresh = f.management.create_job(f.request(queue, "job"));
    REQUIRE(fresh);
    CHECK(fresh->id != job.id);
}

TEST_CASE("Finished definitions keep their current secret references after history and creation retirement",
          "[jobu][retention][lifetime]")
{
    LifetimeFixture  f;
    SecretRepository secrets{f.storage.database};
    REQUIRE(secrets.set("token", {}, f.time.utc_now()));
    auto queue   = f.queue();
    auto request = f.request(queue, "secret-job");
    auto payload = parse_json(R"({"command":"/inert","arguments":[{"secret":"token"}]})");
    REQUIRE(payload);
    request.payload = *payload;
    auto job        = f.management.create_job(request);
    REQUIRE(job);
    f.complete(f.scheduled(*job));
    f.finish(*job);
    auto counts = f.sweep();
    CHECK(counts.runs == 1);
    CHECK(counts.idempotency_records == 1);
    CHECK(counts.jobs == 0);
    CHECK(*secrets.reference_count("token") == 1);
}

TEST_CASE("Deleted queue cleanup reaches once cron manual and moved historical ownership",
          "[jobu][retention][lifetime]")
{
    LifetimeFixture f;
    auto            old_queue = f.queue("old");
    auto            new_queue = f.queue("new");
    f.job(old_queue, "once");
    auto cron   = f.job(old_queue, "cron", true);
    auto manual = f.manual(cron);
    f.complete(manual);
    auto moved = f.job(old_queue, "moved", true);
    f.complete(f.manual(moved, "moved-manual"));
    auto suspended = f.management.suspend_job(moved.id);
    REQUIRE(suspended);
    auto moved_result = f.management.move_job(
        {.job_id = moved.id, .expected_revision = suspended->revision, .target_queue = new_queue.id});
    REQUIRE(moved_result);
    f.delete_queue(old_queue);

    auto first = f.settle_sweeps(UtcTimePoint{50s}, 1);
    CHECK(first.jobs == 2);
    CHECK(first.queues == 0);
    CHECK(f.record("job.create", old_queue.id, "moved").resource_id == moved.id);
    CHECK(f.count("jobu_queues") == 2);
    CHECK(f.count("jobu_jobs") == 1);
    CHECK(f.count("jobu_runs") == 1);
    REQUIRE(f.management.delete_job({.job_id = moved.id, .expected_revision = moved_result->revision}));

    auto final = f.settle_sweeps(UtcTimePoint{50s}, 1);
    CHECK(final.jobs == 1);
    CHECK(final.queues == 1);
    CHECK(f.count("jobu_jobs") == 0);
    CHECK(f.count("jobu_runs") == 0);
    CHECK(f.count("jobu_idempotency") == 1); // Only the live target queue's creation record remains.
    CHECK(f.record("queue.create", {}, "new").resource_id == new_queue.id);
    auto reused = f.queue("old");
    CHECK(reused.id != old_queue.id);
}

TEST_CASE("Unrelated scoped replay records survive deleted owner retirement", "[jobu][retention][lifetime]")
{
    LifetimeFixture f;
    auto            first    = f.queue("first");
    auto            second   = f.queue("second");
    auto            removed  = f.job(first, "shared");
    auto            retained = f.job(second, "shared");
    auto            original = f.record("job.create", second.id, "shared");
    f.delete_job(removed);
    auto counts = f.sweep();
    CHECK(counts.jobs == 1);
    CHECK(f.record("job.create", second.id, "shared").result_json == original.result_json);
    CHECK(f.record("job.create", second.id, "shared").resource_id == retained.id);
    CHECK(f.count("jobu_idempotency") == 3);
}

TEST_CASE("Unknown and inconsistent manual replay relationships fail before any run deletion",
          "[jobu][retention][fault]")
{
    auto const* mutation =
        GENERATE("UPDATE jobu_idempotency SET method = 'unknown' WHERE key = 'manual'",
                 "UPDATE jobu_idempotency SET scope_id = zeroblob(16) WHERE key = 'manual'",
                 "UPDATE jobu_idempotency SET request_json = '{}' WHERE key = 'manual'",
                 "UPDATE jobu_idempotency SET result_json = '{}' WHERE key = 'manual'",
                 "UPDATE jobu_runs SET origin = 'scheduled', schedule_owned = 1 WHERE origin = 'manual'");
    LifetimeFixture f;
    auto            queue  = f.queue();
    auto            job    = f.job(queue);
    auto            manual = f.manual(job);
    f.complete(manual);
    f.sql(mutation);
    auto before = storage_snapshot(f.storage.database);
    auto result = f.retention.purge_next_batch(UtcTimePoint{50s}, 1s, 100, {});
    check_relationship_failure(result);
    CHECK(storage_snapshot(f.storage.database) == before);
}

TEST_CASE("Creation retirement checks the request replay and original scope after moves", "[jobu][retention][fault]")
{
    LifetimeFixture f;
    auto            queue     = f.queue();
    auto            target    = f.queue("target");
    auto            job       = f.job(queue);
    auto            suspended = f.management.suspend_job(job.id);
    REQUIRE(suspended);
    REQUIRE(
        f.management.move_job({.job_id = job.id, .expected_revision = suspended->revision, .target_queue = target.id}));
    f.complete(f.scheduled(job));
    f.finish(job);
    f.remove_runs(job);

    SECTION("valid original creation scope survives a current queue change")
    {
        CHECK(f.visit({.phase = RetentionSweepPhase::OnceKeys}).purged.idempotency_records == 1);
        CHECK(f.count("jobu_jobs") == 1);
    }
    SECTION("changing scope to the current queue is corruption")
    {
        f.sql(fmt::format("UPDATE jobu_idempotency SET scope_id = X'{}' WHERE key = 'job'",
                          LifetimeFixture::blob_hex(target.id)));
        auto before = storage_snapshot(f.storage.database);
        auto result =
            f.retention.purge_next_batch(UtcTimePoint{50s}, 1s, 100, {.phase = RetentionSweepPhase::OnceKeys});
        check_relationship_failure(result);
        CHECK(storage_snapshot(f.storage.database) == before);
    }
}

TEST_CASE("Deleted owner preflight refuses unexpected secret references instead of cascading them",
          "[jobu][retention][fault]")
{
    LifetimeFixture f;
    auto            job = f.job(f.queue());
    f.delete_job(job);
    f.remove_runs(job);
    SecretRepository secrets{f.storage.database};
    REQUIRE(secrets.set("unexpected", {}, f.time.utc_now()));
    f.sql(fmt::format(
        "INSERT INTO jobu_secret_refs(secret_name, job_id, field_path) VALUES('unexpected', X'{}', 'arguments[0]')",
        LifetimeFixture::blob_hex(job.id)));
    auto before = storage_snapshot(f.storage.database);
    auto result = f.retention.purge_next_batch(UtcTimePoint{50s}, 1s, 100, {.phase = RetentionSweepPhase::DeletedJobs});
    check_relationship_failure(result);
    CHECK(storage_snapshot(f.storage.database) == before);
}

TEST_CASE("Owner retirement pages references without splitting an atomic lifetime group",
          "[jobu][retention][pagination]")
{
    LifetimeFixture f;
    auto            queue    = f.queue();
    auto            job      = f.job(queue);
    auto            original = f.record("job.create", queue.id, "job");
    for (std::size_t i = 0; i < 35; ++i) {
        auto copy = original;
        copy.key  = fmt::format("copy-{:02}", i);
        REQUIRE(f.records.insert(copy));
    }
    f.delete_job(job);
    f.remove_runs(job);
    auto batch = f.visit({.phase = RetentionSweepPhase::DeletedJobs}, UtcTimePoint{50s}, 1);
    CHECK(batch.purged.jobs == 1);
    CHECK(batch.purged.idempotency_records == 36);
    CHECK(batch.next.after_owner == job.id);
    CHECK(f.visit(batch.next).next.phase == RetentionSweepPhase::DeletedQueues);
    CHECK(f.count("jobu_idempotency") == 1);
}

TEST_CASE("Every multi-delete retention phase rolls back after replay or resource writes", "[jobu][retention][fault]")
{
    auto            phase      = GENERATE(RetentionSweepPhase::History,
                                          RetentionSweepPhase::OnceKeys,
                                          RetentionSweepPhase::DeletedJobs,
                                          RetentionSweepPhase::DeletedQueues);
    auto            fault_kind = GENERATE(0, 1, 2, 3);
    LifetimeFixture f;
    auto            queue           = f.queue();
    auto            cursor          = RetentionSweepCursor{.phase = phase};
    auto            parent_boundary = std::string{"replay.delete"};
    if (phase == RetentionSweepPhase::DeletedQueues) {
        f.delete_queue(queue);
        parent_boundary = "queue.delete";
    }
    else {
        auto job = f.job(queue);
        if (phase == RetentionSweepPhase::History) {
            f.complete(f.manual(job));
            parent_boundary = "run.delete";
        }
        else if (phase == RetentionSweepPhase::DeletedJobs) {
            f.delete_job(job);
            f.remove_runs(job);
            parent_boundary = "job.delete";
        }
        else {
            f.complete(f.scheduled(job));
            f.finish(job);
            f.remove_runs(job);
        }
    }
    auto fault = DatabaseCall{.boundary = "replay.page", .operation = DatabaseOperation::Finish};
    if (fault_kind == 1) {
        fault = {.boundary  = "replay.delete",
                 .operation = DatabaseOperation::Execute,
                 .phase     = DatabaseFaultPhase::AfterSuccess};
    }
    else if (fault_kind == 2) {
        fault = {.boundary  = parent_boundary,
                 .operation = DatabaseOperation::Execute,
                 .phase     = DatabaseFaultPhase::AfterSuccess};
    }
    else if (fault_kind == 3) {
        fault = {.boundary = "connection", .operation = DatabaseOperation::Commit};
    }
    auto before = storage_snapshot(f.storage.database);
    f.faults->faults.push_back({.at = fault, .error = fault_error()});
    auto result = f.retention.purge_next_batch(UtcTimePoint{50s}, 1s, 100, cursor);
    REQUIRE_FALSE(result);
    check_safe_error(result.error(), "db.io");
    require_consumed_faults(*f.faults);
    CHECK(storage_snapshot(f.storage.database) == before);
    f.visit(cursor); // Cleanup returned with no active query/transaction and the same unit can succeed.
}

TEST_CASE("Owner deletion affected-count mismatch restores its retired replay keys", "[jobu][retention][fault]")
{
    auto            phase = GENERATE(RetentionSweepPhase::DeletedJobs, RetentionSweepPhase::DeletedQueues);
    LifetimeFixture f;
    auto            queue = f.queue();
    if (phase == RetentionSweepPhase::DeletedJobs) {
        auto job = f.job(queue);
        f.delete_job(job);
        f.remove_runs(job);
        f.sql("CREATE TRIGGER skip_owner BEFORE DELETE ON jobu_jobs BEGIN SELECT RAISE(IGNORE); END");
    }
    else {
        f.delete_queue(queue);
        f.sql("CREATE TRIGGER skip_owner BEFORE DELETE ON jobu_queues BEGIN SELECT RAISE(IGNORE); END");
    }
    auto before = storage_snapshot(f.storage.database);
    auto result = f.retention.purge_next_batch(UtcTimePoint{50s}, 1s, 100, {.phase = phase});
    check_relationship_failure(result);
    CHECK(storage_snapshot(f.storage.database) == before);
}

TEST_CASE("Skipped replay deletion rolls back its terminal run", "[jobu][retention][fault]")
{
    LifetimeFixture f;
    auto            job = f.job(f.queue());
    f.complete(f.manual(job));
    f.sql("CREATE TRIGGER skip_replay BEFORE DELETE ON jobu_idempotency BEGIN SELECT RAISE(IGNORE); END");
    auto before = storage_snapshot(f.storage.database);
    auto result = f.retention.purge_next_batch(UtcTimePoint{50s}, 1s, 100, {});
    check_relationship_failure(result);
    CHECK(storage_snapshot(f.storage.database) == before);
}

TEST_CASE("Owner scope guards reject dangling and unknown references without retiring creation",
          "[jobu][retention][fault]")
{
    LifetimeFixture f;
    auto            queue = f.queue();
    auto            job   = f.job(queue);
    SECTION("a manual record scoped to a deleted job cannot silently pin a missing run")
    {
        f.manual(job);
        f.delete_job(job);
        f.remove_runs(job);
    }
    SECTION("an unknown record scoped to an otherwise purgeable queue cannot be ignored")
    {
        auto record        = f.record("job.create", queue.id, "job");
        record.method      = "unknown";
        record.resource_id = recovery_id(999);
        REQUIRE(f.records.insert(record));
        f.delete_job(job);
        f.remove_runs(job);
        REQUIRE(f.visit({.phase = RetentionSweepPhase::DeletedJobs}).purged.jobs == 1);
        f.delete_queue(queue);
    }
    auto phase  = f.count("jobu_jobs") == 0 ? RetentionSweepPhase::DeletedQueues : RetentionSweepPhase::DeletedJobs;
    auto before = storage_snapshot(f.storage.database);
    auto result = f.retention.purge_next_batch(UtcTimePoint{50s}, 1s, 100, {.phase = phase});
    check_relationship_failure(result);
    CHECK(storage_snapshot(f.storage.database) == before);
}

TEST_CASE("Replay retirement retains uncertain commit and poisoned rollback semantics in every phase",
          "[jobu][retention][fault]")
{
    auto            phase = GENERATE(RetentionSweepPhase::History,
                                     RetentionSweepPhase::OnceKeys,
                                     RetentionSweepPhase::DeletedJobs,
                                     RetentionSweepPhase::DeletedQueues);
    LifetimeFixture f;
    auto            queue = f.queue();
    if (phase == RetentionSweepPhase::DeletedQueues) {
        f.delete_queue(queue);
    }
    else {
        auto job = f.job(queue);
        if (phase == RetentionSweepPhase::History) {
            f.complete(f.manual(job));
        }
        else if (phase == RetentionSweepPhase::DeletedJobs) {
            f.delete_job(job);
            f.remove_runs(job);
        }
        else {
            f.complete(f.scheduled(job));
            f.finish(job);
            f.remove_runs(job);
        }
    }
    auto before = storage_snapshot(f.storage.database);
    SECTION("durable commit with a lost acknowledgement does not leave a dangling replay")
    {
        f.faults->faults.push_back({
            .at    = {.boundary  = "connection",
                      .operation = DatabaseOperation::Commit,
                      .phase     = DatabaseFaultPhase::AfterSuccess},
            .error = fault_error()
        });
        auto result = f.retention.purge_next_batch(UtcTimePoint{50s}, 1s, 100, {.phase = phase});
        REQUIRE_FALSE(result);
        check_safe_error(result.error(), "db.io");
        CHECK(classify_storage_failure(result.error(), StorageOperation::Mutation) == StorageFailureDisposition::Fatal);
        require_consumed_faults(*f.faults);
        f.storage.reopen();
        CHECK(storage_snapshot(f.storage.database) != before);
        CHECK(f.count("jobu_idempotency") == (phase == RetentionSweepPhase::History         ? 2
                                              : phase == RetentionSweepPhase::DeletedQueues ? 0
                                                                                            : 1));
        if (phase == RetentionSweepPhase::History) {
            CHECK(f.count("jobu_runs") == 1);
        }
        else if (phase == RetentionSweepPhase::DeletedJobs) {
            CHECK(f.count("jobu_jobs") == 0);
        }
        else if (phase == RetentionSweepPhase::DeletedQueues) {
            CHECK(f.count("jobu_queues") == 0);
        }
    }
    SECTION("failed rollback poisons the connection and the reopened database restores the complete unit")
    {
        f.faults->faults.push_back({
            .at    = {.boundary  = "replay.delete",
                      .operation = DatabaseOperation::Execute,
                      .phase     = DatabaseFaultPhase::AfterSuccess},
            .error = fault_error()
        });
        f.faults->faults.push_back({
            .at    = {.boundary = "connection", .operation = DatabaseOperation::Rollback},
            .error = fault_error()
        });
        auto result = f.retention.purge_next_batch(UtcTimePoint{50s}, 1s, 100, {.phase = phase});
        REQUIRE_FALSE(result);
        check_safe_error(result.error(), "db.io");
        require_consumed_faults(*f.faults);
        auto poisoned = Transaction::begin(f.storage.database);
        REQUIRE_FALSE(poisoned);
        CHECK(poisoned.error().code == "db.connection_failed");
        f.storage.reopen();
        CHECK(storage_snapshot(f.storage.database) == before);
    }
}

TEST_CASE("Owner pages honor maximum bounds and revisit candidates inserted behind the cursor",
          "[jobu][retention][pagination]")
{
    LifetimeFixture f;
    auto            queue = f.queue();
    for (std::uint32_t i = 0; i < 1001; ++i) {
        auto job       = f.storage.make_job(recovery_id(1000 + i), queue.id);
        job.state      = JobState::Deleted;
        job.deleted_at = UtcTimePoint{20s};
        job.updated_at = *job.deleted_at;
        f.storage.insert_job(job);
    }
    auto first = f.visit({.phase = RetentionSweepPhase::DeletedJobs}, UtcTimePoint{50s}, 1000);
    CHECK(first.purged.jobs == 1000);
    CHECK(first.next.after_owner == recovery_id(1999));
    auto late       = f.storage.make_job(recovery_id(999), queue.id);
    late.state      = JobState::Deleted;
    late.deleted_at = UtcTimePoint{20s};
    late.updated_at = *late.deleted_at;
    f.storage.insert_job(late);

    auto second = f.visit(first.next, UtcTimePoint{50s}, 1000);
    CHECK(second.purged.jobs == 1);
    CHECK(second.next.phase == RetentionSweepPhase::DeletedQueues);
    CHECK(f.count("jobu_jobs") == 1);
    CHECK(f.sweep().jobs == 1);
}

TEST_CASE("Empty owner page finish failures do not advance the sweep", "[jobu][retention][fault]")
{
    auto phase =
        GENERATE(RetentionSweepPhase::OnceKeys, RetentionSweepPhase::DeletedJobs, RetentionSweepPhase::DeletedQueues);
    LifetimeFixture f;
    f.faults->faults.push_back({
        .at    = {.boundary = "owner.page", .operation = DatabaseOperation::Finish},
        .error = fault_error()
    });
    auto result = f.retention.purge_next_batch(UtcTimePoint{50s}, 1s, 100, {.phase = phase});
    REQUIRE_FALSE(result);
    check_safe_error(result.error(), "db.io");
    require_consumed_faults(*f.faults);
    CHECK(f.count("jobu_idempotency") == 0);
    auto retry = f.visit({.phase = phase});
    CHECK(retry.sweep_complete == (phase == RetentionSweepPhase::DeletedQueues));
}

TEST_CASE("Reference and owner cleanup plans use the existing ownership indexes", "[jobu][retention][plan]")
{
    LifetimeFixture f;
    auto            queue  = f.queue();
    auto            job    = f.job(queue);
    auto            record = f.record("job.create", queue.id, "job");
    for (std::size_t i = 0; i < 17; ++i) {
        record.key = fmt::format("copy-{:02}", i);
        REQUIRE(f.records.insert(record));
    }
    f.delete_job(job);
    f.remove_runs(job);
    f.statements->clear();
    auto batch = f.visit({.phase = RetentionSweepPhase::DeletedJobs}, UtcTimePoint{50s}, 1);
    REQUIRE(batch.purged.jobs == 1);
    f.visit(batch.next);
    f.delete_queue(queue);
    f.visit({.phase = RetentionSweepPhase::DeletedQueues});
    auto queries         = *f.statements;
    auto seen            = std::vector<std::string>{};
    auto reference_pages = std::size_t{0};
    for (auto const& sql : queries) {
        bool const reference = sql.starts_with("SELECT method AS idempotency_method");
        bool const owner     = sql.starts_with("SELECT id AS owner_id");
        bool const deletion  = sql.starts_with("DELETE FROM jobu_jobs") || sql.starts_with("DELETE FROM jobu_queues");
        if ((!reference && !owner && !deletion) || std::ranges::find(seen, sql) != seen.end()) {
            continue;
        }
        seen.push_back(sql);
        if (owner || deletion) {
            CHECK(sql.find("payload_json") == std::string::npos);
            CHECK(sql.find("attributes_json") == std::string::npos);
        }
        Query plan{f.storage.database};
        REQUIRE(plan.prepare("EXPLAIN QUERY PLAN " + sql));
        if (reference) {
            ++reference_pages;
            REQUIRE(plan.bind_value(":owner", uuid_to_storage(job.id)));
            REQUIRE(plan.bind_value(":limit", std::int64_t{16}));
            if (sql.find(":after_method") != std::string::npos) {
                REQUIRE(plan.bind_value(":after_method", make_text("job.create")));
                REQUIRE(plan.bind_value(":after_scope", uuid_to_storage(queue.id)));
                REQUIRE(plan.bind_value(":after_key", make_text("copy-15")));
            }
        }
        else if (owner) {
            REQUIRE(plan.bind_value(":limit", std::int64_t{1}));
            if (sql.find(":after") != std::string::npos) {
                REQUIRE(plan.bind_value(":after", uuid_to_storage(job.id)));
            }
        }
        else {
            REQUIRE(plan.bind_value(":id", uuid_to_storage(job.id)));
        }
        REQUIRE(plan.exec());
        auto details = std::string{};
        for (;;) {
            auto next = plan.next();
            REQUIRE(next);
            if (!*next) {
                break;
            }
            auto const* detail = std::get_if<std::string>(plan.value("detail"));
            REQUIRE(detail);
            details += *detail + "\n";
        }
        if (reference || deletion) {
            CHECK(details.find("jobu_idempotency_resource_id_idx") != std::string::npos);
            CHECK(details.find("jobu_idempotency_scope_method_resource_idx") != std::string::npos);
        }
        if (reference) {
            CHECK(details.find("SCAN jobu_idempotency") == std::string::npos);
        }
        if (owner) {
            CHECK(details.find("TEMP B-TREE") == std::string::npos);
        }
        auto kind = std::string_view{"owner delete"};
        if (reference) {
            kind = "reference";
        }
        else if (owner) {
            kind = "owner page";
        }
        std::cout << "Retention cleanup plan (" << kind << "):\n" << details;
    }
    CHECK(reference_pages == 2);
}
