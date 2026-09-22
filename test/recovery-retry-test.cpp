#include "support/fake_attempt_executor.hpp"
#include "support/recovery_fixture.hpp"
#include "support/rejecting_secret_provider.hpp"

#include "attempt_repository_priv.hpp"
#include "attribute_codec_priv.hpp"
#include "byte_buffer.hpp"
#include "json.hpp"
#include "query.hpp"
#include "recovery_repository_priv.hpp"
#include "scheduler_dispatch_priv.hpp"
#include "scheduler_repository_priv.hpp"
#include "transaction.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <chrono>
#include <cstddef>
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

auto key_for(RecoveryRunFixture const& run) -> RecoveryAttemptKey
{
    return {.run_id = run.run.id, .attempt_number = run.attempts.back().attempt.attempt_number};
}

void execute(Database& database, std::string_view sql)
{
    INFO(sql);
    Query query{database};
    REQUIRE(query.exec(sql));
}

auto seed_running(RecoveryFixture& fixture,
                  RecoveryPolicy   policy         = RecoveryPolicy::RetryInterrupted,
                  AttemptNumber    prior_failures = 0,
                  std::string      mode           = "reschedule",
                  RunOrigin        origin         = RunOrigin::Scheduled,
                  JobType          type           = JobType::Cli) -> RecoveryRunFixture
{
    auto queue                                    = recovery_queue(recovery_id(1), QueueState::Active, policy);
    auto job                                      = fixture.make_job(recovery_id(2), queue.id, type);
    job.attributes.at("retry.initial_delay").data = Duration{4s};
    job.attributes.at("retry.max_delay").data     = Duration{20s};
    job.attributes.at("retry.mode").data          = std::move(mode);
    fixture.insert_queue(queue);
    fixture.insert_job(job);
    if (origin == RunOrigin::Manual) {
        fixture.insert_run(fixture.make_run(recovery_id(4), job));
    }
    auto original = fixture.make_run(recovery_id(3), job, RunState::Running, prior_failures, origin);
    if (prior_failures != 0) {
        original.attempts.front().output = jb::jobu::detail::AttemptOutput{.stdout_bytes = ByteBuffer{std::byte{0x41}},
                                                                           .stderr_bytes = ByteBuffer{},
                                                                           .stdout_truncated = true};
    }
    fixture.insert_run(original);
    return original;
}

auto expected_recovery(RecoveryRunFixture original, UtcTimePoint time, RetryDecision const& decision)
    -> RecoveryRunFixture
{
    auto result = parse_json(R"({"reason":"daemon_interrupted","outcome_unknown":true})");
    REQUIRE(result);
    auto& latest                = original.attempts.back();
    latest.attempt.state        = AttemptState::Completed;
    latest.attempt.outcome      = AttemptOutcome::Interrupted;
    latest.attempt.completed_at = time;
    latest.attempt.result       = *result;
    latest.output               = jb::jobu::detail::AttemptOutput{.stdout_bytes = ByteBuffer{},
                                                                  .stderr_bytes = ByteBuffer{},
                                                                  .capture_lost = true};
    if (decision.retry) {
        original.run.state       = RunState::RetryWait;
        original.run.runnable_at = decision.retry->due_at;
    }
    else {
        original.run.state        = RunState::Interrupted;
        original.run.completed_at = time;
        original.run.result       = *result;
    }
    return original;
}

auto apply_recovery(RecoveryRepository& repository, RecoveryAttemptKey const& key, UtcTimePoint time)
    -> Result<RetryDecision, Error>
{
    // Model only one caller-owned interruption transaction. Later stages add recurrence and
    // suspension repair before its commit, and orchestrate scans over the entire database.
    auto decision = repository.find_retry_decision(key, time);
    if (!decision) {
        return decision;
    }
    auto result = repository.interrupt_attempt(key, time);
    if (result) {
        result = decision->retry ? repository.set_run_retry_wait(key, time, decision->retry->due_at)
                                 : repository.set_run_interrupted(key, time);
    }
    if (!result) {
        return Result<RetryDecision, Error>::failure(std::move(result).error());
    }
    return decision;
}

