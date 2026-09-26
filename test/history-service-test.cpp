#include "history_service.hpp"

#include "domain_storage_priv.hpp"
#include "management.hpp"
#include "management_json.hpp"
#include "query.hpp"
#include "support/catch_utils.hpp" // IWYU pragma: keep for Catch::StringMaker specializations
#include "support/fake_cron_engine.hpp"
#include "support/fake_time_source.hpp"
#include "support/fault_database_driver.hpp"
#include "support/recovery_fixture.hpp"
#include "support/sequence_uuid_generator.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
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
        {recovery_id(101), recovery_id(102), recovery_id(103), recovery_id(104), recovery_id(105), recovery_id(106)}
    };
    HistoryService     history{storage.database, storage.registry, tokens, time};
    std::vector<Error> failures;

    Fixture()
    {
        history.failed.connect(&history, [this](Error const& error) { failures.push_back(error); });
    }

    void execute(std::string_view sql, Uuid const& id)
    {
        Query query{storage.database};
        REQUIRE(query.prepare(sql));
        REQUIRE(query.bind_value(":id", jb::jobu::detail::uuid_to_storage(id)));
        REQUIRE(query.exec());
    }
};

auto create_job(Fixture& fixture, std::uint32_t queue_suffix, std::uint32_t job_suffix) -> JobDefinition
{
    auto queue = recovery_queue(recovery_id(queue_suffix));
    fixture.storage.insert_queue(queue);
    auto job = fixture.storage.make_job(recovery_id(job_suffix), queue.id);
    fixture.storage.insert_job(job);
    return job;
}

auto create_run(Fixture&             fixture,
                JobDefinition const& job,
                std::uint32_t        suffix,
                UtcTimePoint         planned,
                AttemptNumber        prior_failures = 0) -> RecoveryRunFixture
{
    auto run           = fixture.storage.make_run(recovery_id(suffix), job, RunState::Succeeded, prior_failures);
    run.run.planned_at = planned;
    fixture.storage.insert_run(run);
    return run;
}

} // namespace

TEST_CASE("History service rejects out-of-domain attempt numbers without closing admission", "[jobu][history][sqlite]")
{
    Fixture    fixture;
    auto const run = recovery_id(10);

    for (auto number : {AttemptNumber{0}, maximum_attempt_number + 1, std::numeric_limits<AttemptNumber>::max()}) {
        auto key     = AttemptKey{.run_id = run, .attempt_number = number};
        auto attempt = fixture.history.get_attempt(key);
        REQUIRE_FALSE(attempt);
        CHECK(attempt.error().category == ErrorCategory::InvalidArgument);
        CHECK(attempt.error().code == "jobu.history.invalid_request");

        auto output = fixture.history.read_output(AttemptOutputRequest{.attempt = key});
        REQUIRE_FALSE(output);
        CHECK(output.error().category == ErrorCategory::InvalidArgument);
        CHECK(output.error().code == "jobu.history.invalid_request");
        CHECK(fixture.failures.empty());
    }

    for (auto number : {AttemptNumber{1}, maximum_attempt_number}) {
        auto key = AttemptKey{.run_id = run, .attempt_number = number};
        CHECK(fixture.history.get_attempt(key).error().code == "jobu.attempt.not_found");
        CHECK(fixture.history.read_output(AttemptOutputRequest{.attempt = key}).error().code ==
              "jobu.attempt.not_found");
    }
    CHECK(fixture.failures.empty());
}

