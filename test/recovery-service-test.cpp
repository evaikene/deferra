#include "recovery.hpp"

#include "recovery_priv.hpp"
#include "support/fake_attempt_executor.hpp"
#include "support/fake_cron_engine.hpp"
#include "support/fake_event_loop_backend.hpp"
#include "support/fake_time_source.hpp"
#include "support/recovery_fixture.hpp"
#include "support/rejecting_secret_provider.hpp"
#include "support/sequence_uuid_generator.hpp"

#include "attempt_repository_priv.hpp"
#include "byte_buffer.hpp"
#include "job_repository_priv.hpp"
#include "json.hpp"
#include "query.hpp"
#include "queue_repository_priv.hpp"
#include "run_repository_priv.hpp"
#include "scheduler.hpp"
#include "scheduler_repository_priv.hpp"
#include "secret_provider_priv.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#  include <crt_externs.h>
#endif
#include <csignal> // IWYU pragma: keep - SIGKILL for the child-process recovery fixture
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace jb::core;
using namespace jb::db;
using namespace jb::jobu;
using namespace jb::jobu::detail;
using namespace jb::test;
using namespace std::chrono_literals;

namespace {

auto process_environment() noexcept -> char**
{
#if defined(__APPLE__)
    return *_NSGetEnviron();
#else
    return environ;
#endif
}

struct ServiceFixture {
    RecoveryFixture       storage;
    FakeCronEngine        cron;
    FakeTimeSource        time;
    SequenceUuidGenerator generator{
        {recovery_id(1000), recovery_id(1001), recovery_id(1002)}
    };

    ServiceFixture() { time.set_utc(UtcTimePoint{120s}); }

    auto recover(std::size_t batch = 1, std::function<bool()> const& stop = {}) -> Result<RecoveryReport, Error>
    {
        return detail::recover_startup(storage.database,
                                       storage.registry,
                                       cron,
                                       generator,
                                       time,
                                       {.scan_batch_size = batch},
                                       stop);
    }

    auto run(Uuid id) -> JobRun
    {
        RunRepository runs{storage.database, storage.registry};
        auto          found = runs.find_by_id(id);
        REQUIRE(found);
        REQUIRE(*found);
        return **found;
    }

    auto job(Uuid id) -> JobDefinition
    {
        JobRepository jobs{storage.database, storage.registry};
        auto          found = jobs.find_by_id(id, true);
        REQUIRE(found);
        REQUIRE(*found);
        return **found;
    }

    auto queue(Uuid id) -> Queue
    {
        QueueRepository queues{storage.database, storage.registry};
        auto            found = queues.find_by_id(id, true);
        REQUIRE(found);
        REQUIRE(*found);
        return **found;
    }

    auto recurring(Uuid id, Uuid queue_id, JobState state = JobState::Active) -> JobDefinition
    {
        auto definition     = storage.make_job(id, queue_id);
        definition.schedule = CronSchedule{.expression = "* * * * *", .timezone = "UTC"};
        definition.state    = state;
        cron.set_occurrences(std::get<CronSchedule>(definition.schedule),
                             {UtcTimePoint{120s}, UtcTimePoint{180s}, UtcTimePoint{300s}});
        storage.insert_job(definition);
        return definition;
    }
};

void require_report(RecoveryReport const& actual, RecoveryReport const& expected = {})
{
    CHECK(actual.interrupted_attempts == expected.interrupted_attempts);
    CHECK(actual.retrying_runs == expected.retrying_runs);
    CHECK(actual.terminal_runs == expected.terminal_runs);
    CHECK(actual.inserted_successors == expected.inserted_successors);
    CHECK(actual.suspended_jobs == expected.suspended_jobs);
    CHECK(actual.suspended_queues == expected.suspended_queues);
}

void execute(Database& database, std::string_view sql)
{
    Query query{database};
    REQUIRE(query.exec(sql));
}

void require_interrupted(ServiceFixture& fixture, RecoveryRunFixture expected, bool retry)
{
    auto document = parse_json(R"({"reason":"daemon_interrupted","outcome_unknown":true})");
    REQUIRE(document);
    auto& last                = expected.attempts.back();
    last.attempt.state        = AttemptState::Completed;
    last.attempt.outcome      = AttemptOutcome::Interrupted;
    last.attempt.completed_at = UtcTimePoint{120s};
    last.attempt.result       = *document;
    last.output               = jb::jobu::detail::AttemptOutput{.stdout_bytes = ByteBuffer{},
                                                                .stderr_bytes = ByteBuffer{},
                                                                .capture_lost = true};
    expected.run.state        = retry ? RunState::RetryWait : RunState::Interrupted;
    if (retry) {
        // This scenario explicitly configures a fixed five-second recovery retry delay.
        expected.run.runnable_at = UtcTimePoint{125s};
    }
    else {
        expected.run.completed_at = UtcTimePoint{120s};
        expected.run.result       = *document;
    }
    fixture.storage.require_run(expected);
}

} // namespace

