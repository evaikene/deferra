#include "support/recovery_fixture.hpp"

#include "attempt_repository_priv.hpp"
#include "query.hpp"
#include "recovery_repository_priv.hpp"
#include "transaction.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <chrono>
#include <cstddef>
#include <optional>
#include <string_view>
#include <vector>

using namespace jb::core;
using namespace jb::db;
using namespace jb::jobu;
using namespace jb::jobu::detail;
using namespace jb::test;
using namespace std::chrono_literals;

namespace {

void execute(Database& database, std::string_view sql)
{
    Query query{database};
    REQUIRE(query.exec(sql));
}

template <typename T>
void require_invariant(Result<T, Error> const& result)
{
    REQUIRE_FALSE(result);
    CHECK(result.error().code == "jobu.recovery.invariant");
    CHECK(result.error().detail.starts_with("reason="));
    CHECK(result.error().detail.find("SELECT") == std::string_view::npos);
}

void require_scans(RecoveryRepository& repository)
{
    REQUIRE(repository.list_runs(4096));
    REQUIRE(repository.list_attempts(4096));
    REQUIRE(repository.list_outputs(4096));
    REQUIRE(repository.list_jobs(4096));
    REQUIRE(repository.list_queues(4096));
}

} // namespace

TEST_CASE("Recovery scan limits and empty pages", "[jobu][recovery][sqlite]")
{
    RecoveryFixture    fixture;
    RecoveryRepository repository{fixture.database, fixture.registry};
    for (auto limit : {std::size_t{0}, std::size_t{4097}}) {
        auto runs     = repository.list_runs(limit);
        auto attempts = repository.list_attempts(limit);
        auto outputs  = repository.list_outputs(limit);
        REQUIRE_FALSE(runs);
        REQUIRE_FALSE(attempts);
        REQUIRE_FALSE(outputs);
        CHECK(runs.error().code == "jobu.storage.invalid_limit");
        CHECK(attempts.error().code == "jobu.storage.invalid_limit");
        CHECK(outputs.error().code == "jobu.storage.invalid_limit");
    }
    for (auto limit : {std::size_t{1}, std::size_t{4096}}) {
        auto runs     = repository.list_runs(limit);
        auto attempts = repository.list_attempts(limit);
        auto outputs  = repository.list_outputs(limit);
        REQUIRE(runs);
        REQUIRE(attempts);
        REQUIRE(outputs);
        CHECK(runs->empty());
        CHECK(attempts->empty());
        CHECK(outputs->empty());
    }
    require_invariant(repository.find_run(recovery_id(99)));
}

TEST_CASE("Recovery pages follow stored UUID and composite order without changing history", "[jobu][recovery][sqlite]")
{
    auto            page_size = GENERATE(std::size_t{1}, std::size_t{2}, std::size_t{3}, std::size_t{4});
    RecoveryFixture fixture;
    auto            queue = recovery_queue(recovery_id(1));
    auto            job   = fixture.make_job(recovery_id(2), queue.id);
    fixture.insert_queue(queue);
    fixture.insert_job(job);
    auto expected = std::vector<RecoveryRunFixture>{};
    // Reverse insertion across a byte boundary distinguishes keyset order from insertion order.
    for (auto id : {65536U, 256U, 255U, 3U}) {
        auto run = fixture.make_run(recovery_id(id), job, RunState::Failed, 1);
        for (auto& attempt : run.attempts) {
            attempt.output = AttemptOutput{};
        }
        fixture.insert_run(run);
        expected.push_back(run);
    }
    RecoveryRepository repository{fixture.database, fixture.registry};
    auto               ids   = std::vector<Uuid>{};
    auto               after = std::optional<Uuid>{};
    while (true) {
        auto page = repository.list_runs(page_size, after);
        REQUIRE(page);
        CHECK(page->size() <= page_size);
        if (page->empty()) {
            break;
        }
        after = page->back().id;
        for (auto const& run : *page) {
            ids.push_back(run.id);
        }
    }
    REQUIRE(ids == std::vector<Uuid>{recovery_id(3), recovery_id(255), recovery_id(256), recovery_id(65536)});

    auto attempt_keys = std::vector<RecoveryAttemptKey>{};
    auto cursor       = std::optional<RecoveryAttemptKey>{};
    while (true) {
        auto page = repository.list_attempts(page_size, cursor);
        REQUIRE(page);
        CHECK(page->size() <= page_size);
        if (page->empty()) {
            break;
        }
        for (auto const& attempt : *page) {
            attempt_keys.push_back({.run_id = attempt.run_id, .attempt_number = attempt.attempt_number});
        }
        cursor = attempt_keys.back();
    }
    auto expected_keys = std::vector<RecoveryAttemptKey>{};
    for (auto const& id : ids) {
        expected_keys.push_back({.run_id = id, .attempt_number = 1});
        expected_keys.push_back({.run_id = id, .attempt_number = 2});
    }
    CHECK(attempt_keys == expected_keys);
    auto output_keys = std::vector<RecoveryAttemptKey>{};
    cursor.reset();
    while (true) {
        auto page = repository.list_outputs(page_size, cursor);
        REQUIRE(page);
        if (page->empty()) {
            break;
        }
        output_keys.insert(output_keys.end(), page->begin(), page->end());
        cursor = page->back();
    }
    CHECK(output_keys == expected_keys);

    // Returned pages retain no live query, and scans leave every durable field unchanged.
    auto transaction = Transaction::begin(fixture.database);
    REQUIRE(transaction);
    REQUIRE(transaction->rollback());
    fixture.reopen();
    for (auto const& run : expected) {
        fixture.require_run(run);
    }
}