TEST_CASE("History pages preserve equal-time keysets and descending attempt numbers", "[jobu][history][sqlite]")
{
    Fixture fixture;
    auto    job    = create_job(fixture, 1, 2);
    auto    first  = create_run(fixture, job, 10, UtcTimePoint{10s}, 2);
    auto    second = create_run(fixture, job, 11, UtcTimePoint{10s});
    auto    older  = create_run(fixture, job, 12, UtcTimePoint{9s});

    auto page1 = fixture.history.list_runs(RunQuery{.limit = 1});
    REQUIRE(page1);
    REQUIRE(page1->items.size() == 1);
    CHECK(page1->items[0].id == second.run.id);
    REQUIRE(page1->next_cursor);

    auto page2 = fixture.history.list_runs(CursorRequest{*page1->next_cursor});
    REQUIRE(page2);
    REQUIRE(page2->items.size() == 1);
    CHECK(page2->items[0].id == first.run.id);
    REQUIRE(page2->next_cursor);

    auto retry = fixture.history.list_runs(CursorRequest{*page1->next_cursor});
    REQUIRE(retry);
    CHECK(retry->items[0].id == first.run.id);

    auto page3 = fixture.history.list_runs(CursorRequest{*page2->next_cursor});
    REQUIRE(page3);
    REQUIRE(page3->items.size() == 1);
    CHECK(page3->items[0].id == older.run.id);
    CHECK_FALSE(page3->next_cursor);

    auto attempts = fixture.history.list_attempts(AttemptQuery{.run_id = first.run.id, .limit = 1});
    REQUIRE(attempts);
    REQUIRE(attempts->items.size() == 1);
    CHECK(attempts->items[0].attempt_number == 3);
    REQUIRE(attempts->next_cursor);
    auto next_attempt = fixture.history.list_attempts(CursorRequest{*attempts->next_cursor});
    REQUIRE(next_attempt);
    CHECK(next_attempt->items[0].attempt_number == 2);
    REQUIRE(next_attempt->next_cursor);
    auto last_attempt = fixture.history.list_attempts(CursorRequest{*next_attempt->next_cursor});
    REQUIRE(last_attempt);
    CHECK(last_attempt->items[0].attempt_number == 1);
    CHECK_FALSE(last_attempt->next_cursor);

    auto details = fixture.history.get_run(first.run.id);
    REQUIRE(details);
    CHECK(details->payload == first.run.payload);
    CHECK(details->result == first.run.result);
    auto attempt = fixture.history.get_attempt({.run_id = first.run.id, .attempt_number = 3});
    REQUIRE(attempt);
    CHECK(attempt->result == first.attempts[2].attempt.result);
}

TEST_CASE("History filters use durable snapshots and half-open nullable time ranges", "[jobu][history][sqlite]")
{
    Fixture fixture;
    auto    original_job = create_job(fixture, 1, 2);
    auto    other_job    = create_job(fixture, 3, 4);
    auto    matched      = create_run(fixture, original_job, 10, UtcTimePoint{10s});
    create_run(fixture, other_job, 11, UtcTimePoint{10s});

    // The job's current queue changes; its existing run still belongs to the snapshotted queue.
    Query move{fixture.storage.database};
    REQUIRE(move.prepare("UPDATE jobu_jobs SET queue_id = :queue WHERE id = :id"));
    REQUIRE(move.bind_value(":queue", jb::jobu::detail::uuid_to_storage(other_job.queue_id)));
    REQUIRE(move.bind_value(":id", jb::jobu::detail::uuid_to_storage(original_job.id)));
    REQUIRE(move.exec());

    auto filtered = fixture.history.list_runs(RunQuery{
        .filters = {.queue_id  = original_job.queue_id,
                    .job_id    = original_job.id,
                    .state     = RunState::Succeeded,
                    .origin    = RunOrigin::Scheduled,
                    .type      = JobType::Cli,
                    .planned   = {.from = UtcTimePoint{10s}, .to = UtcTimePoint{11s}},
                    .started   = {.from = UtcTimePoint{11s}, .to = UtcTimePoint{12s}},
                    .completed = {.from = UtcTimePoint{12s}, .to = UtcTimePoint{13s}}}
    });
    REQUIRE(filtered);
    REQUIRE(filtered->items.size() == 1);
    CHECK(filtered->items[0].id == matched.run.id);
    CHECK(filtered->items[0].queue_id == original_job.queue_id);

    fixture.execute("UPDATE jobu_jobs SET state = 'deleted', deleted_at_us = 13000000 WHERE id = :id", original_job.id);
    fixture.execute("UPDATE jobu_queues SET state = 'deleted', deleted_name = name, name = 'deleted-history-owner', "
                    "deleted_at_us = 13000000 WHERE id = :id",
                    original_job.queue_id);
    auto retained = fixture.history.list_runs(RunQuery{.filters = {.queue_id = original_job.queue_id}});
    REQUIRE(retained);
    REQUIRE(retained->items.size() == 1);
    CHECK(retained->items[0].id == matched.run.id);

    auto excluded = fixture.history.list_runs(RunQuery{.filters = {.completed = {.from = UtcTimePoint{13s}}}});
    REQUIRE(excluded);
    CHECK(excluded->items.empty());

    auto pending = fixture.storage.make_run(recovery_id(12), other_job, RunState::Scheduled);
    fixture.storage.insert_run(pending);
    auto started_only = fixture.history.list_runs(RunQuery{.filters = {.started = {.from = UtcTimePoint{0s}}}});
    REQUIRE(started_only);
    CHECK(started_only->items.size() == 2);
}