TEST_CASE("Recovery validates options and an empty database", "[jobu][recovery][sqlite]")
{
    ServiceFixture fixture;
    auto batch  = GENERATE(std::size_t{0}, std::size_t{1}, std::size_t{256}, std::size_t{4096}, std::size_t{4097});
    auto result = jb::jobu::recover_startup(fixture.storage.database,
                                            fixture.storage.registry,
                                            fixture.cron,
                                            fixture.generator,
                                            fixture.time,
                                            {.scan_batch_size = batch});
    if (batch == 0 || batch > 4096) {
        REQUIRE_FALSE(result);
        CHECK(result.error().code == "jobu.recovery.invalid_options");
    }
    else {
        REQUIRE(result);
        require_report(*result);
    }
}

TEST_CASE("Recovery pages interruption units and preserves complete snapshots", "[jobu][recovery][sqlite]")
{
    auto           policy = GENERATE(RecoveryPolicy::FailInterrupted, RecoveryPolicy::RetryInterrupted);
    auto           batch  = GENERATE(std::size_t{1}, std::size_t{2}, std::size_t{3});
    ServiceFixture fixture;
    auto           queue = recovery_queue(recovery_id(1), QueueState::Active, policy);
    fixture.storage.insert_queue(queue);
    auto originals = std::vector<RecoveryRunFixture>{};
    // Reverse insertion order crosses UUID byte boundaries and must not affect keyset coverage.
    for (auto suffix : {300U, 256U, 255U}) {
        auto job                                      = fixture.storage.make_job(recovery_id(suffix), queue.id);
        job.attributes.at("retry.initial_delay").data = Duration{5s};
        fixture.storage.insert_job(job);
        auto original =
            fixture.storage.make_run(recovery_id(suffix + 100), job, RunState::Running, suffix == 300 ? 2 : 0);
        fixture.storage.insert_run(original);
        originals.push_back(std::move(original));
    }
    auto result = fixture.recover(batch);
    REQUIRE(result);
    auto retries = policy == RecoveryPolicy::RetryInterrupted ? 2U : 0U;
    require_report(*result, {.interrupted_attempts = 3, .retrying_runs = retries, .terminal_runs = 3 - retries});
    for (auto const& original : originals) {
        require_interrupted(fixture,
                            original,
                            policy == RecoveryPolicy::RetryInterrupted && original.attempts.size() < 3);
    }
    fixture.storage.reopen();
    auto again = fixture.recover(batch);
    REQUIRE(again);
    require_report(*again);
    for (auto const& original : originals) {
        require_interrupted(fixture,
                            original,
                            policy == RecoveryPolicy::RetryInterrupted && original.attempts.size() < 3);
    }
}