TEST_CASE("Recovery scans accept valid lifecycle histories and recovered retry outcomes", "[jobu][recovery][sqlite]")
{
    auto            state               = GENERATE(RunState::Scheduled,
                                                   RunState::Running,
                                                   RunState::RetryWait,
                                                   RunState::Succeeded,
                                                   RunState::Failed,
                                                   RunState::Interrupted,
                                                   RunState::Cancelled);
    auto            interrupted_history = GENERATE(false, true);
    RecoveryFixture fixture;
    auto            queue = recovery_queue(recovery_id(1));
    auto            job   = fixture.make_job(recovery_id(2), queue.id);
    fixture.insert_queue(queue);
    fixture.insert_job(job);
    auto failures = state == RunState::Scheduled ? 0U : 1U;
    auto run      = fixture.make_run(recovery_id(3), job, state, failures);
    if (interrupted_history && !run.attempts.empty()) {
        run.attempts.front().attempt.outcome = AttemptOutcome::Interrupted;
    }
    if (!run.attempts.empty()) {
        run.attempts.front().output = AttemptOutput{.capture_lost = interrupted_history};
    }
    fixture.insert_run(run);
    RecoveryRepository repository{fixture.database, fixture.registry};
    require_scans(repository);
    auto running = repository.list_runs(1, {}, RunState::Running);
    REQUIRE(running);
    CHECK(running->size() == (state == RunState::Running ? 1U : 0U));
    fixture.require_run(run);
}

TEST_CASE("Recovery rejects invalid run and attempt relationships", "[jobu][recovery][sqlite]")
{
    auto const* sql =
        GENERATE("DELETE FROM jobu_attempts WHERE attempt_number = 2",
                 "UPDATE jobu_attempts SET state = 'running', completed_at_us = NULL, outcome = NULL, "
                 "result_json = NULL WHERE attempt_number = 1",
                 "UPDATE jobu_attempts SET attempt_number = 3 WHERE attempt_number = 2",
                 "UPDATE jobu_attempts SET due_at_us = due_at_us + 1 WHERE attempt_number = 2",
                 "UPDATE jobu_runs SET started_at_us = started_at_us + 1",
                 "UPDATE jobu_attempts SET outcome = 'succeeded' WHERE attempt_number = 1",
                 "UPDATE jobu_attempts SET state = 'pending', started_at_us = NULL WHERE attempt_number = 2",
                 "UPDATE jobu_jobs SET state = 'deleted', deleted_at_us = 100000000",
                 "UPDATE jobu_queues SET state = 'deleted', deleted_at_us = 100000000, deleted_name = name",
                 "UPDATE jobu_queues SET state = 'suspended'",
                 "UPDATE jobu_jobs SET state = 'suspended'");
    CAPTURE(sql);
    RecoveryFixture fixture;
    auto            queue = recovery_queue(recovery_id(1));
    auto            job   = fixture.make_job(recovery_id(2), queue.id);
    fixture.insert_queue(queue);
    fixture.insert_job(job);
    fixture.insert_run(fixture.make_run(recovery_id(3), job, RunState::Running, 1));
    execute(fixture.database, sql);
    RecoveryRepository repository{fixture.database, fixture.registry};
    require_invariant(repository.list_runs(1));
}