TEST_CASE("History continuation observes live state while preserving its immutable boundary", "[jobu][history][sqlite]")
{
    Fixture fixture;
    auto    job    = create_job(fixture, 1, 2);
    auto    oldest = create_run(fixture, job, 10, UtcTimePoint{8s});
    auto    middle = create_run(fixture, job, 11, UtcTimePoint{9s});
    auto    newest = create_run(fixture, job, 12, UtcTimePoint{10s});

    auto first = fixture.history.list_runs(RunQuery{.filters = {.state = RunState::Succeeded}, .limit = 1});
    REQUIRE(first);
    CHECK(first->items[0].id == newest.run.id);
    REQUIRE(first->next_cursor);

    fixture.execute("UPDATE jobu_runs SET state = 'failed' WHERE id = :id", middle.run.id);
    auto continuation = fixture.history.list_runs(CursorRequest{*first->next_cursor});
    REQUIRE(continuation);
    REQUIRE(continuation->items.size() == 1);
    CHECK(continuation->items[0].id == oldest.run.id);
    CHECK_FALSE(continuation->next_cursor);
}

TEST_CASE("History ordering queries can use the version-two owner indexes", "[jobu][history][sqlite]")
{
    auto faults       = std::make_shared<DatabaseFaultState>();
    auto selected_sql = std::string{};
    faults->classify  = [&selected_sql](std::string_view sql) -> std::string {
        if (sql.starts_with("SELECT id AS run_id") &&
            sql.find("FROM jobu_runs WHERE 1 = 1") != std::string_view::npos) {
            selected_sql = sql;
        }
        return "history";
    };
    RecoveryFixture storage{[faults](std::unique_ptr<Driver> driver) {
        return std::make_unique<FaultDatabaseDriver>(std::move(driver), faults);
    }};
    auto            queue = recovery_queue(recovery_id(1));
    storage.insert_queue(queue);
    auto job = storage.make_job(recovery_id(2), queue.id);
    storage.insert_job(job);
    storage.insert_run(storage.make_run(recovery_id(10), job, RunState::Succeeded));

    FakeTimeSource        time;
    SequenceUuidGenerator tokens{{}};
    HistoryService        history{storage.database, storage.registry, tokens, time};
    for (auto const& [column, expected] : {
             std::pair{"queue_id", "jobu_runs_queue_planned_id_idx"},
             std::pair{"job_id",   "jobu_runs_job_planned_id_idx"  }
    }) {
        auto const queue_filter = std::string_view{column} == "queue_id";
        auto       listed       = history.list_runs(RunQuery{
            .filters = {.queue_id = queue_filter ? std::optional{queue.id} : std::nullopt,
                        .job_id   = queue_filter ? std::nullopt : std::optional{job.id}}
        });
        REQUIRE(listed);
        REQUIRE(listed->items.size() == 1);
        REQUIRE_FALSE(selected_sql.empty());

        Query plan{storage.database};
        REQUIRE(plan.prepare("EXPLAIN QUERY PLAN " + selected_sql));
        REQUIRE(plan.bind_value(queue_filter ? ":queue_id" : ":job_id",
                                jb::jobu::detail::uuid_to_storage(queue_filter ? queue.id : job.id)));
        REQUIRE(plan.bind_value(":limit", std::int64_t{101}));
        REQUIRE(plan.exec());
        REQUIRE(plan.next());
        auto description = jb::jobu::detail::read_text(plan.record(), "detail");
        REQUIRE(description);
        CHECK(description->find(expected) != std::string::npos);
    }
}

TEST_CASE("History read errors distinguish requests, absence, and damaged durable rows", "[jobu][history][sqlite]")
{
    Fixture fixture;
    auto    job = create_job(fixture, 1, 2);
    auto    run = create_run(fixture, job, 10, UtcTimePoint{10s});

    CHECK(fixture.history.get_run(recovery_id(99)).error().code == "jobu.run.not_found");
    CHECK(fixture.history.get_attempt({.run_id = run.run.id, .attempt_number = 99}).error().code ==
          "jobu.attempt.not_found");
    CHECK(fixture.history.get_attempt({.run_id = run.run.id, .attempt_number = 0}).error().code ==
          "jobu.history.invalid_request");
    CHECK(fixture.history.list_runs(RunQuery{.limit = 0}).error().code == "jobu.history.invalid_request");
    CHECK(fixture.history.list_runs(RunQuery{.filters = {.origin = RunOrigin::Submitted}}).error().code ==
          "jobu.history.invalid_request");
    CHECK(fixture.failures.empty());

    fixture.execute("UPDATE jobu_runs SET result_json = '[]' WHERE id = :id", run.run.id);
    auto summaries = fixture.history.list_runs(RunQuery{});
    REQUIRE(summaries);
    CHECK(summaries->items.size() == 1);

    auto damaged = fixture.history.get_run(run.run.id);
    REQUIRE_FALSE(damaged);
    CHECK(damaged.error().code == "jobu.storage.invalid_json");
    REQUIRE(fixture.failures.size() == 1);
    CHECK(fixture.failures[0].message.find("[]") == std::string::npos);
    CHECK(fixture.history.list_runs(RunQuery{}).error().code == "jobu.service.stopping");
}