TEST_CASE("Recovery repairs recurring work and all drained owner shapes", "[jobu][recovery][sqlite]")
{
    ServiceFixture fixture;
    auto           queue = recovery_queue(recovery_id(1), QueueState::Suspending);
    auto           empty = recovery_queue(recovery_id(2), QueueState::Suspending);
    fixture.storage.insert_queue(queue);
    fixture.storage.insert_queue(empty);
    auto running_job = fixture.recurring(recovery_id(10), queue.id, JobState::Suspending);
    auto missing_job = fixture.recurring(recovery_id(11), queue.id, JobState::Suspending);
    auto suspended   = fixture.recurring(recovery_id(12), queue.id, JobState::Suspended);
    auto once        = fixture.storage.make_job(recovery_id(13), queue.id);
    once.state       = JobState::Suspending;
    fixture.storage.insert_job(once);
    auto original = fixture.storage.make_run(recovery_id(20), running_job, RunState::Running);
    fixture.storage.insert_run(original);

    auto report = fixture.recover();
    REQUIRE(report);
    require_report(*report,
                   {.interrupted_attempts = 1,
                    .terminal_runs        = 1,
                    .inserted_successors  = 3,
                    .suspended_jobs       = 3,
                    .suspended_queues     = 2});
    require_interrupted(fixture, original, false);
    RunRepository runs{fixture.storage.database, fixture.storage.registry};
    for (auto const& job : {running_job, missing_job, suspended}) {
        auto successor = runs.find_schedule_owned(job.id);
        REQUIRE(successor);
        REQUIRE(*successor);
        CHECK((*successor)->planned_at == UtcTimePoint{180s});
        CHECK((*successor)->job_revision == job.revision);
        CHECK(fixture.job(job.id).state == JobState::Suspended);
    }
    CHECK(fixture.job(running_job.id).revision == running_job.revision + 1);
    CHECK(fixture.job(missing_job.id).revision == missing_job.revision + 1);
    CHECK(fixture.job(suspended.id).revision == suspended.revision);
    auto no_once = runs.find_schedule_owned(once.id);
    REQUIRE(no_once);
    CHECK_FALSE(*no_once);
    CHECK(fixture.queue(queue.id).state == QueueState::Suspended);
    CHECK(fixture.queue(empty.id).state == QueueState::Suspended);
    auto again = fixture.recover();
    REQUIRE(again);
    require_report(*again);
}

TEST_CASE("Recovery preserves manual retry barriers and overdue scheduled snapshots", "[jobu][recovery][sqlite]")
{
    auto           retry = GENERATE(false, true);
    ServiceFixture fixture;
    auto           queue = recovery_queue(recovery_id(1),
                                          QueueState::Active,
                                          retry ? RecoveryPolicy::RetryInterrupted : RecoveryPolicy::FailInterrupted);
    fixture.storage.insert_queue(queue);
    auto job       = fixture.recurring(recovery_id(2), queue.id);
    auto scheduled = fixture.storage.make_run(recovery_id(3), job);
    fixture.storage.insert_run(scheduled);
    auto manual = fixture.storage.make_run(recovery_id(4), job, RunState::Running, 0, RunOrigin::Manual);
    fixture.storage.insert_run(manual);

    auto report = fixture.recover();
    REQUIRE(report);
    require_report(*report,
                   {.interrupted_attempts = 1, .retrying_runs = retry ? 1U : 0U, .terminal_runs = retry ? 0U : 1U});
    fixture.storage.require_run(scheduled);
    SchedulerRepository scheduler{fixture.storage.database, fixture.storage.registry};
    auto                barriers = scheduler.list_manual_barriers(10, {});
    REQUIRE(barriers);
    REQUIRE(barriers->size() == (retry ? 1U : 0U));
    if (retry) {
        CHECK(barriers->front().run_id == manual.run.id);
    }
    CHECK(fixture.cron.next_calls().empty());
}

TEST_CASE("Recovery checks every family before committing any repair", "[jobu][recovery][sqlite]")
{
    auto const*    malformed = GENERATE("attempt", "output", "job", "queue");
    ServiceFixture fixture;
    auto           queue = recovery_queue(recovery_id(1));
    fixture.storage.insert_queue(queue);
    auto job = fixture.storage.make_job(recovery_id(2), queue.id);
    fixture.storage.insert_job(job);
    auto original = fixture.storage.make_run(recovery_id(3), job, RunState::Running);
    fixture.storage.insert_run(original);
    auto historical_job = fixture.storage.make_job(recovery_id(4), queue.id);
    fixture.storage.insert_job(historical_job);
    auto historical = fixture.storage.make_run(recovery_id(5), historical_job, RunState::Failed, 1);
    fixture.storage.insert_run(historical);
    if (std::string_view{malformed} == "attempt") {
        // The latest attempt is valid; a malformed earlier row needs the full attempt scan.
        execute(fixture.storage.database,
                "UPDATE jobu_attempts SET result_json = '[]' WHERE attempt_number = 1 AND state = 'completed'");
    }
    else if (std::string_view{malformed} == "output") {
        execute(fixture.storage.database, "PRAGMA foreign_keys = OFF");
        execute(fixture.storage.database,
                "INSERT INTO jobu_attempt_output SELECT run_id, 99, X'', X'', 0, 0, 1 FROM jobu_attempts LIMIT 1");
    }
    else if (std::string_view{malformed} == "job") {
        auto orphan = fixture.storage.make_job(recovery_id(50), queue.id);
        fixture.storage.insert_job(orphan);
        execute(fixture.storage.database,
                "UPDATE jobu_jobs SET payload_json = '[]' WHERE id NOT IN (SELECT job_id FROM jobu_runs)");
    }
    else {
        auto empty = recovery_queue(recovery_id(60));
        fixture.storage.insert_queue(empty);
        execute(fixture.storage.database,
                "UPDATE jobu_queues SET defaults_json = '[]' WHERE id NOT IN (SELECT queue_id FROM jobu_jobs)");
    }
    auto report = fixture.recover();
    REQUIRE_FALSE(report);
    fixture.storage.require_run(original);
}