TEST_CASE("Recovery scans diagnose pending and early output instead of executing them", "[jobu][recovery][sqlite]")
{
    auto            pending = GENERATE(false, true);
    RecoveryFixture fixture;
    auto            queue = recovery_queue(recovery_id(1));
    auto            job   = fixture.make_job(recovery_id(2), queue.id);
    fixture.insert_queue(queue);
    fixture.insert_job(job);
    auto run                   = fixture.make_run(recovery_id(3), job, RunState::Running);
    run.attempts.back().output = AttemptOutput{};
    fixture.insert_run(run);
    if (pending) {
        execute(fixture.database, "UPDATE jobu_attempts SET state = 'pending', started_at_us = NULL");
    }
    RecoveryRepository repository{fixture.database, fixture.registry};
    require_invariant(repository.list_runs(1));
    require_invariant(repository.list_attempts(1));
    require_invariant(repository.list_outputs(1));
}

TEST_CASE("Recovery scans find orphan rows without inner joins hiding them", "[jobu][recovery][sqlite]")
{
    auto            orphan = GENERATE(0, 1, 2);
    RecoveryFixture fixture;
    auto            queue = recovery_queue(recovery_id(1));
    auto            job   = fixture.make_job(recovery_id(2), queue.id);
    fixture.insert_queue(queue);
    fixture.insert_job(job);
    auto run                   = fixture.make_run(recovery_id(3), job, RunState::Failed);
    run.attempts.back().output = AttemptOutput{};
    fixture.insert_run(run);
    // Deliberate physical corruption belongs only in this SQLite fixture, never generic JobU.
    execute(fixture.database, "PRAGMA foreign_keys = OFF");
    RecoveryRepository repository{fixture.database, fixture.registry};
    if (orphan == 0) {
        execute(fixture.database, "DELETE FROM jobu_jobs");
        require_invariant(repository.list_runs(1));
    }
    else if (orphan == 1) {
        execute(fixture.database, "DELETE FROM jobu_runs");
        require_invariant(repository.list_attempts(1));
    }
    else {
        execute(fixture.database, "DELETE FROM jobu_attempts");
        require_invariant(repository.list_outputs(1));
    }
}

TEST_CASE("Recovery barriers distinguish remaining one-time work from invalid siblings", "[jobu][recovery][sqlite]")
{
    auto            scenario = GENERATE(0, 1, 2, 3);
    RecoveryFixture fixture;
    auto            queue = recovery_queue(recovery_id(1));
    auto            job   = fixture.make_job(recovery_id(2), queue.id);
    if (scenario == 3) {
        job.schedule = CronSchedule{.expression = "* * * * *", .timezone = "UTC"};
    }
    fixture.insert_queue(queue);
    fixture.insert_job(job);
    fixture.insert_run(fixture.make_run(recovery_id(3), job, RunState::Scheduled, 0, RunOrigin::Manual));
    if (scenario == 1 || scenario == 2) {
        fixture.insert_run(fixture.make_run(recovery_id(4), job));
    }
    if (scenario == 1) {
        fixture.insert_run(fixture.make_run(recovery_id(5), job, RunState::Scheduled, 0, RunOrigin::Manual));
    }
    if (scenario == 2) {
        execute(fixture.database, "DROP INDEX jobu_runs_schedule_owned_non_terminal_uidx");
        fixture.insert_run(fixture.make_run(recovery_id(5), job));
    }
    RecoveryRepository repository{fixture.database, fixture.registry};
    if (scenario == 0) {
        require_scans(repository);
    }
    else {
        require_invariant(repository.list_runs(1));
    }
}

TEST_CASE("Recovery preserves historical owners and suspended manual work", "[jobu][recovery][sqlite]")
{
    RecoveryFixture fixture;
    auto            old_queue = recovery_queue(recovery_id(1), QueueState::Deleted);
    auto            queue     = recovery_queue(recovery_id(2));
    auto            job       = fixture.make_job(recovery_id(3), queue.id);
    job.state                 = JobState::Suspended;
    fixture.insert_queue(old_queue);
    fixture.insert_queue(queue);
    fixture.insert_job(job);
    auto history         = fixture.make_run(recovery_id(4), job, RunState::Failed, 1);
    history.run.queue_id = old_queue.id;
    fixture.insert_run(history);
    auto scheduled = fixture.make_run(recovery_id(5), job);
    fixture.insert_run(scheduled);
    auto manual = fixture.make_run(recovery_id(6), job, RunState::Running, 1, RunOrigin::Manual);
    // A backward wall-clock step does not invalidate independently sampled starts.
    manual.attempts.back().attempt.started_at = UtcTimePoint{1s};
    fixture.insert_run(manual);
    RecoveryRepository repository{fixture.database, fixture.registry};
    require_scans(repository);
    fixture.require_run(history);
    fixture.require_run(manual);
}

