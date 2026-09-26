#include "support/catch_utils.hpp" // IWYU pragma: keep for Catch::StringMaker specializations
#include "support/recovery_fixture.hpp"

#include "attempt_repository_priv.hpp"
#include "byte_buffer.hpp"
#include "json.hpp"
#include "query.hpp"
#include "recovery_repository_priv.hpp"
#include "scheduler_repository_priv.hpp"
#include "transaction.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <chrono>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

using namespace jb::core;
using namespace jb::db;
using namespace jb::jobu;
using namespace jb::jobu::detail;
using namespace jb::test;
using namespace std::chrono_literals;

namespace {

auto interruption_document() -> JsonValue
{
    auto parsed = parse_json(R"({"reason":"daemon_interrupted","outcome_unknown":true})");
    REQUIRE(parsed);
    return std::move(*parsed);
}

auto key_for(RecoveryRunFixture const& expected) -> RecoveryAttemptKey
{
    return {.run_id = expected.run.id, .attempt_number = expected.attempts.back().attempt.attempt_number};
}

auto seed_running(RecoveryFixture& fixture,
                  AttemptNumber    prior_failures = 0,
                  RunOrigin        origin         = RunOrigin::Scheduled,
                  JobType          type           = JobType::Cli) -> RecoveryRunFixture
{
    auto queue = recovery_queue(recovery_id(1));
    auto job   = fixture.make_job(recovery_id(2), queue.id, type);
    fixture.insert_queue(queue);
    fixture.insert_job(job);
    if (origin == RunOrigin::Manual) {
        // A nonterminal manual run requires its schedule-owned sibling, which recovery must preserve.
        fixture.insert_run(fixture.make_run(recovery_id(4), job));
    }
    auto expected = fixture.make_run(recovery_id(3), job, RunState::Running, prior_failures, origin);
    if (prior_failures != 0) {
        expected.attempts.front().output = AttemptOutput{.stdout_bytes     = ByteBuffer{std::byte{0x41}},
                                                         .stderr_bytes     = ByteBuffer{},
                                                         .stdout_truncated = true};
    }
    fixture.insert_run(expected);
    return expected;
}

auto interrupted(RecoveryRunFixture expected, UtcTimePoint recovery_time) -> RecoveryRunFixture
{
    // Change only the fields belonging to the interruption; require_run compares every other field and row count.
    auto& latest                = expected.attempts.back();
    latest.attempt.state        = AttemptState::Completed;
    latest.attempt.outcome      = AttemptOutcome::Interrupted;
    latest.attempt.completed_at = recovery_time;
    latest.attempt.result       = interruption_document();
    latest.output = AttemptOutput{.stdout_bytes = ByteBuffer{}, .stderr_bytes = ByteBuffer{}, .capture_lost = true};
    expected.run.state        = RunState::Interrupted;
    expected.run.completed_at = recovery_time;
    expected.run.result       = interruption_document();
    return expected;
}

void execute(Database& database, std::string_view sql)
{
    INFO(sql);
    Query query{database};
    REQUIRE(query.exec(sql));
}

void require_invariant(Result<void, Error> const& result)
{
    REQUIRE_FALSE(result);
    CHECK(result.error().code == "jobu.recovery.invariant");
    CHECK(result.error().detail.starts_with("reason="));
}

void require_valid(RecoveryFixture& fixture)
{
    RecoveryRepository repository{fixture.database, fixture.registry};
    REQUIRE(repository.list_runs(256));
    REQUIRE(repository.list_attempts(256));
    REQUIRE(repository.list_outputs(256));
    REQUIRE(repository.list_jobs(256));
    REQUIRE(repository.list_queues(256));
}

} // namespace