TEST_CASE("Recovery cancellation rolls back the complete current repair unit", "[jobu][recovery][sqlite]")
{
    ServiceFixture fixture;
    auto           queue = recovery_queue(recovery_id(1), QueueState::Suspending);
    fixture.storage.insert_queue(queue);
    auto job      = fixture.recurring(recovery_id(2), queue.id, JobState::Suspending);
    auto original = fixture.storage.make_run(recovery_id(3), job, RunState::Running);
    fixture.storage.insert_run(original);
    bool saw_repairs = false;
    auto report      = fixture.recover(1, [&] {
        // The predicate only observes state. Stop after all writes, at the precommit boundary.
        saw_repairs = fixture.run(original.run.id).state == RunState::Interrupted;
        if (saw_repairs) {
            CHECK(fixture.job(job.id).state == JobState::Suspended);
            CHECK(fixture.queue(queue.id).state == QueueState::Suspended);
        }
        return saw_repairs;
    });
    REQUIRE_FALSE(report);
    CHECK(report.error().code == "jobu.recovery.cancelled");
    REQUIRE(saw_repairs);
    fixture.storage.reopen();
    fixture.storage.require_run(original);
    CHECK(fixture.job(job.id).state == JobState::Suspending);
    CHECK(fixture.queue(queue.id).state == QueueState::Suspending);
    RunRepository runs{fixture.storage.database, fixture.storage.registry};
    auto          current = runs.find_schedule_owned(job.id);
    REQUIRE(current);
    REQUIRE(*current);
    CHECK((*current)->id == original.run.id);
    auto resumed = fixture.recover();
    REQUIRE(resumed);
    require_report(*resumed,
                   {.interrupted_attempts = 1,
                    .terminal_runs        = 1,
                    .inserted_successors  = 1,
                    .suspended_jobs       = 1,
                    .suspended_queues     = 1});
}

TEST_CASE("Recovery restart retains committed units and retries only unfinished units", "[jobu][recovery][sqlite]")
{
    ServiceFixture fixture;
    auto           queue = recovery_queue(recovery_id(1));
    fixture.storage.insert_queue(queue);
    auto first_job = fixture.storage.make_job(recovery_id(2), queue.id);
    fixture.storage.insert_job(first_job);
    auto second_job = fixture.recurring(recovery_id(3), queue.id);
    auto first      = fixture.storage.make_run(recovery_id(4), first_job, RunState::Running);
    auto second     = fixture.storage.make_run(recovery_id(5), second_job, RunState::Running);
    fixture.storage.insert_run(first);
    fixture.storage.insert_run(second);

    // Failure in the second unit must roll back its attempt/output/run writes while preserving
    // the first unit's commit. Closing/reopening models restart with no invocation-local state.
    fixture.cron.set_next_error(
        Error{.code = "test.cron.failure", .message = "private payload", .detail = "private SQL"});
    auto failed = fixture.recover();
    REQUIRE_FALSE(failed);
    CHECK(failed.error().code == "test.cron.failure");
    CHECK(failed.error().message.find("private") == std::string::npos);
    CHECK(failed.error().detail.find("private") == std::string::npos);
    fixture.storage.reopen();
    require_interrupted(fixture, first, false);
    fixture.storage.require_run(second);
    fixture.cron.set_next_error({});
    auto resumed = fixture.recover();
    REQUIRE(resumed);
    require_report(*resumed, {.interrupted_attempts = 1, .terminal_runs = 1, .inserted_successors = 1});
    require_interrupted(fixture, first, false);
    require_interrupted(fixture, second, false);
    auto again = fixture.recover();
    REQUIRE(again);
    require_report(*again);
}

