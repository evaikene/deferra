#include "history_service.hpp"

#include "domain_storage_priv.hpp"
#include "history_cursor_priv.hpp"
#include "history_json.hpp"
#include "history_repository_priv.hpp"
#include "history_run_page_priv.hpp"
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

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
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

auto create_state_run(Fixture&             fixture,
                      JobDefinition const& job,
                      std::uint32_t        suffix,
                      RunState             state,
                      UtcTimePoint         planned) -> RecoveryRunFixture
{
    // Manual occurrences allow several independent nonterminal runs for one definition.
    auto prior_failures = state == RunState::RetryWait ? AttemptNumber{1} : AttemptNumber{0};
    auto run            = fixture.storage.make_run(recovery_id(suffix), job, state, prior_failures, RunOrigin::Manual);
    run.run.planned_at  = planned;
    fixture.storage.insert_run(run);
    return run;
}

auto run_ids(RunPage const& page) -> std::vector<Uuid>
{
    auto ids = std::vector<Uuid>{};
    for (auto const& run : page.items) {
        ids.push_back(run.id);
    }
    return ids;
}

auto page_bytes(RunPage const& page) -> std::size_t
{
    auto encoded = run_page_to_json(page);
    REQUIRE(encoded);
    auto serialized = serialize_json(*encoded);
    REQUIRE(serialized);
    return serialized->size();
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

TEST_CASE("History state sets select every state without changing scalar or unfiltered queries",
          "[jobu][history][sqlite]")
{
    Fixture    fixture;
    auto       job    = create_job(fixture, 1, 2);
    auto const states = std::vector{RunState::Scheduled,
                                    RunState::Running,
                                    RunState::RetryWait,
                                    RunState::Succeeded,
                                    RunState::Failed,
                                    RunState::Interrupted,
                                    RunState::Cancelled};
    for (auto index = std::size_t{0}; index < states.size(); ++index) {
        auto suffix = std::uint32_t{10} + static_cast<std::uint32_t>(index);
        create_state_run(fixture, job, suffix, states[index], UtcTimePoint{10s});

        auto scalar    = fixture.history.list_runs(RunQuery{.filters = {.state = states[index]}});
        auto singleton = fixture.history.list_runs(RunQuery{.filters = {.states = std::vector{states[index]}}});
        REQUIRE(scalar);
        REQUIRE(singleton);
        CHECK(run_ids(*scalar) == std::vector{recovery_id(suffix)});
        CHECK(run_ids(*singleton) == run_ids(*scalar));
    }

    auto unfiltered = fixture.history.list_runs(RunQuery{});
    auto all_states = fixture.history.list_runs(RunQuery{.filters = {.states = states}});
    REQUIRE(unfiltered);
    REQUIRE(all_states);
    CHECK(run_ids(*unfiltered) == std::vector{recovery_id(16),
                                              recovery_id(15),
                                              recovery_id(14),
                                              recovery_id(13),
                                              recovery_id(12),
                                              recovery_id(11),
                                              recovery_id(10)});
    CHECK(run_ids(*all_states) == run_ids(*unfiltered));

    auto reversed_states = states;
    std::ranges::reverse(reversed_states);
    auto reversed = fixture.history.list_runs(RunQuery{.filters = {.states = std::move(reversed_states)}});
    REQUIRE(reversed);
    CHECK(run_ids(*reversed) == run_ids(*all_states));

    auto started   = fixture.history.list_runs(RunQuery{
        .filters = {.states = states, .started = {.from = UtcTimePoint{0s}}}
    });
    auto completed = fixture.history.list_runs(RunQuery{
        .filters = {.states = states, .completed = {.from = UtcTimePoint{12s}, .to = UtcTimePoint{13s}}}
    });
    REQUIRE(started);
    REQUIRE(completed);
    CHECK(run_ids(*started) ==
          std::vector{recovery_id(15), recovery_id(14), recovery_id(13), recovery_id(12), recovery_id(11)});
    CHECK(run_ids(*completed) == std::vector{recovery_id(16), recovery_id(15), recovery_id(14), recovery_id(13)});
}

TEST_CASE("History state sets filter interleaved rows before every small page", "[jobu][history][sqlite]")
{
    Fixture    fixture;
    auto       job  = create_job(fixture, 1, 2);
    auto const rows = std::array{
        std::pair{RunState::Succeeded,   8s },
        std::pair{RunState::Scheduled,   9s },
        std::pair{RunState::Failed,      9s },
        std::pair{RunState::Interrupted, 10s},
        std::pair{RunState::Running,     10s},
        std::pair{RunState::Succeeded,   10s},
        std::pair{RunState::RetryWait,   11s},
        std::pair{RunState::Cancelled,   11s},
        std::pair{RunState::Scheduled,   12s}
    };
    auto suffix = std::uint32_t{10};
    for (auto const& [state, planned] : rows) {
        create_state_run(fixture, job, suffix++, state, UtcTimePoint{planned});
    }

    auto request = RunListRequest{
        RunQuery{.filters = {.states = std::vector{RunState::Cancelled,
                                                   RunState::Failed,
                                                   RunState::Succeeded,
                                                   RunState::Interrupted}},
                 .limit   = 2}
    };
    auto const expected_pages = std::vector{
        std::vector{recovery_id(17), recovery_id(15)},
        std::vector{recovery_id(13), recovery_id(12)},
        std::vector{recovery_id(10)}
    };
    auto first_cursor = std::optional<std::string>{};
    for (auto index = std::size_t{0}; index < expected_pages.size(); ++index) {
        auto page = fixture.history.list_runs(request);
        REQUIRE(page);
        CHECK(run_ids(*page) == expected_pages[index]);
        CHECK(page_bytes(*page) <= std::size_t{512} * 1024U);
        CHECK(page->next_cursor.has_value() == (index + 1U < expected_pages.size()));
        if (page->next_cursor) {
            if (!first_cursor) {
                first_cursor = page->next_cursor;
            }
            request = CursorRequest{*page->next_cursor};
        }
    }

    REQUIRE(first_cursor);
    auto replay = fixture.history.list_runs(CursorRequest{*first_cursor});
    REQUIRE(replay);
    CHECK(run_ids(*replay) == expected_pages[1]);

    auto empty = fixture.history.list_runs(RunQuery{
        .filters = {.states = std::vector{RunState::Failed}, .planned = {.from = UtcTimePoint{20s}}}
    });
    REQUIRE(empty);
    CHECK(empty->items.empty());
    CHECK_FALSE(empty->next_cursor);
}

TEST_CASE("History state sets intersect snapshot owners, type, origin, and every time range", "[jobu][history][sqlite]")
{
    Fixture fixture;
    auto    job       = create_job(fixture, 1, 2);
    auto    other_job = create_job(fixture, 3, 4);
    auto    http_job  = fixture.storage.make_job(recovery_id(5), job.queue_id, JobType::Http);
    fixture.storage.insert_job(http_job);

    auto suffix = std::uint32_t{10};
    for (auto state : {RunState::Failed, RunState::Interrupted}) {
        fixture.storage.insert_run(fixture.storage.make_run(recovery_id(suffix++), job, state, 0, RunOrigin::Manual));
    }

    // Each near miss changes one captured dimension, independently of the job's current definition.
    for (suffix = 12; suffix <= 18; ++suffix) {
        auto run = fixture.storage.make_run(recovery_id(suffix), job, RunState::Failed, 0, RunOrigin::Manual);
        switch (suffix) {
            case 12:
                run.run.queue_id = other_job.queue_id;
                break;
            case 13:
                run.run.job_id = other_job.id;
                break;
            case 14:
                run.run.type    = JobType::Http;
                run.run.payload = http_job.payload;
                break;
            case 15:
                run.run.origin         = RunOrigin::Scheduled;
                run.run.schedule_owned = true;
                break;
            case 16:
                run.run.planned_at = UtcTimePoint{9s};
                break;
            case 17:
                run.run.started_at = UtcTimePoint{10s};
                break;
            case 18:
                run.run.completed_at = UtcTimePoint{13s};
                break;
            default:
                FAIL("Unexpected near-miss fixture");
        }
        fixture.storage.insert_run(run);
    }

    auto query = RunQuery{
        .filters = {.queue_id  = job.queue_id,
                    .job_id    = job.id,
                    .states    = std::vector{RunState::Interrupted, RunState::Failed},
                    .origin    = RunOrigin::Manual,
                    .type      = JobType::Cli,
                    .planned   = {.from = UtcTimePoint{10s}, .to = UtcTimePoint{11s}},
                    .started   = {.from = UtcTimePoint{11s}, .to = UtcTimePoint{12s}},
                    .completed = {.from = UtcTimePoint{12s}, .to = UtcTimePoint{13s}}},
        .limit   = 1
    };
    auto first = fixture.history.list_runs(query);
    REQUIRE(first);
    CHECK(run_ids(*first) == std::vector{recovery_id(11)});
    REQUIRE(first->next_cursor);

    // Moving and deleting the live definition cannot retarget the retained run snapshots or saved query.
    Query move{fixture.storage.database};
    REQUIRE(move.prepare("UPDATE jobu_jobs SET queue_id = :queue WHERE id = :id"));
    REQUIRE(move.bind_value(":queue", jb::jobu::detail::uuid_to_storage(other_job.queue_id)));
    REQUIRE(move.bind_value(":id", jb::jobu::detail::uuid_to_storage(job.id)));
    REQUIRE(move.exec());
    fixture.execute("UPDATE jobu_jobs SET state = 'deleted', deleted_at_us = 13000000 WHERE id = :id", job.id);

    auto second = fixture.history.list_runs(CursorRequest{*first->next_cursor});
    REQUIRE(second);
    CHECK(run_ids(*second) == std::vector{recovery_id(10)});
    CHECK_FALSE(second->next_cursor);
}

TEST_CASE("History state-set continuation observes live changes only below its boundary", "[jobu][history][sqlite]")
{
    Fixture fixture;
    auto    job = create_job(fixture, 1, 2);
    create_state_run(fixture, job, 10, RunState::Failed, UtcTimePoint{6s});
    create_state_run(fixture, job, 11, RunState::Failed, UtcTimePoint{7s});
    create_state_run(fixture, job, 12, RunState::Running, UtcTimePoint{8s});
    create_state_run(fixture, job, 13, RunState::Failed, UtcTimePoint{9s});
    create_state_run(fixture, job, 14, RunState::Running, UtcTimePoint{10s});

    auto first = fixture.history.list_runs(
        RunQuery{.filters = {.states = std::vector{RunState::Failed, RunState::Interrupted}}, .limit = 1});
    REQUIRE(first);
    CHECK(run_ids(*first) == std::vector{recovery_id(13)});
    REQUIRE(first->next_cursor);

    fixture.execute("UPDATE jobu_runs SET state = 'succeeded' WHERE id = :id", recovery_id(11));
    fixture.execute("UPDATE jobu_runs SET state = 'interrupted', completed_at_us = 12000000 WHERE id = :id",
                    recovery_id(12));
    fixture.execute("UPDATE jobu_runs SET state = 'failed', completed_at_us = 12000000 WHERE id = :id",
                    recovery_id(14));

    auto second = fixture.history.list_runs(CursorRequest{*first->next_cursor});
    REQUIRE(second);
    CHECK(run_ids(*second) == std::vector{recovery_id(12)});
    REQUIRE(second->next_cursor);
    auto third = fixture.history.list_runs(CursorRequest{*second->next_cursor});
    REQUIRE(third);
    CHECK(run_ids(*third) == std::vector{recovery_id(10)});
    CHECK_FALSE(third->next_cursor);
}

TEST_CASE("History state-set cursors preserve method and expiry errors at the service boundary",
          "[jobu][history][sqlite]")
{
    Fixture fixture;
    auto    job = create_job(fixture, 1, 2);
    auto    run = create_run(fixture, job, 10, UtcTimePoint{10s}, 2);
    create_state_run(fixture, job, 11, RunState::Failed, UtcTimePoint{11s});
    auto page = fixture.history.list_runs(
        RunQuery{.filters = {.states = std::vector{RunState::Succeeded, RunState::Failed}}, .limit = 1});
    REQUIRE(page);
    REQUIRE(page->next_cursor);
    CHECK(fixture.history.list_attempts(CursorRequest{*page->next_cursor}).error().code ==
          "jobu.history.invalid_cursor");
    auto attempts = fixture.history.list_attempts(AttemptQuery{.run_id = run.run.id, .limit = 1});
    REQUIRE(attempts);
    REQUIRE(attempts->next_cursor);
    for (auto const& token : {std::string{"not-a-token"}, recovery_id(999).to_string(), *attempts->next_cursor}) {
        auto invalid = fixture.history.list_runs(CursorRequest{token});
        REQUIRE_FALSE(invalid);
        CHECK(invalid.error().code == "jobu.history.invalid_cursor");
    }
    fixture.time.advance(5min);
    auto expired = fixture.history.list_runs(CursorRequest{*page->next_cursor});
    REQUIRE_FALSE(expired);
    CHECK(expired.error().code == "jobu.history.invalid_cursor");
    CHECK(fixture.failures.empty());
}

TEST_CASE("Byte-limited run pages retain every selected row and the complete successor query",
          "[jobu][history][sqlite]")
{
    Fixture    fixture;
    auto       job    = create_job(fixture, 1, 2);
    auto const states = std::array{RunState::Failed,
                                   RunState::Scheduled,
                                   RunState::Interrupted,
                                   RunState::Failed,
                                   RunState::Running,
                                   RunState::Interrupted};
    auto       suffix = std::uint32_t{10};
    for (auto state : states) {
        create_state_run(fixture, job, suffix++, state, UtcTimePoint{10s});
    }

    auto query = RunQuery{
        .filters = {.job_id = job.id, .states = std::vector{RunState::Interrupted, RunState::Failed}},
        .limit   = 4
    };
    auto repository = jb::jobu::detail::HistoryRepository{fixture.storage.database, fixture.storage.registry};
    auto cursors    = jb::jobu::detail::HistoryCursorStore{fixture.tokens, fixture.time};
    auto rows       = repository.list_runs(query, std::nullopt, query.limit + 1U);
    REQUIRE(rows);
    REQUIRE(rows->size() == 4);
    auto maximum_row_bytes = std::size_t{0};
    for (auto const& row : *rows) {
        auto encoded = run_summary_to_json(row);
        REQUIRE(encoded);
        auto serialized = serialize_json(*encoded);
        REQUIRE(serialized);
        maximum_row_bytes = std::max(maximum_row_bytes, serialized->size());
    }

    // This leaves room for the wrapper and one summary, but not two summaries plus the wrapper.
    auto const budget   = maximum_row_bytes * 2U;
    auto       previous = std::optional<jb::jobu::detail::RunCursorState>{};
    auto       seen     = std::vector<Uuid>{};
    do {
        auto const& retained = previous ? previous->query : query;
        auto        after    = previous ? std::optional{previous->after} : std::nullopt;
        rows                 = repository.list_runs(retained, after, retained.limit + 1U);
        REQUIRE(rows);
        auto page = jb::jobu::detail::build_run_page(*rows, retained, previous, cursors, budget);
        REQUIRE(page);
        REQUIRE(page->items.size() == 1);
        CHECK(page_bytes(*page) <= budget);
        seen.push_back(page->items.front().id);
        REQUIRE(seen.size() <= 4);
        if (!page->next_cursor) {
            break;
        }
        auto saved = cursors.get_run(*page->next_cursor);
        REQUIRE(saved);
        CHECK(saved->query.filters.job_id == job.id);
        REQUIRE(saved->query.filters.states);
        CHECK(*saved->query.filters.states == *query.filters.states);
        CHECK(saved->query.limit == query.limit);
        CHECK(saved->after.id == page->items.back().id);
        previous = std::move(*saved);
    } while (true);
    CHECK(seen == std::vector{recovery_id(15), recovery_id(13), recovery_id(12), recovery_id(10)});
}

TEST_CASE("A run summary that cannot fit the byte budget issues no cursor", "[jobu][history][sqlite]")
{
    Fixture fixture;
    auto    job = create_job(fixture, 1, 2);
    create_state_run(fixture, job, 10, RunState::Failed, UtcTimePoint{10s});
    auto repository = jb::jobu::detail::HistoryRepository{fixture.storage.database, fixture.storage.registry};
    auto cursors    = jb::jobu::detail::HistoryCursorStore{fixture.tokens, fixture.time};
    auto query      = RunQuery{.filters = {.states = std::vector{RunState::Failed}}};
    auto rows       = repository.list_runs(query, std::nullopt, query.limit + 1U);
    REQUIRE(rows);
    REQUIRE(rows->size() == 1);
    auto encoded = run_summary_to_json(rows->front());
    REQUIRE(encoded);
    auto serialized = serialize_json(*encoded);
    REQUIRE(serialized);
    for (auto budget : {std::size_t{0}, serialized->size()}) {
        auto page = jb::jobu::detail::build_run_page(*rows, query, std::nullopt, cursors, budget);
        REQUIRE_FALSE(page);
        CHECK(page.error().category == ErrorCategory::ResourceExhausted);
        CHECK(page.error().code == "jobu.response.too_large");
        CHECK(cursors.size() == 0);
    }
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

TEST_CASE("Management job lists filter before their serialized result budget", "[jobu][history][management]")
{
    Fixture fixture;
    auto    queue = recovery_queue(recovery_id(1));
    fixture.storage.insert_queue(queue);
    for (auto suffix = std::uint32_t{2}; suffix < 8; ++suffix) {
        auto job = fixture.storage.make_job(recovery_id(suffix), queue.id);
        std::get<JsonValue::Object>(job.payload.data).emplace("padding", JsonValue{.data = std::string(200000, 'x')});
        if (suffix % 2U != 0) {
            job.state    = JobState::Succeeded;
            job.revision = 2;
        }
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
    CHECK(seen == std::vector<Uuid>{recovery_id(2),
                                    recovery_id(3),
                                    recovery_id(4),
                                    recovery_id(5),
                                    recovery_id(6),
                                    recovery_id(7)});

    request.page.after_id = std::nullopt;
    request.state         = JobState::Active;
    seen.clear();
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
            CHECK(job.state == JobState::Active);
            seen.push_back(job.id);
        }
        request.page.after_id = page->next_after_id;
        REQUIRE(seen.size() <= 3U);
    } while (request.page.after_id);
    CHECK(seen == std::vector<Uuid>{recovery_id(2), recovery_id(4), recovery_id(6)});
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