TEST_CASE("Recovery interruption commits unknown outcome and lost capture atomically", "[jobu][recovery][sqlite]")
{
    auto const      prior_failures = GENERATE(AttemptNumber{0}, AttemptNumber{2});
    auto const      origin         = GENERATE(RunOrigin::Scheduled, RunOrigin::Manual);
    auto const      type           = GENERATE(JobType::Cli, JobType::Http);
    // A wall-clock regression is legal: recovery must preserve historical timestamps rather than reorder them.
    auto const      recovery_time  = GENERATE(UtcTimePoint{5s}, UtcTimePoint{100s + 123456us});
    RecoveryFixture fixture;
    auto            expected = seed_running(fixture, prior_failures, origin, type);
    auto            key      = key_for(expected);
    {
        auto transaction = Transaction::begin(fixture.database);
        REQUIRE(transaction);
        RecoveryRepository repository{fixture.database, fixture.registry};
        REQUIRE(repository.interrupt_attempt(key, recovery_time));
        REQUIRE(repository.set_run_interrupted(key, recovery_time));
        REQUIRE(transaction->commit());
    }

    expected = interrupted(std::move(expected), recovery_time);
    fixture.require_run(expected);
    fixture.reopen();
    fixture.require_run(expected);
    require_valid(fixture);

    if (origin == RunOrigin::Manual) {
        auto job = fixture.make_job(recovery_id(2), recovery_id(1), type);
        fixture.require_run(fixture.make_run(recovery_id(4), job));
    }
}

TEST_CASE("Recovery interruption preserves the claimed snapshot after definition edits", "[jobu][recovery][sqlite]")
{
    RecoveryFixture fixture;
    auto            queue    = recovery_queue(recovery_id(1));
    auto            job      = fixture.make_job(recovery_id(2), queue.id);
    auto            original = fixture.make_run(recovery_id(3), job, RunState::Running, 1);
    // The current definition is deliberately different from the already claimed run.
    job.revision             = 2;
    job.priority             = 42;
    job.payload              = {.data = JsonValue::Object{{"command", {.data = std::string{"/bin/false"}}}}};
    fixture.insert_queue(queue);
    fixture.insert_job(job);
    fixture.insert_run(original);

    auto const recovery_time = UtcTimePoint{100s};
    {
        auto transaction = Transaction::begin(fixture.database);
        REQUIRE(transaction);
        RecoveryRepository repository{fixture.database, fixture.registry};
        REQUIRE(repository.interrupt_attempt(key_for(original), recovery_time));
        REQUIRE(repository.set_run_interrupted(key_for(original), recovery_time));
        REQUIRE(transaction->commit());
    }
    fixture.reopen();
    fixture.require_run(interrupted(original, recovery_time));
    require_valid(fixture);
}

TEST_CASE("Recovery interruption remains inside the caller transaction", "[jobu][recovery][sqlite]")
{
    auto const      finish_run        = GENERATE(false, true);
    auto const      explicit_rollback = GENERATE(false, true);
    RecoveryFixture fixture;
    auto            original = seed_running(fixture, 1);
    {
        auto transaction = Transaction::begin(fixture.database);
        REQUIRE(transaction);
        RecoveryRepository repository{fixture.database, fixture.registry};
        REQUIRE(repository.interrupt_attempt(key_for(original), UtcTimePoint{100s}));
        if (finish_run) {
            REQUIRE(repository.set_run_interrupted(key_for(original), UtcTimePoint{100s}));
        }
        if (explicit_rollback) {
            REQUIRE(transaction->rollback());
        }
        // Otherwise RAII must undo even a fully successful unit that the caller has not committed.
    }
    fixture.require_run(original);
    fixture.reopen();
    fixture.require_run(original);
    require_valid(fixture);
}