TEST_CASE("Recovery cancellation between pages retains earlier commits", "[jobu][recovery][sqlite]")
{
    ServiceFixture fixture;
    auto           queue = recovery_queue(recovery_id(1));
    fixture.storage.insert_queue(queue);
    auto job = fixture.storage.make_job(recovery_id(2), queue.id);
    fixture.storage.insert_job(job);
    auto original = fixture.storage.make_run(recovery_id(3), job, RunState::Running);
    fixture.storage.insert_run(original);
    // First observation of Interrupted is precommit; the next is the following page boundary.
    bool observed_before_commit = false;
    auto cancelled              = fixture.recover(1, [&] {
        if (fixture.run(original.run.id).state != RunState::Interrupted) {
            return false;
        }
        return std::exchange(observed_before_commit, true);
    });
    REQUIRE_FALSE(cancelled);
    CHECK(cancelled.error().code == "jobu.recovery.cancelled");
    fixture.storage.reopen();
    require_interrupted(fixture, original, false);
    auto resumed = fixture.recover();
    REQUIRE(resumed);
    require_report(*resumed);
}

TEST_CASE("Recovery polls cancellation before validation and after the final repair", "[jobu][recovery][sqlite]")
{
    ServiceFixture fixture;
    auto           stop_immediately = fixture.recover(1, [] { return true; });
    REQUIRE_FALSE(stop_immediately);
    CHECK(stop_immediately.error().code == "jobu.recovery.cancelled");

    auto queue = recovery_queue(recovery_id(1), QueueState::Suspending);
    fixture.storage.insert_queue(queue);
    bool precommit_seen     = false;
    bool next_page_seen     = false;
    auto stop_after_repairs = fixture.recover(1, [&] {
        if (fixture.queue(queue.id).state != QueueState::Suspended) {
            return false;
        }
        if (!std::exchange(precommit_seen, true)) {
            return false;
        }
        // The next empty queue page finishes repair; subsequent polling must still stop.
        return std::exchange(next_page_seen, true);
    });
    REQUIRE_FALSE(stop_after_repairs);
    CHECK(stop_after_repairs.error().code == "jobu.recovery.cancelled");
    CHECK(fixture.queue(queue.id).state == QueueState::Suspended);
}

TEST_CASE("Recovery uses one interruption time and fresh recurrence lower bounds", "[jobu][recovery][sqlite]")
{
    auto           fresh = GENERATE(UtcTimePoint{60s}, UtcTimePoint{240s});
    ServiceFixture fixture;
    auto           queue = recovery_queue(recovery_id(1));
    fixture.storage.insert_queue(queue);
    auto job      = fixture.recurring(recovery_id(2), queue.id);
    auto original = fixture.storage.make_run(recovery_id(3), job, RunState::Running);
    fixture.storage.insert_run(original);
    auto report = fixture.recover(1, [&] {
        // Recovery has sampled its invocation timestamp before its first stop check.
        fixture.time.set_utc(fresh);
        return false;
    });
    REQUIRE(report);
    require_interrupted(fixture, original, false);
    REQUIRE(fixture.cron.next_calls().size() == 1);
    CHECK(fixture.cron.next_calls().front().exclusive_lower_bound == std::max(UtcTimePoint{120s}, fresh));
    RunRepository runs{fixture.storage.database, fixture.storage.registry};
    auto          successor = runs.find_schedule_owned(job.id);
    REQUIRE(successor);
    REQUIRE(*successor);
    CHECK((*successor)->planned_at == (fresh > UtcTimePoint{120s} ? UtcTimePoint{300s} : UtcTimePoint{180s}));
}