auto commit_recovery(RecoveryFixture& fixture, RecoveryAttemptKey const& key, UtcTimePoint time) -> RetryDecision
{
    auto transaction = Transaction::begin(fixture.database);
    REQUIRE(transaction);
    RecoveryRepository repository{fixture.database, fixture.registry};
    auto               result = apply_recovery(repository, key, time);
    REQUIRE(result);
    REQUIRE(transaction->commit());
    return *result;
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

TEST_CASE("Recovery policy commits retry or terminal interruption without speculative attempts",
          "[jobu][recovery][sqlite]")
{
    auto const      policy   = GENERATE(RecoveryPolicy::FailInterrupted, RecoveryPolicy::RetryInterrupted);
    auto const      failures = GENERATE(AttemptNumber{0}, AttemptNumber{1}, AttemptNumber{2});
    auto const      type     = GENERATE(JobType::Cli, JobType::Http);
    auto const      origin   = GENERATE(RunOrigin::Scheduled, RunOrigin::Manual);
    auto const      time     = GENERATE(UtcTimePoint{5s}, UtcTimePoint{100s});
    RecoveryFixture fixture;
    auto            original = seed_running(fixture, policy, failures, "reschedule", origin, type);
    auto            decision = commit_recovery(fixture, key_for(original), time);
    CHECK(decision.retry.has_value() == (policy == RecoveryPolicy::RetryInterrupted && failures < 2));
    if (decision.retry) {
        CHECK(decision.retry->due_at == time + 4s);
    }
    auto expected = expected_recovery(original, time, decision);
    fixture.require_run(expected);
    fixture.reopen();
    fixture.require_run(expected);
    require_valid(fixture);

    SchedulerRepository scheduler{fixture.database, fixture.registry};
    auto                barriers = scheduler.list_manual_barriers(256, {});
    REQUIRE(barriers);
    CHECK(barriers->size() == (origin == RunOrigin::Manual && decision.retry ? 1U : 0U));
}

TEST_CASE("Recovery policy reads the current queue but keeps the run retry snapshot", "[jobu][recovery][sqlite]")
{
    auto const         policy = GENERATE(RecoveryPolicy::FailInterrupted, RecoveryPolicy::RetryInterrupted);
    RecoveryFixture    fixture;
    auto               original = seed_running(fixture, policy, 1);
    RecoveryRepository repository{fixture.database, fixture.registry};
    REQUIRE(repository.find_run(original.run.id));
    RetryDecision decision;
    {
        auto transaction = Transaction::begin(fixture.database);
        REQUIRE(transaction);
        // Simulate edits after the earlier candidate read. Neither new defaults nor new definition
        // attributes may replace the retry allowance/delay of the already-running snapshot.
        auto edited                           = original.run.attributes;
        edited.at("retry.max_attempts").data  = std::int64_t{1};
        edited.at("retry.initial_delay").data = Duration{15s};
        auto document = encode_and_serialize_attribute_document(fixture.registry,
                                                                edited,
                                                                AttributeScope::Job,
                                                                AttributeDocumentMode::Materialized);
        REQUIRE(document);
        {
            Query query{fixture.database};
            REQUIRE(query.prepare("UPDATE jobu_jobs SET revision = revision + 1, attributes_json = :attributes"));
            REQUIRE(query.bind_value(":attributes", make_text(document->serialized())));
            REQUIRE(query.exec());
        }
        execute(fixture.database,
                policy == RecoveryPolicy::FailInterrupted
                    ? "UPDATE jobu_queues SET recovery_policy = 'retry_interrupted'"
                    : "UPDATE jobu_queues SET recovery_policy = 'fail_interrupted'");
        auto result = apply_recovery(repository, key_for(original), UtcTimePoint{100s});
        REQUIRE(result);
        decision = *result;
        REQUIRE(transaction->commit());
    }
    CHECK(decision.retry.has_value() == (policy == RecoveryPolicy::FailInterrupted));
    fixture.reopen();
    fixture.require_run(expected_recovery(original, UtcTimePoint{100s}, decision));
    require_valid(fixture);
}

TEST_CASE("Recovered retries preserve suspension gates and blocking capacity rules", "[jobu][recovery][sqlite]")
{
    auto const      mode        = GENERATE(std::string{"blocking"}, std::string{"reschedule"});
    auto const      origin      = GENERATE(RunOrigin::Scheduled, RunOrigin::Manual);
    auto const      job_state   = GENERATE(std::string{"active"}, std::string{"suspending"}, std::string{"suspended"});
    auto const      queue_state = GENERATE(std::string{"active"}, std::string{"suspending"}, std::string{"suspended"});
    RecoveryFixture fixture;
    auto            original = seed_running(fixture, RecoveryPolicy::RetryInterrupted, 0, mode, origin);

    // Suspending owners can have Running work; a suspended job can still have Running manual
    // work. Other suspended states become valid once the old Running attempt is interrupted.
    if (job_state != "suspended" || origin == RunOrigin::Manual) {
        execute(fixture.database, "UPDATE jobu_jobs SET state = '" + job_state + "'");
    }
    if (queue_state != "suspended") {
        execute(fixture.database, "UPDATE jobu_queues SET state = '" + queue_state + "'");
    }
    auto decision = commit_recovery(fixture, key_for(original), UtcTimePoint{100s});
    REQUIRE(decision.retry);
    CHECK(decision.retry->due_at == UtcTimePoint{104s});
    execute(fixture.database, "UPDATE jobu_jobs SET state = '" + job_state + "'");
    execute(fixture.database, "UPDATE jobu_queues SET state = '" + queue_state + "'");
    auto expected = expected_recovery(original, UtcTimePoint{100s}, decision);
    fixture.reopen();
    fixture.require_run(expected);
    require_valid(fixture);

    SchedulerRepository scheduler{fixture.database, fixture.registry};
    auto const          eligible = queue_state == "active" && (job_state == "active" || origin == RunOrigin::Manual);
    auto const          occupies = eligible && mode == "blocking";
    auto                rows     = scheduler.list_capacity_rows(256, {});
    REQUIRE(rows);
    REQUIRE(rows->size() == 1);
    CHECK(rows->front().usage == CapacityUsage{.queue_slots = occupies ? 1U : 0U});
    CHECK(rows->front().blocking_retry.has_value() == occupies);
    auto future = scheduler.earliest_future_runnable(JobType::Cli, UtcTimePoint{100s});
    REQUIRE(future);
    CHECK(future->has_value() == eligible);
    if (*future) {
        CHECK(**future == UtcTimePoint{104s});
    }
    auto runnable = scheduler.list_runnable(original.run.queue_id, JobType::Cli, UtcTimePoint{104s}, 256);
    REQUIRE(runnable);
    CHECK(runnable->size() == (eligible ? 1U : 0U));
    auto context = scheduler.find_dispatch_context(original.run.id, UtcTimePoint{104s});
    REQUIRE(context);
    CHECK(context->has_value() == eligible);

    // Resumption does not rewrite the stored due time or spend another attempt.
    execute(fixture.database, "UPDATE jobu_jobs SET state = 'active'");
    execute(fixture.database, "UPDATE jobu_queues SET state = 'active'");
    context = scheduler.find_dispatch_context(original.run.id, UtcTimePoint{200s});
    REQUIRE(context);
    REQUIRE(*context);
    CHECK((**context).next_attempt == 2);
    fixture.require_run(expected);
}

TEST_CASE("Normal dispatch creates the next recovered attempt from the original snapshot", "[jobu][recovery][sqlite]")
{
    auto const      mode = GENERATE(std::string{"blocking"}, std::string{"reschedule"});
    auto const      type = GENERATE(JobType::Cli, JobType::Http);
    RecoveryFixture fixture;
    auto original = seed_running(fixture, RecoveryPolicy::RetryInterrupted, 1, mode, RunOrigin::Scheduled, type);
    auto decision = commit_recovery(fixture, key_for(original), UtcTimePoint{100s});
    REQUIRE(decision.retry);
    auto expected = expected_recovery(original, UtcTimePoint{100s}, decision);
    fixture.reopen();
    fixture.require_run(expected);

    FakeAttemptExecutor executor;
    executor.set_available(type, true);
    RejectingSecretProvider secrets;
    auto                    early = dispatch_selected(fixture.database,
                                                      fixture.registry,
                                                      executor,
                                                      secrets,
                                                      original.run.id,
                                                      UtcTimePoint{103s},
                                                      [](AttemptCompletion const&) {});
    REQUIRE(early);
    CHECK_FALSE(*early);
    CHECK(executor.start_requests().empty());
    fixture.require_run(expected);

    auto dispatched = dispatch_selected(fixture.database,
                                        fixture.registry,
                                        executor,
                                        secrets,
                                        original.run.id,
                                        UtcTimePoint{104s},
                                        [](AttemptCompletion const&) {});
    REQUIRE(dispatched);
    REQUIRE(*dispatched);
    CHECK((**dispatched).key.attempt_number == 3);
    REQUIRE(executor.start_requests().size() == 1);
    auto const& request = executor.start_requests().front();
    CHECK(request.key.run_id == original.run.id);
    auto requested_attributes = encode_attribute_document(fixture.registry,
                                                          request.attributes,
                                                          AttributeScope::Job,
                                                          AttributeDocumentMode::Materialized);
    auto original_attributes  = encode_attribute_document(fixture.registry,
                                                          original.run.attributes,
                                                          AttributeScope::Job,
                                                          AttributeDocumentMode::Materialized);
    REQUIRE(requested_attributes);
    REQUIRE(original_attributes);
    CHECK(*requested_attributes == *original_attributes);
    CHECK(request.payload == original.run.payload);
    expected.run.state = RunState::Running;
    expected.attempts.push_back({
        .attempt = {.run_id         = original.run.id,
                    .attempt_number = 3,
                    .due_at         = UtcTimePoint{104s},
                    .started_at     = UtcTimePoint{104s},
                    .state          = AttemptState::Running}
    });
    fixture.require_run(expected);
    require_valid(fixture);
}

TEST_CASE("Recovery retry writes roll back together and can be retried after reopening", "[jobu][recovery][sqlite]")
{
    auto const table =
        GENERATE(std::string{"jobu_attempts"}, std::string{"jobu_attempt_output"}, std::string{"jobu_runs"});
    auto const      ignore = GENERATE(false, true);
    RecoveryFixture fixture;
    auto            original  = seed_running(fixture);
    auto const*     operation = table == "jobu_attempt_output" ? "INSERT" : "UPDATE";
    execute(fixture.database,
            "CREATE TRIGGER fail_retry BEFORE " + std::string{operation} + " ON " + table + " BEGIN SELECT RAISE(" +
                (ignore ? "IGNORE" : "ABORT, 'private sentinel'") + "); END");
    {
        auto transaction = Transaction::begin(fixture.database);
        REQUIRE(transaction);
        RecoveryRepository repository{fixture.database, fixture.registry};
        auto               result = apply_recovery(repository, key_for(original), UtcTimePoint{100s});
        REQUIRE_FALSE(result);
        CHECK(result.error().message.find("sentinel") == std::string::npos);
        CHECK(result.error().detail.find("sentinel") == std::string::npos);
        if (ignore) {
            CHECK(
                (result.error().code == "jobu.recovery.invariant" || result.error().code == "jobu.storage.invariant"));
        }
        else {
            CHECK(result.error().code == "db.constraint");
        }
    }
    fixture.reopen();
    fixture.require_run(original);
    execute(fixture.database, "DROP TRIGGER fail_retry");
    auto decision = commit_recovery(fixture, key_for(original), UtcTimePoint{100s});
    REQUIRE(decision.retry);
    fixture.reopen();
    fixture.require_run(expected_recovery(original, UtcTimePoint{100s}, decision));
}

TEST_CASE("Recovery retry supports explicit and RAII rollback after a successful transition",
          "[jobu][recovery][sqlite]")
{
    auto const      explicit_rollback = GENERATE(false, true);
    RecoveryFixture fixture;
    auto            original = seed_running(fixture);
    {
        auto transaction = Transaction::begin(fixture.database);
        REQUIRE(transaction);
        RecoveryRepository repository{fixture.database, fixture.registry};
        auto               result = apply_recovery(repository, key_for(original), UtcTimePoint{100s});
        REQUIRE(result);
        REQUIRE(result->retry);
        fixture.require_run(expected_recovery(original, UtcTimePoint{100s}, *result));
        if (explicit_rollback) {
            REQUIRE(transaction->rollback());
        }
    }
    fixture.reopen();
    fixture.require_run(original);
}

TEST_CASE("Recovery rejects deleted Running owners rather than inventing a terminal repair", "[jobu][recovery][sqlite]")
{
    auto const      policy      = GENERATE(RecoveryPolicy::FailInterrupted, RecoveryPolicy::RetryInterrupted);
    auto const      job_deleted = GENERATE(false, true);
    RecoveryFixture fixture;
    auto            original = seed_running(fixture, policy);
    {
        auto transaction = Transaction::begin(fixture.database);
        REQUIRE(transaction);
        execute(fixture.database,
                job_deleted
                    ? "UPDATE jobu_jobs SET state = 'deleted', deleted_at_us = 90000000"
                    : "UPDATE jobu_queues SET state = 'deleted', deleted_name = name, deleted_at_us = 90000000");
        RecoveryRepository repository{fixture.database, fixture.registry};
        auto               result = repository.find_retry_decision(key_for(original), UtcTimePoint{100s});
        REQUIRE_FALSE(result);
        CHECK(result.error().code == "jobu.recovery.invariant");
        CHECK(result.error().detail == "reason=nonterminal_ownership");
    }
    fixture.reopen();
    fixture.require_run(original);
}

TEST_CASE("Recovery retry planning detects stale keys and overflow before any write", "[jobu][recovery][sqlite]")
{
    auto const      stale = GENERATE(false, true);
    RecoveryFixture fixture;
    auto            original = seed_running(fixture, RecoveryPolicy::RetryInterrupted, 1);
    auto            key      = key_for(original);
    if (stale) {
        --key.attempt_number;
    }
    auto const time = stale ? UtcTimePoint{100s} : UtcTimePoint::max() - 1s;
    {
        auto transaction = Transaction::begin(fixture.database);
        REQUIRE(transaction);
        RecoveryRepository repository{fixture.database, fixture.registry};
        auto               result = repository.find_retry_decision(key, time);
        REQUIRE_FALSE(result);
        CHECK(result.error().code == (stale ? "jobu.recovery.invariant" : "jobu.retry.out_of_range"));
        // Commit deliberately: planning failure must not rely on rollback to hide partial writes.
        REQUIRE(transaction->commit());
    }
    fixture.reopen();
    fixture.require_run(original);
}

TEST_CASE("Recovery checks microsecond storage boundaries before writing", "[jobu][recovery][sqlite]")
{
    RecoveryFixture fixture;
    auto            original = seed_running(fixture);
    auto const      earliest =
        UtcTimePoint{std::chrono::ceil<std::chrono::microseconds>(UtcTimePoint::min().time_since_epoch())};
    if (earliest != UtcTimePoint::min()) {
        // A finer UTC clock has a fractional lower-bound microsecond that storage cannot reopen.
        auto transaction = Transaction::begin(fixture.database);
        REQUIRE(transaction);
        RecoveryRepository repository{fixture.database, fixture.registry};
        auto               result = repository.find_retry_decision(key_for(original), UtcTimePoint::min());
        REQUIRE_FALSE(result);
        CHECK(result.error().code == "jobu.recovery.invariant");
        CHECK(result.error().detail == "reason=timestamp_out_of_range");
        REQUIRE(transaction->commit());
    }
    fixture.reopen();
    fixture.require_run(original);

    // Large clock regression is valid when both stored timestamps are representable.
    auto decision = commit_recovery(fixture, key_for(original), earliest);
    REQUIRE(decision.retry);
    CHECK(decision.retry->due_at == earliest + 4s);
    fixture.reopen();
    fixture.require_run(expected_recovery(original, earliest, decision));
    require_valid(fixture);
}

TEST_CASE("Recovery RetryWait setter guards the interrupted attempt and current ownership", "[jobu][recovery][sqlite]")
{
    auto const      scenario = GENERATE(std::string{"unfinished"},
                                        std::string{"key"},
                                        std::string{"timestamp"},
                                        std::string{"capture"},
                                        std::string{"policy"},
                                        std::string{"owner"},
                                        std::string{"duplicate"},
                                        std::string{"due"});
    RecoveryFixture fixture;
    auto            original = seed_running(fixture, RecoveryPolicy::RetryInterrupted, 1);
    {
        auto transaction = Transaction::begin(fixture.database);
        REQUIRE(transaction);
        RecoveryRepository repository{fixture.database, fixture.registry};
        auto               key      = key_for(original);
        auto               time     = UtcTimePoint{100s};
        auto               decision = repository.find_retry_decision(key, time);
        REQUIRE(decision);
        REQUIRE(decision->retry);
        auto due = decision->retry->due_at;
        if (scenario != "unfinished") {
            REQUIRE(repository.interrupt_attempt(key, time));
        }
        if (scenario == "key") {
            --key.attempt_number;
        }
        else if (scenario == "timestamp") {
            time += 1us;
        }
        else if (scenario == "capture") {
            execute(fixture.database, "UPDATE jobu_attempt_output SET capture_lost = 0 WHERE attempt_number = 2");
        }
        else if (scenario == "policy") {
            execute(fixture.database, "UPDATE jobu_queues SET recovery_policy = 'fail_interrupted'");
        }
        else if (scenario == "owner") {
            execute(fixture.database, "UPDATE jobu_jobs SET state = 'deleted', deleted_at_us = 90000000");
        }
        else if (scenario == "duplicate") {
            REQUIRE(repository.set_run_retry_wait(key, time, due));
        }
        else if (scenario == "due") {
            due = time - 1us;
        }
        auto result = repository.set_run_retry_wait(key, time, due);
        REQUIRE_FALSE(result);
        CHECK(result.error().code == "jobu.recovery.invariant");
    }
    fixture.reopen();
    fixture.require_run(original);
}

TEST_CASE("Scheduler retry history still rejects successful cancelled and unfinished attempts",
          "[jobu][recovery][sqlite]")
{
    auto const      outcome = GENERATE(std::string{"succeeded"}, std::string{"cancelled"}, std::string{"running"});
    RecoveryFixture fixture;
    auto            original = seed_running(fixture);
    REQUIRE(commit_recovery(fixture, key_for(original), UtcTimePoint{100s}).retry);
    // Deliberately corrupt the history after valid recovery; widening the accepted history
    // to Failed/Interrupted must not weaken any other retry eligibility invariant.
    if (outcome == "running") {
        execute(
            fixture.database,
            "UPDATE jobu_attempts SET state = 'running', outcome = NULL, completed_at_us = NULL, result_json = NULL");
    }
    else {
        execute(fixture.database, "UPDATE jobu_attempts SET outcome = '" + outcome + "'");
    }
    SchedulerRepository scheduler{fixture.database, fixture.registry};
    CHECK_FALSE(scheduler.list_capacity_rows(256, {}));
    CHECK_FALSE(scheduler.list_runnable(original.run.queue_id, JobType::Cli, UtcTimePoint{104s}, 256));
    CHECK_FALSE(scheduler.earliest_future_runnable(JobType::Cli, UtcTimePoint{100s}));
    CHECK_FALSE(scheduler.find_dispatch_context(original.run.id, UtcTimePoint{104s}));
}