TEST_CASE("Recovery interruption write failures roll back all three rows", "[jobu][recovery][sqlite]")
{
    auto const* boundary = GENERATE("UPDATE ON jobu_attempts", "INSERT ON jobu_attempt_output", "UPDATE ON jobu_runs");
    auto const  ignore   = GENERATE(false, true);
    RecoveryFixture fixture;
    auto            original = seed_running(fixture, 1);
    // A named table boundary fails deterministically, including zero affected rows without a driver error.
    auto const*     action   = ignore ? "IGNORE" : "ABORT, 'private SQL payload credential sentinel'";
    execute(fixture.database,
            "CREATE TRIGGER fail_interruption BEFORE " + std::string{boundary} + " BEGIN SELECT RAISE(" + action +
                "); END");
    {
        auto transaction = Transaction::begin(fixture.database);
        REQUIRE(transaction);
        RecoveryRepository repository{fixture.database, fixture.registry};
        auto               result = repository.interrupt_attempt(key_for(original), UtcTimePoint{100s});
        if (result) {
            result = repository.set_run_interrupted(key_for(original), UtcTimePoint{100s});
        }
        REQUIRE_FALSE(result);
        if (ignore) {
            CHECK(
                (result.error().code == "jobu.recovery.invariant" || result.error().code == "jobu.storage.invariant"));
        }
        else {
            CHECK(result.error().code == "db.constraint");
        }
        CHECK(result.error().message.find("sentinel") == std::string::npos);
        CHECK(result.error().detail.find("sentinel") == std::string::npos);
    }
    fixture.require_run(original);
    fixture.reopen();
    fixture.require_run(original);
    require_valid(fixture);

    // A later invocation can repair the original durable state once the injected fault is gone.
    execute(fixture.database, "DROP TRIGGER fail_interruption");
    {
        auto transaction = Transaction::begin(fixture.database);
        REQUIRE(transaction);
        RecoveryRepository repository{fixture.database, fixture.registry};
        REQUIRE(repository.interrupt_attempt(key_for(original), UtcTimePoint{100s}));
        REQUIRE(repository.set_run_interrupted(key_for(original), UtcTimePoint{100s}));
        REQUIRE(transaction->commit());
    }
    fixture.reopen();
    fixture.require_run(interrupted(original, UtcTimePoint{100s}));
}

TEST_CASE("Recovery interruption rejects stale and invalid attempt keys", "[jobu][recovery][sqlite]")
{
    auto const number =
        GENERATE(AttemptNumber{0}, AttemptNumber{1}, AttemptNumber{3}, std::numeric_limits<AttemptNumber>::max());
    RecoveryFixture fixture;
    auto            original = seed_running(fixture, 1);
    {
        auto transaction = Transaction::begin(fixture.database);
        REQUIRE(transaction);
        RecoveryRepository repository{fixture.database, fixture.registry};
        require_invariant(
            repository.interrupt_attempt({.run_id = original.run.id, .attempt_number = number}, UtcTimePoint{100s}));
    }
    fixture.reopen();
    fixture.require_run(original);
}

TEST_CASE("Recovery interruption rejects an already terminal run", "[jobu][recovery][sqlite]")
{
    RecoveryFixture fixture;
    auto            original = seed_running(fixture);
    auto            expected = interrupted(original, UtcTimePoint{100s});
    {
        auto transaction = Transaction::begin(fixture.database);
        REQUIRE(transaction);
        RecoveryRepository repository{fixture.database, fixture.registry};
        REQUIRE(repository.interrupt_attempt(key_for(original), UtcTimePoint{100s}));
        REQUIRE(repository.set_run_interrupted(key_for(original), UtcTimePoint{100s}));
        REQUIRE(transaction->commit());
    }
    {
        auto transaction = Transaction::begin(fixture.database);
        REQUIRE(transaction);
        RecoveryRepository repository{fixture.database, fixture.registry};
        require_invariant(repository.interrupt_attempt(key_for(original), UtcTimePoint{200s}));
    }
    {
        auto transaction = Transaction::begin(fixture.database);
        REQUIRE(transaction);
        RecoveryRepository repository{fixture.database, fixture.registry};
        require_invariant(repository.set_run_interrupted(key_for(original), UtcTimePoint{200s}));
    }
    fixture.reopen();
    fixture.require_run(expected);
}

TEST_CASE("Recovery interruption revalidates persisted state inside the transaction", "[jobu][recovery][sqlite]")
{
    auto const* mutation =
        GENERATE("DELETE FROM jobu_runs",
                 "UPDATE jobu_attempts SET state = 'completed', outcome = 'failed', completed_at_us "
                 "= 90000000, result_json = '{}'",
                 "UPDATE jobu_queues SET state = 'deleted', deleted_name = name, deleted_at_us = 90000000",
                 "UPDATE jobu_jobs SET state = 'suspended'");
    RecoveryFixture    fixture;
    auto               original = seed_running(fixture);
    RecoveryRepository repository{fixture.database, fixture.registry};
    REQUIRE(repository.find_run(original.run.id));
    {
        auto transaction = Transaction::begin(fixture.database);
        REQUIRE(transaction);
        // The candidate was valid before this transaction; recovery must not trust that earlier read.
        execute(fixture.database, mutation);
        require_invariant(repository.interrupt_attempt(key_for(original), UtcTimePoint{100s}));
    }
    fixture.reopen();
    fixture.require_run(original);
    require_valid(fixture);
}