TEST_CASE("Successful recovery satisfies scheduler startup and recovered retry dispatch", "[jobu][recovery][sqlite]")
{
    auto                                   loop = jb::core::priv::make_fake_event_loop();
    jb::core::priv::ScopedCurrentEventLoop current{loop.loop.get()};
    ServiceFixture                         fixture;
    FakeAttemptExecutor                    executor;
    executor.set_available(JobType::Cli, true);
    auto queue = recovery_queue(recovery_id(1), QueueState::Active, RecoveryPolicy::RetryInterrupted);
    fixture.storage.insert_queue(queue);
    auto job                                      = fixture.storage.make_job(recovery_id(2), queue.id);
    job.attributes.at("retry.initial_delay").data = Duration{5s};
    fixture.storage.insert_job(job);
    auto original = fixture.storage.make_run(recovery_id(3), job, RunState::Running);
    fixture.storage.insert_run(original);
    RejectingSecretProvider secrets;
    Scheduler               scheduler{fixture.storage.database,
                                      fixture.storage.registry,
                                      fixture.cron,
                                      fixture.generator,
                                      fixture.time,
                                      executor,
                                      secrets};
    auto                    before = scheduler.start();
    REQUIRE_FALSE(before);
    CHECK(executor.start_requests().empty());
    auto recovered = fixture.recover();
    REQUIRE(recovered);
    CHECK(executor.start_requests().empty());
    fixture.time.set_utc(UtcTimePoint{125s});
    REQUIRE(scheduler.start());
    REQUIRE(executor.start_requests().size() == 1);
    auto key = executor.pending_keys().front();
    CHECK(key.run_id == original.run.id);
    CHECK(key.attempt_number == 2);
    scheduler.stop();
    // Respect the pre-7.8 callback lifetime contract before destroying the scheduler.
    REQUIRE(executor.complete(
        key,
        {.key = key, .outcome = AttemptOutcome::Succeeded, .result = JsonValue{.data = JsonValue::Object{}}}));
}

TEST_CASE("Recovery converges after abrupt process exit between committed units", "[jobu][recovery][sqlite]")
{
    ServiceFixture fixture;
    auto           queue = recovery_queue(recovery_id(1));
    fixture.storage.insert_queue(queue);
    auto first_job  = fixture.recurring(recovery_id(2), queue.id);
    auto second_job = fixture.recurring(recovery_id(3), queue.id);
    auto first      = fixture.storage.make_run(recovery_id(4), first_job, RunState::Running);
    auto second     = fixture.storage.make_run(recovery_id(5), second_job, RunState::Running);
    fixture.storage.insert_run(first);
    fixture.storage.insert_run(second);
    REQUIRE(fixture.storage.database.close());

    // The helper starts as a new process so its SQLite connection and runtime state are
    // created after exec. The inherited pipe writer closes on exit; hangup is an exit
    // handshake, not recovery progress evidence.
    int descriptors[2];
    REQUIRE(::pipe(descriptors) == 0);

    auto executable      = std::string{RECOVERY_SERVICE_TEST_HELPER};
    auto database_file   = fixture.storage.database_file.string();
    auto first_run       = first.run.id.to_string();
    auto successor_one   = recovery_id(1000).to_string();
    auto successor_two   = recovery_id(1001).to_string();
    auto successor_three = recovery_id(1002).to_string();
    auto arguments       = std::array<char*, 7>{executable.data(),
                                                database_file.data(),
                                                first_run.data(),
                                                successor_one.data(),
                                                successor_two.data(),
                                                successor_three.data(),
                                                nullptr};

    pid_t pid{};
    auto  spawn_error =
        ::posix_spawn(&pid, executable.c_str(), nullptr, nullptr, arguments.data(), process_environment());
    ::close(descriptors[1]);
    if (spawn_error != 0) {
        ::close(descriptors[0]);
        FAIL("Could not start recovery child");
    }

    pollfd descriptor{.fd = descriptors[0], .events = POLLIN, .revents = 0};
    int    ready;
    do {
        ready = ::poll(&descriptor, 1, 10000);
    } while (ready < 0 && errno == EINTR);
    ::close(descriptors[0]);
    if (ready <= 0) {
        ::kill(pid, SIGKILL);
    }
    int   status = 0;
    pid_t waited;
    do {
        waited = ::waitpid(pid, &status, 0);
    } while (waited < 0 && errno == EINTR);
    REQUIRE(ready > 0);
    REQUIRE(waited == pid);
    REQUIRE(WIFEXITED(status));
    REQUIRE(WEXITSTATUS(status) == 77);

    REQUIRE(fixture.storage.database.open());
    require_interrupted(fixture, first, false);
    fixture.storage.require_run(second);
    // The restarted UUID source must not replay the child's already committed successor ID.
    fixture.generator = SequenceUuidGenerator{
        {recovery_id(1001), recovery_id(1002)}
    };
    auto resumed = fixture.recover();
    REQUIRE(resumed);
    require_report(*resumed, {.interrupted_attempts = 1, .terminal_runs = 1, .inserted_successors = 1});
    require_interrupted(fixture, first, false);
    require_interrupted(fixture, second, false);
    auto again = fixture.recover();
    REQUIRE(again);
    require_report(*again);
}