TEST_CASE("A malformed summary row closes history admission", "[jobu][history][sqlite]")
{
    Fixture fixture;
    auto    job = create_job(fixture, 1, 2);
    auto    run = create_run(fixture, job, 10, UtcTimePoint{10s});

    fixture.execute("UPDATE jobu_runs SET started_at_us = NULL WHERE id = :id", run.run.id);
    auto listed = fixture.history.list_runs(RunQuery{});
    REQUIRE_FALSE(listed);
    CHECK(listed.error().code == "jobu.storage.invariant");
    REQUIRE(fixture.failures.size() == 1);
    CHECK(fixture.history.get_run(run.run.id).error().code == "jobu.service.stopping");
}

TEST_CASE("Management job lists advance within their serialized result budget", "[jobu][history][management]")
{
    Fixture fixture;
    auto    queue = recovery_queue(recovery_id(1));
    fixture.storage.insert_queue(queue);
    for (auto suffix = std::uint32_t{2}; suffix < 6; ++suffix) {
        auto job = fixture.storage.make_job(recovery_id(suffix), queue.id);
        std::get<JsonValue::Object>(job.payload.data).emplace("padding", JsonValue{.data = std::string(200000, 'x')});
        fixture.storage.insert_job(job);
    }

    FakeCronEngine        cron;
    SequenceUuidGenerator ids{{}};
    ManagementService     management{fixture.storage.database, fixture.storage.registry, cron, ids, fixture.time};
    auto                  request = JobListRequest{};
    auto                  seen    = std::vector<Uuid>{};
    do {
        auto page = management.list_jobs(request);
        REQUIRE(page);
        REQUIRE_FALSE(page->items.empty());
        auto encoded = job_page_to_json(*page, fixture.storage.registry);
        REQUIRE(encoded);
        auto serialized = serialize_json(*encoded);
        REQUIRE(serialized);
        CHECK(serialized->size() <= std::size_t{512} * 1024U);
        for (auto const& job : page->items) {
            seen.push_back(job.id);
        }
        request.page.after_id = page->next_after_id;
    } while (request.page.after_id);
    CHECK(seen == std::vector<Uuid>{recovery_id(2), recovery_id(3), recovery_id(4), recovery_id(5)});
}

TEST_CASE("History service distinguishes transient reads from fatal durable failures", "[jobu][history][failure]")
{
    auto            faults = std::make_shared<DatabaseFaultState>();
    RecoveryFixture storage{[faults](std::unique_ptr<Driver> driver) {
        return std::make_unique<FaultDatabaseDriver>(std::move(driver), faults);
    }};
    faults->classify = [](std::string_view sql) -> std::string {
        return sql.find("FROM jobu_runs") != std::string_view::npos ? "history" : "other";
    };

    FakeTimeSource        time;
    SequenceUuidGenerator tokens{{}};
    HistoryService        history{storage.database, storage.registry, tokens, time};
    auto                  failures = std::vector<Error>{};
    history.failed.connect(&history, [&failures](Error const& error) { failures.push_back(error); });

    faults->faults.push_back({
        .at    = {.boundary = "history", .operation = DatabaseOperation::Execute},
        .error = {.category = ErrorCategory::Io, .code = "db.io", .message = "backend detail"}
    });
    auto transient = history.get_run(recovery_id(20));
    REQUIRE_FALSE(transient);
    CHECK(transient.error().code == "db.io");
    CHECK(transient.error().message.find("backend detail") == std::string::npos);
    CHECK(failures.empty());
    CHECK(history.get_run(recovery_id(20)).error().code == "jobu.run.not_found");

    faults->faults.push_back({
        .at    = {.boundary = "history", .operation = DatabaseOperation::Execute},
        .error = {.category = ErrorCategory::Internal, .code = "db.corrupt", .message = "backend detail"}
    });
    auto fatal = history.get_run(recovery_id(20));
    REQUIRE_FALSE(fatal);
    CHECK(fatal.error().code == "db.corrupt");
    REQUIRE(failures.size() == 1);
    CHECK(failures[0].message.find("backend detail") == std::string::npos);
    CHECK(history.get_run(recovery_id(20)).error().code == "jobu.service.stopping");
}