TEST_CASE("Recovery decodes malformed durable rows with safe errors", "[jobu][recovery][sqlite]")
{
    auto const* sql =
        GENERATE("UPDATE jobu_runs SET payload_json = 'sensitive-invalid-json'",
                 "UPDATE jobu_runs SET attributes_json = '{}'",
                 "UPDATE jobu_runs SET state = 'bogus'",
                 "UPDATE jobu_runs SET id = zeroblob(16)",
                 "UPDATE jobu_runs SET runnable_at_us = 'invalid'",
                 "UPDATE jobu_attempts SET result_json = 'sensitive-invalid-json' WHERE attempt_number = 1");
    CAPTURE(sql);
    RecoveryFixture fixture;
    auto            queue = recovery_queue(recovery_id(1));
    auto            job   = fixture.make_job(recovery_id(2), queue.id);
    fixture.insert_queue(queue);
    fixture.insert_job(job);
    fixture.insert_run(fixture.make_run(recovery_id(3), job, RunState::Running, 1));
    execute(fixture.database, "PRAGMA foreign_keys = OFF");
    execute(fixture.database, "PRAGMA ignore_check_constraints = ON");
    execute(fixture.database, sql);
    RecoveryRepository repository{fixture.database, fixture.registry};
    auto               result = repository.list_runs(1);
    require_invariant(result);
    CHECK(result.error().message.find("sensitive") == std::string_view::npos);
    CHECK(result.error().detail.find("sensitive") == std::string_view::npos);
}

TEST_CASE("Recovery driver errors preserve identity without backend details", "[jobu][recovery][sqlite]")
{
    RecoveryFixture fixture;
    execute(fixture.database, "DROP TABLE jobu_attempt_output");
    RecoveryRepository repository{fixture.database, fixture.registry};
    auto               result = repository.list_outputs(1);
    REQUIRE_FALSE(result);
    CHECK(result.error().code.starts_with("db."));
    CHECK(result.error().detail.find("jobu_attempt_output") == std::string_view::npos);
    CHECK(result.error().message.find("SELECT") == std::string_view::npos);
}

TEST_CASE("Recovery owner pages include repair candidates without inventing work", "[jobu][recovery][sqlite]")
{
    RecoveryFixture fixture;
    for (auto suffix : {256U, 255U, 3U}) {
        auto queue   = recovery_queue(recovery_id(suffix), QueueState::Suspending);
        auto job     = fixture.make_job(recovery_id(suffix + 1000), queue.id);
        job.state    = JobState::Suspending;
        job.schedule = CronSchedule{.expression = "* * * * *"};
        fixture.insert_queue(queue);
        fixture.insert_job(job);
    }
    RecoveryRepository repository{fixture.database, fixture.registry};
    auto               cursor = std::optional<Uuid>{};
    for (auto suffix : {3U, 255U, 256U}) {
        auto page = repository.list_queues(1, cursor);
        REQUIRE(page);
        REQUIRE(page->size() == 1);
        CHECK(page->front().id == recovery_id(suffix));
        CHECK(page->front().state == QueueState::Suspending);
        cursor = page->front().id;
    }
    auto queues_done = repository.list_queues(1, cursor);
    REQUIRE(queues_done);
    CHECK(queues_done->empty());
    cursor.reset();
    for (auto suffix : {3U, 255U, 256U}) {
        auto page = repository.list_jobs(1, cursor);
        REQUIRE(page);
        REQUIRE(page->size() == 1);
        CHECK(page->front().id == recovery_id(suffix + 1000));
        CHECK(page->front().state == JobState::Suspending);
        cursor = page->front().id;
    }
    auto jobs_done = repository.list_jobs(1, cursor);
    REQUIRE(jobs_done);
    CHECK(jobs_done->empty());
    auto runs = repository.list_runs(1);
    REQUIRE(runs);
    CHECK(runs->empty());
    for (auto limit : {std::size_t{0}, std::size_t{4097}}) {
        CHECK_FALSE(repository.list_jobs(limit));
        CHECK_FALSE(repository.list_queues(limit));
    }
}