TEST_CASE("Recovery retains templates across reopen without secret lookup", "[jobu][recovery][template]")
{
    auto const     type = GENERATE(JobType::Cli, JobType::Http);
    ServiceFixture fixture;
    auto           queue   = recovery_queue(recovery_id(1));
    auto           job     = fixture.storage.make_job(recovery_id(2), queue.id, type);
    auto           payload = parse_json(
        type == JobType::Cli ? R"({"command":"/bin/tool","environment":{"TOKEN":{"secret":"missing.token"}}})"
                             : R"({"url":"https://example.test/","method":"POST","body":{"secret":"missing.body"}})");
    REQUIRE(payload);
    job.payload = *payload;
    fixture.storage.insert_queue(queue);
    fixture.storage.insert_job(job);
    auto run = fixture.storage.make_run(recovery_id(3), job);
    fixture.storage.insert_run(run);
    fixture.storage.reopen();

    // No secret rows exist. Recovery must validate immutable templates without resolving their names.
    auto recovered = fixture.recover();
    REQUIRE(recovered);
    CHECK(recovered->interrupted_attempts == 0);
    CHECK(fixture.job(job.id).payload == *payload);
    fixture.storage.require_run(run);
    REQUIRE(fixture.recover());

    // Resolution belongs to the first dispatch after recovery, not to the recovery scan. The still-missing name
    // becomes a durable terminal attempt without an external launch.
    auto                                   loop = jb::core::priv::make_fake_event_loop();
    jb::core::priv::ScopedCurrentEventLoop current{loop.loop.get()};
    DatabaseSecretProvider                 provider{fixture.storage.database};
    FakeAttemptExecutor                    executor;
    executor.set_available(type, true);
    Scheduler scheduler{fixture.storage.database,
                        fixture.storage.registry,
                        fixture.cron,
                        fixture.generator,
                        fixture.time,
                        executor,
                        provider};
    REQUIRE(scheduler.start());
    CHECK(executor.start_requests().empty());
    auto failed = fixture.run(run.run.id);
    CHECK(failed.state == RunState::Failed);
    CHECK(failed.payload == *payload);
    REQUIRE(failed.result);
    CHECK(failed.result->as_object().at("error_code").as_string() == "jobu.secret.not_found");
}

TEST_CASE("Recovery fails closed on malformed job or immutable run templates", "[jobu][recovery][template]")
{
    auto const     corrupt_job = GENERATE(false, true);
    ServiceFixture fixture;
    auto           queue = recovery_queue(recovery_id(1));
    auto           job   = fixture.storage.make_job(recovery_id(2), queue.id);
    fixture.storage.insert_queue(queue);
    fixture.storage.insert_job(job);
    fixture.storage.insert_run(fixture.storage.make_run(recovery_id(3), job));
    {
        // Corrupt only one document, leaving the other valid so both durable boundaries are exercised.
        Query query{fixture.storage.database};
        REQUIRE(query.exec(
            corrupt_job
                ? R"(UPDATE jobu_jobs SET payload_json = '{"command":"/bin/tool","arguments":[{"secret":"private marker"}]}')"
                : R"(UPDATE jobu_runs SET payload_json = '{"command":"/bin/tool","arguments":[{"secret":"private marker"}]}')"));
    }
    fixture.storage.reopen();
    auto recovered = fixture.recover();
    REQUIRE_FALSE(recovered);
    CHECK(recovered.error().code == "jobu.recovery.invariant");
    CHECK(recovered.error().message.find("private marker") == std::string::npos);
    CHECK(recovered.error().detail.find("private marker") == std::string::npos);
}