TEST_CASE("Recovery interruption rejects nonrunning candidates without changing history", "[jobu][recovery][sqlite]")
{
    auto const      state = GENERATE(RunState::Scheduled, RunState::RetryWait, RunState::Failed);
    RecoveryFixture fixture;
    auto            queue    = recovery_queue(recovery_id(1));
    auto            job      = fixture.make_job(recovery_id(2), queue.id);
    auto            original = fixture.make_run(recovery_id(3), job, state, state == RunState::Scheduled ? 0 : 1);
    fixture.insert_queue(queue);
    fixture.insert_job(job);
    fixture.insert_run(original);
    {
        auto transaction = Transaction::begin(fixture.database);
        REQUIRE(transaction);
        RecoveryRepository repository{fixture.database, fixture.registry};
        require_invariant(
            repository.interrupt_attempt({.run_id = original.run.id, .attempt_number = 1}, UtcTimePoint{100s}));
    }
    fixture.reopen();
    fixture.require_run(original);
    require_valid(fixture);
}

TEST_CASE("Recovery interruption preserves unexpected output evidence", "[jobu][recovery][sqlite]")
{
    RecoveryFixture fixture;
    auto            original        = seed_running(fixture);
    original.attempts.back().output = AttemptOutput{.stdout_bytes     = ByteBuffer{std::byte{0x42}},
                                                    .stderr_bytes     = ByteBuffer{},
                                                    .stderr_truncated = true};
    AttemptRepository attempts{fixture.database};
    REQUIRE(attempts.insert_or_replace_output(original.run.id, 1, *original.attempts.back().output));
    {
        auto transaction = Transaction::begin(fixture.database);
        REQUIRE(transaction);
        RecoveryRepository repository{fixture.database, fixture.registry};
        require_invariant(repository.interrupt_attempt(key_for(original), UtcTimePoint{100s}));
    }
    fixture.reopen();
    fixture.require_run(original);
}

TEST_CASE("Recovery terminal setter requires matching interrupted attempt and capture", "[jobu][recovery][sqlite]")
{
    auto const*     scenario = GENERATE("unfinished", "timestamp", "key", "missing_output", "capture_metadata");
    RecoveryFixture fixture;
    auto            original = seed_running(fixture, 1);
    {
        auto transaction = Transaction::begin(fixture.database);
        REQUIRE(transaction);
        RecoveryRepository repository{fixture.database, fixture.registry};
        auto               key  = key_for(original);
        auto               time = UtcTimePoint{100s};
        if (std::string_view{scenario} != "unfinished") {
            REQUIRE(repository.interrupt_attempt(key, time));
        }
        if (std::string_view{scenario} == "timestamp") {
            time += 1us;
        }
        else if (std::string_view{scenario} == "key") {
            --key.attempt_number;
        }
        else if (std::string_view{scenario} == "missing_output") {
            execute(fixture.database, "DELETE FROM jobu_attempt_output WHERE attempt_number = 2");
        }
        else if (std::string_view{scenario} == "capture_metadata") {
            execute(fixture.database, "UPDATE jobu_attempt_output SET capture_lost = 0 WHERE attempt_number = 2");
        }
        require_invariant(repository.set_run_interrupted(key, time));
    }
    fixture.reopen();
    fixture.require_run(original);
}

TEST_CASE("Normal completion still rejects Interrupted run state", "[jobu][recovery][sqlite]")
{
    RecoveryFixture fixture;
    auto            original = seed_running(fixture);
    {
        auto transaction = Transaction::begin(fixture.database);
        REQUIRE(transaction);
        SchedulerRepository repository{fixture.database, fixture.registry};
        auto result = repository.set_run_terminal(original.run.id, RunState::Interrupted, UtcTimePoint{100s}, "{}");
        REQUIRE_FALSE(result);
        CHECK(result.error().code == "jobu.storage.invariant");
    }
    fixture.reopen();
    fixture.require_run(original);
}