TEST_CASE("Recovery validates owners even without runs", "[jobu][recovery][sqlite]")
{
    auto            scenario = GENERATE(0, 1, 2, 3);
    RecoveryFixture fixture;
    auto            queue = recovery_queue(recovery_id(1));
    auto            job   = fixture.make_job(recovery_id(2), queue.id);
    fixture.insert_queue(queue);
    fixture.insert_job(job);
    RecoveryRepository repository{fixture.database, fixture.registry};
    if (scenario == 0) {
        execute(fixture.database, "PRAGMA foreign_keys = OFF");
        execute(fixture.database, "DELETE FROM jobu_queues");
    }
    else if (scenario == 1) {
        execute(fixture.database,
                "UPDATE jobu_queues SET state = 'deleted', deleted_at_us = 100000000, deleted_name = name");
    }
    else if (scenario == 2) {
        execute(fixture.database, "UPDATE jobu_jobs SET attributes_json = '{}'");
    }
    else {
        execute(fixture.database, "PRAGMA ignore_check_constraints = ON");
        execute(fixture.database, "UPDATE jobu_queues SET recovery_policy = 'invalid'");
        require_invariant(repository.list_queues(1));
    }
    require_invariant(repository.list_jobs(1));
}

TEST_CASE("Recovery attempt pages decode middle history and reject running terminal leftovers",
          "[jobu][recovery][sqlite]")
{
    auto const* sql = GENERATE("UPDATE jobu_attempts SET result_json = 'invalid' WHERE attempt_number = 2",
                               "UPDATE jobu_attempts SET state = 'running', completed_at_us = NULL, outcome = NULL, "
                               "result_json = NULL WHERE attempt_number = 2",
                               "UPDATE jobu_attempts SET completed_at_us = NULL WHERE attempt_number = 2");
    RecoveryFixture fixture;
    auto            queue = recovery_queue(recovery_id(1));
    auto            job   = fixture.make_job(recovery_id(2), queue.id);
    fixture.insert_queue(queue);
    fixture.insert_job(job);
    fixture.insert_run(fixture.make_run(recovery_id(3), job, RunState::Failed, 2));
    execute(fixture.database, sql);
    RecoveryRepository repository{fixture.database, fixture.registry};
    // Start inside a run: the middle row must still be decoded, not just the first and last.
    require_invariant(repository.list_attempts(1, RecoveryAttemptKey{.run_id = recovery_id(3), .attempt_number = 1}));
}

TEST_CASE("Recovery validates terminal metadata and RetryWait history", "[jobu][recovery][sqlite]")
{
    auto            state = GENERATE(RunState::RetryWait, RunState::Failed, RunState::Cancelled);
    RecoveryFixture fixture;
    auto            queue = recovery_queue(recovery_id(1));
    auto            job   = fixture.make_job(recovery_id(2), queue.id);
    fixture.insert_queue(queue);
    fixture.insert_job(job);
    fixture.insert_run(fixture.make_run(recovery_id(3), job, state, 1));
    if (state == RunState::RetryWait) {
        execute(fixture.database, "UPDATE jobu_attempts SET outcome = 'succeeded'");
    }
    else if (state == RunState::Failed) {
        execute(fixture.database, "UPDATE jobu_runs SET completed_at_us = completed_at_us + 1");
    }
    else {
        execute(fixture.database, "UPDATE jobu_runs SET completed_at_us = NULL");
    }
    RecoveryRepository repository{fixture.database, fixture.registry};
    require_invariant(repository.list_runs(1));
}

TEST_CASE("Recovery scans support pre-dispatch and running cancellation without writes", "[jobu][recovery][sqlite]")
{
    auto            started = GENERATE(false, true);
    RecoveryFixture fixture;
    auto            queue = recovery_queue(recovery_id(1));
    auto            job   = fixture.make_job(recovery_id(2), queue.id);
    fixture.insert_queue(queue);
    fixture.insert_job(job);
    auto run = fixture.make_run(recovery_id(3), job, started ? RunState::Failed : RunState::Cancelled);
    if (started) {
        run.run.state                       = RunState::Cancelled;
        run.attempts.back().attempt.outcome = AttemptOutcome::Cancelled;
    }
    fixture.insert_run(run);
    execute(fixture.database, "PRAGMA query_only = ON");
    RecoveryRepository repository{fixture.database, fixture.registry};
    require_scans(repository);
    fixture.require_run(run);
}

TEST_CASE("Recovery output pages validate metadata without loading capture", "[jobu][recovery][sqlite]")
{
    RecoveryFixture fixture;
    auto            queue = recovery_queue(recovery_id(1));
    auto            job   = fixture.make_job(recovery_id(2), queue.id);
    fixture.insert_queue(queue);
    fixture.insert_job(job);
    auto run                   = fixture.make_run(recovery_id(3), job, RunState::Failed);
    run.attempts.back().output = AttemptOutput{};
    fixture.insert_run(run);
    execute(fixture.database, "PRAGMA ignore_check_constraints = ON");
    execute(fixture.database, "UPDATE jobu_attempt_output SET capture_lost = 2");
    RecoveryRepository repository{fixture.database, fixture.registry};
    require_invariant(repository.list_outputs(1));
}
