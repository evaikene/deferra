#include "application.hpp"
#include "attempt_repository_priv.hpp"
#include "byte_buffer.hpp"
#include "client.hpp"
#include "cron.hpp"
#include "event_loop.hpp"
#include "job_repository_priv.hpp"
#include "json.hpp"
#include "local_socket.hpp"
#include "management_json.hpp"
#include "process.hpp"
#include "protocol.hpp"
#include "query.hpp"
#include "queue_repository_priv.hpp"
#include "run_repository_priv.hpp"
#include "support/http_test_server.hpp"
#include "support/process_exit_watch.hpp"
#include "support/recovery_fixture.hpp"
#include "support/storage_fault_helpers.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <sqlite3.h>

#include <cerrno>
#include <chrono>
#include <csignal> // IWYU pragma: keep Provides POSIX signal constants.
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace jb::core;
using namespace jb::jobu;
using namespace jb::jobu::detail;
using namespace jb::test;
using namespace std::chrono_literals;

namespace {

void require_execution_environment(JobType type)
{
    auto const* enabled = std::getenv("JOBU_TEST_ALLOW_ROOT_CLI");
    if (type == JobType::Cli && ::geteuid() == 0 && (!enabled || std::string_view{enabled} != "1")) {
        SKIP("root target execution requires JOBU_TEST_ALLOW_ROOT_CLI=1 in an isolated test environment");
    }
}

auto text(std::string value) -> JsonValue
{
    return {.data = std::move(value)};
}

/// Separate FIFOs prevent the helper from consuming its own readiness report.
class Channel final {
public:
    explicit Channel(std::filesystem::path path)
        : _path{std::move(path)}
    {
        REQUIRE(::mkfifo(_path.c_str(), 0600) == 0);
        _fd = ::open(_path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
        REQUIRE(_fd >= 0);
    }

    ~Channel() { ::close(_fd); }

    Channel(Channel const&)                    = delete;
    auto operator=(Channel const&) -> Channel& = delete;

    auto path() const -> std::string { return _path.string(); }

    void release() const { REQUIRE(::write(_fd, "R", 1) == 1); }

    auto ready_pid() const -> std::optional<pid_t>
    {
        pid_t      pid{};
        auto const count = ::read(_fd, &pid, sizeof(pid));
        if (count < 0 && (errno == EAGAIN || errno == EINTR)) {
            return {};
        }
        REQUIRE(count == sizeof(pid));
        REQUIRE(pid > 0);
        return pid;
    }

private:
    std::filesystem::path _path;
    int                   _fd{-1};
};

/// One database, multiple real daemon incarnations. Only the stopped parent opens the owning driver.
/// The live observer is read-only and keeps no statement/transaction between readiness polls.
class CrashFixture final {
public:
    explicit CrashFixture(RecoveryFixtureSchema schema = RecoveryFixtureSchema::Current)
        : storage{{}, schema}
        , report{storage.directory.path() / "report"}
        , release{storage.directory.path() / "release"}
    {}

    ~CrashFixture()
    {
        // On assertion failure Process cleans up the daemon; watches or helper alarms bound orphan lifetimes.
        daemon.reset();
        targets.clear();
    }

    template <typename Predicate>
    void until(Predicate predicate, bool allow_exit = false)
    {
        auto const deadline = Clock::now() + 8s;
        while (!predicate() && (allow_exit || !exit) && Clock::now() < deadline) {
            REQUIRE(app.process_events(EventFlag::All, 10) != ProcessEventsResult::Failed);
        }
        INFO(log);
        if (!allow_exit) {
            REQUIRE_FALSE(exit);
        }
        REQUIRE(predicate());
    }

    void launch(bool allow_root_cli)
    {
        REQUIRE_FALSE(daemon);
        REQUIRE(storage.database.close());
        // A killed LocalServer leaves its socket entry intentionally intact. Never reuse that path.
        socket_path = storage.directory.path() / ("daemon-" + std::to_string(++incarnation) + ".sock");
        exit.reset();
        log.clear();
        daemon = std::make_unique<Process>();
        daemon->standard_output.connect(&app, [&](ByteBuffer const& bytes) { log.append(as_string_view(bytes)); });
        daemon->standard_error.connect(&app, [&](ByteBuffer const& bytes) { log.append(as_string_view(bytes)); });
        daemon->finished.connect(&app, [&](ProcessExit const& value) { exit = value; });
        auto arguments = std::vector<std::string>{"--database",
                                                  storage.database_file.string(),
                                                  "--socket",
                                                  socket_path.string(),
                                                  "--cli-concurrency",
                                                  "2",
                                                  "--http-concurrency",
                                                  "2"};
        if (allow_root_cli && ::geteuid() == 0) {
            arguments.emplace_back("--allow-root-cli");
        }

        started_after = std::chrono::time_point_cast<std::chrono::microseconds>(UtcClock::now());
        REQUIRE(daemon->start(
            {.executable = JOBUD_EXECUTABLE, .arguments = std::move(arguments), .termination_grace = 0ms}));
    }

    void start(bool allow_root_cli = false)
    {
        launch(allow_root_cli);
        until([&] {
            std::error_code error;
            return std::filesystem::is_socket(socket_path, error);
        });
        // A successful real client round trip proves recovery and scheduler startup have both returned.
        control_info();
        ready_before = UtcClock::now();

        sqlite3*   raw{};
        auto const opened = sqlite3_open_v2(storage.database_file.c_str(), &raw, SQLITE_OPEN_READONLY, nullptr);
        observer.reset(raw);
        REQUIRE(opened == SQLITE_OK);
        REQUIRE(sqlite3_busy_timeout(raw, 100) == SQLITE_OK);
    }

    void require_schema_startup_failure()
    {
        launch(false);
        until([&] { return exit.has_value(); }, true);
        INFO(log);
        REQUIRE(exit->kind == ProcessExitKind::Exited);
        CHECK(exit->exit_code == EXIT_FAILURE);
        CHECK(log.find("jobu.schema.") != std::string::npos);
        CHECK_FALSE(std::filesystem::exists(socket_path));
        daemon.reset();
        REQUIRE(storage.database.open());
    }

    void crash()
    {
        REQUIRE(daemon);
        auto const pid = daemon->process_id();
        REQUIRE(pid);
        REQUIRE(::kill(static_cast<pid_t>(*pid), SIGKILL) == 0);
        until([&] { return exit.has_value(); }, true);
        REQUIRE(exit->kind == ProcessExitKind::Signaled);
        REQUIRE(exit->signal_number == SIGKILL);
        daemon.reset();

        // Old target effects must not overlap the restarted attempt. Daemon SIGKILL cannot clean these up.
        for (auto const& target : targets) {
#if defined(__APPLE__)
            // kqueue observes identity but cannot signal it safely. Release the controlled orphan
            // over its private channel, and observe exit before starting another incarnation.
            if (!target->exited()) {
                release.release();
            }
#else
            target->kill();
#endif
            until([&] { return target->exited(); }, true);
        }
        targets.clear();
        observer.reset();
        REQUIRE(storage.database.open());
    }

    void control_info()
    {
        Process                    client;
        std::optional<ProcessExit> result;
        std::string                output;
        client.standard_output.connect(&app, [&](ByteBuffer const& bytes) { output.append(as_string_view(bytes)); });
        client.standard_error.connect(&app, [&](ByteBuffer const& bytes) { output.append(as_string_view(bytes)); });
        client.finished.connect(&app, [&](ProcessExit const& value) { result = value; });
        REQUIRE(client.start({
            .executable        = JOBUCTL_EXECUTABLE,
            .arguments         = {"--socket", socket_path.string(), "system", "info"},
            .timeout           = 5s,
            .termination_grace = 0ms
        }));
        until([&] { return result.has_value(); });
        INFO(output);
        REQUIRE(result->kind == ProcessExitKind::Exited);
        REQUIRE(result->exit_code == 0);
        REQUIRE(output.find("1.2") != std::string::npos);
    }

    void rpc(std::string_view method, JsonValue parameters)
    {
        jb::net::LocalSocket socket;
        bool                 connected{false};
        socket.connected.connect(&app, [&] { connected = true; });
        socket.connect_to_server(socket_path);
        until([&] { return connected; });
        jb::rpc::Client                  client{socket};
        bool                             received{false};
        std::optional<jb::rpc::RpcError> error;
        client.result_received.connect(&app, [&](jb::rpc::RequestId const&, JsonValue const&) { received = true; });
        client.error_received.connect(&app, [&](jb::rpc::RequestId const&, jb::rpc::RpcError const& value) {
            error = value;
        });
        REQUIRE(client.call(method, std::move(parameters)));
        until([&] { return received || error.has_value(); });
        INFO((error ? error->message : ""));
        REQUIRE_FALSE(error);
    }

    auto count(std::string const& sql) -> std::int64_t
    {
        sqlite3_stmt* raw{};
        REQUIRE(sqlite3_prepare_v2(observer.get(), sql.c_str(), -1, &raw, nullptr) == SQLITE_OK);
        auto statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>{raw, sqlite3_finalize};
        REQUIRE(sqlite3_step(raw) == SQLITE_ROW);
        auto const value = sqlite3_column_int64(raw, 0);
        REQUIRE(sqlite3_step(raw) == SQLITE_DONE);
        return value;
    }

    void running(JobType type, std::size_t http_request = 1)
    {
        if (type == JobType::Cli) {
            std::optional<pid_t> pid;
            until([&] {
                if (!pid) {
                    pid = report.ready_pid();
                }
                return pid.has_value();
            });
            targets.push_back(std::make_unique<ProcessExitWatch>(*pid));
        }
        else {
            until([&] { return server.requests().size() == http_request; });
        }
        REQUIRE(count("SELECT count(*) FROM jobu_runs WHERE state='running'") == 1);
        REQUIRE(count("SELECT count(*) FROM jobu_attempts WHERE state='running'") == 1);
        REQUIRE(count("SELECT count(*) FROM jobu_attempt_output o JOIN jobu_attempts a "
                      "ON a.run_id=o.run_id AND a.attempt_number=o.attempt_number WHERE a.state='running'") == 0);
    }

    auto job(JobType type, Duration retry_delay = 1h) const -> JobDefinition
    {
        auto definition                                      = storage.make_job(recovery_id(2), recovery_id(1), type);
        definition.attributes.at("retry.initial_delay").data = retry_delay;
        definition.attributes.at("retry.max_delay").data     = retry_delay;
        definition.attributes.at("job.timeout").data         = Duration{60s};
        definition.attributes.at("output.capture").data      = std::string{"always"};
        if (type == JobType::Cli) {
            definition.payload.data = JsonValue::Object{
                {"command",   text(PROCESS_TEST_HELPER)                                                           },
                {"arguments",
                 {.data = JsonValue::Array{text("daemon-output-wait"), text(report.path()), text(release.path())}}}
            };
        }
        else {
            definition.payload.data = JsonValue::Object{
                {"url", text(server.url())}
            };
        }
        return definition;
    }

    auto read_run(Uuid id) -> RecoveryRunFixture
    {
        RunRepository runs{storage.database, storage.registry};
        auto          run = runs.find_by_id(id);
        REQUIRE(run);
        REQUIRE(run->has_value());
        RecoveryRunFixture result{.run = **run};
        AttemptRepository  attempts{storage.database};
        auto               history = attempts.list_for_run(id, 100);
        REQUIRE(history);
        for (auto const& attempt : *history) {
            auto output = attempts.find_output(id, attempt.attempt_number);
            REQUIRE(output);
            result.attempts.push_back({.attempt = attempt, .output = *output});
        }
        return result;
    }

    void unchanged_restart(bool cli = false)
    {
        auto const before = storage_snapshot(storage.database);
        start(cli);
        REQUIRE(count("SELECT count(*) FROM jobu_attempts WHERE state='running'") == 0);
        crash();
        CHECK(storage_snapshot(storage.database) == before);
    }

    RecoveryFixture storage;
    Application     app{0, nullptr};
    Channel         report;
    Channel         release;
    HttpTestServer  server;
    UtcTimePoint    started_after;
    UtcTimePoint    ready_before;

private:
    std::filesystem::path                              socket_path;
    unsigned                                           incarnation{0};
    std::string                                        log;
    std::optional<ProcessExit>                         exit;
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> observer{nullptr, sqlite3_close};
    std::vector<std::unique_ptr<ProcessExitWatch>>     targets;
    std::unique_ptr<Process>                           daemon;
};

/// Build the exact expected interruption from the previously committed Running snapshot.
/// Only the runtime-selected timestamp is read back; its interval and every other changed field are asserted.
void require_interrupted(CrashFixture& fixture, RecoveryRunFixture before, bool retry, Duration delay = 1h)
{
    auto actual = fixture.read_run(before.run.id);
    REQUIRE(actual.attempts.size() == before.attempts.size());
    auto const completed = actual.attempts.back().attempt.completed_at;
    REQUIRE(completed);
    CHECK(*completed >= fixture.started_after);
    CHECK(*completed <= fixture.ready_before);
    auto document = parse_json(R"({"reason":"daemon_interrupted","outcome_unknown":true})");
    REQUIRE(document);
    auto& last = before.attempts.back();
    REQUIRE(last.attempt.state == AttemptState::Running);
    REQUIRE_FALSE(last.output);
    last.attempt.state        = AttemptState::Completed;
    last.attempt.outcome      = AttemptOutcome::Interrupted;
    last.attempt.completed_at = completed;
    last.attempt.result       = *document;
    last.output      = AttemptOutput{.stdout_bytes = ByteBuffer{}, .stderr_bytes = ByteBuffer{}, .capture_lost = true};
    before.run.state = retry ? RunState::RetryWait : RunState::Interrupted;
    if (retry) {
        auto const clock_delay = std::chrono::duration_cast<UtcTimePoint::duration>(delay);
        REQUIRE(clock_delay == delay);
        before.run.runnable_at = *completed + clock_delay;
    }
    else {
        before.run.completed_at = completed;
        before.run.result       = *document;
    }
    fixture.storage.require_run(before);
}

} // namespace

TEST_CASE("daemon crash recovers each runner under both policies and exhausted retries",
          "[jobud][recovery][integration]")
{
    auto const type      = GENERATE(JobType::Cli, JobType::Http);
    auto const policy    = GENERATE(RecoveryPolicy::FailInterrupted, RecoveryPolicy::RetryInterrupted);
    auto const exhausted = GENERATE(false, true);
    CAPTURE(type, policy, exhausted);
    require_execution_environment(type);
    CrashFixture fixture;
    auto         queue = recovery_queue(recovery_id(1), QueueState::Active, policy);
    auto         job   = fixture.job(type);
    auto         seed  = fixture.storage.make_run(recovery_id(3),
                                                  job,
                                                  exhausted ? RunState::RetryWait : RunState::Scheduled,
                                                  exhausted ? 2 : 0);
    fixture.storage.insert_queue(queue);
    fixture.storage.insert_job(job);
    fixture.storage.insert_run(seed);

    fixture.start(type == JobType::Cli);
    fixture.running(type);
    fixture.crash();
    auto const running = fixture.read_run(seed.run.id);
    REQUIRE(running.run.state == RunState::Running);
    REQUIRE(running.attempts.size() == (exhausted ? 3 : 1));

    fixture.start(type == JobType::Cli);
    REQUIRE(fixture.count("SELECT count(*) FROM jobu_attempts WHERE state='running'") == 0);
    REQUIRE(fixture.count("SELECT count(*) FROM jobu_runs") == 1);
    fixture.crash();
    require_interrupted(fixture, running, policy == RecoveryPolicy::RetryInterrupted && !exhausted);
    fixture.unchanged_restart(type == JobType::Cli);
}

TEST_CASE("daemon restart executes the next attempt of the same immutable run", "[jobud][recovery][integration]")
{
    auto const type = GENERATE(JobType::Cli, JobType::Http);
    CAPTURE(type);
    require_execution_environment(type);
    CrashFixture fixture;
    auto const   response = as_bytes("restarted response");
    fixture.server.enqueue_response({
        .body = ByteBuffer{response.begin(), response.end()}
    });
    fixture.server.enqueue_response({
        .body = ByteBuffer{response.begin(), response.end()}
    });
    auto queue = recovery_queue(recovery_id(1), QueueState::Active, RecoveryPolicy::RetryInterrupted);
    auto job   = fixture.job(type, 0ms);
    auto seed  = fixture.storage.make_run(recovery_id(3), job);
    fixture.storage.insert_queue(queue);
    fixture.storage.insert_job(job);
    fixture.storage.insert_run(seed);

    fixture.start(type == JobType::Cli);
    fixture.running(type);
    fixture.crash();
    auto       expected    = fixture.read_run(seed.run.id);
    auto const first_start = expected.run.started_at;
    fixture.start(type == JobType::Cli);
    fixture.running(type, 2);
    REQUIRE(fixture.count("SELECT count(*) FROM jobu_attempts WHERE attempt_number=2 AND state='running'") == 1);
    if (type == JobType::Cli) {
        fixture.release.release();
    }
    else {
        fixture.server.release_responses();
    }
    fixture.until([&] { return fixture.count("SELECT count(*) FROM jobu_runs WHERE state='succeeded'") == 1; });
    fixture.crash();

    auto actual = fixture.read_run(seed.run.id);
    REQUIRE(actual.attempts.size() == 2);
    REQUIRE(actual.attempts.front().attempt.completed_at);
    CHECK(*actual.attempts.front().attempt.completed_at >= fixture.started_after);
    CHECK(*actual.attempts.front().attempt.completed_at <= fixture.ready_before);
    auto document = parse_json(R"({"reason":"daemon_interrupted","outcome_unknown":true})");
    REQUIRE(document);
    expected.attempts.front().attempt.state        = AttemptState::Completed;
    expected.attempts.front().attempt.outcome      = AttemptOutcome::Interrupted;
    expected.attempts.front().attempt.completed_at = actual.attempts.front().attempt.completed_at;
    expected.attempts.front().attempt.result       = *document;
    expected.attempts.front().output =
        AttemptOutput{.stdout_bytes = ByteBuffer{}, .stderr_bytes = ByteBuffer{}, .capture_lost = true};
    auto const& second = actual.attempts.back();
    CHECK(second.attempt.run_id == seed.run.id);
    CHECK(second.attempt.attempt_number == 2);
    CHECK(second.attempt.due_at == *actual.attempts.front().attempt.completed_at);
    CHECK(second.attempt.started_at >= second.attempt.due_at);
    CHECK(second.attempt.completed_at >= second.attempt.started_at);
    CHECK(second.attempt.state == AttemptState::Completed);
    CHECK(second.attempt.outcome == AttemptOutcome::Succeeded);
    REQUIRE(second.output);
    CHECK_FALSE(second.output->capture_lost);
    CHECK_FALSE(second.output->stdout_truncated);
    CHECK_FALSE(second.output->stderr_truncated);
    REQUIRE(second.output->stdout_bytes);
    REQUIRE(second.output->stderr_bytes);
    CHECK(as_string_view(*second.output->stdout_bytes) ==
          (type == JobType::Cli ? std::string_view{"o\0ut", 4} : std::string_view{"restarted response"}));
    CHECK(as_string_view(*second.output->stderr_bytes) ==
          (type == JobType::Cli
               ? std::string_view{"e\0rr", 4}
               : std::string_view{"HTTP/1.1 200 OK\r\nContent-Length: 18\r\nConnection: close\r\n\r\n"}));
    expected.attempts.push_back(second);
    expected.run.state        = RunState::Succeeded;
    expected.run.runnable_at  = second.attempt.due_at;
    expected.run.completed_at = second.attempt.completed_at;
    expected.run.result       = second.attempt.result;
    CHECK(actual.run.started_at == first_start);
    fixture.storage.require_run(expected);
    fixture.unchanged_restart(type == JobType::Cli);
}

TEST_CASE("daemon recovery retains or releases a manual barrier according to policy", "[jobud][recovery][integration]")
{
    auto const retry = GENERATE(false, true);
    CAPTURE(retry);
    CrashFixture fixture;
    fixture.server.enqueue_response({});
    fixture.server.enqueue_response({});
    auto queue     = recovery_queue(recovery_id(1),
                                    QueueState::Active,
                                    retry ? RecoveryPolicy::RetryInterrupted : RecoveryPolicy::FailInterrupted);
    auto job       = fixture.job(JobType::Http);
    auto scheduled = fixture.storage.make_run(recovery_id(3), job);
    auto manual    = fixture.storage.make_run(recovery_id(4), job, RunState::Scheduled, 0, RunOrigin::Manual);
    fixture.storage.insert_queue(queue);
    fixture.storage.insert_job(job);
    fixture.storage.insert_run(scheduled);
    fixture.storage.insert_run(manual);

    fixture.start();
    fixture.running(JobType::Http);
    fixture.crash();
    auto running = fixture.read_run(manual.run.id);
    REQUIRE(running.run.state == RunState::Running);
    fixture.storage.require_run(scheduled);

    fixture.start();
    REQUIRE(fixture.count("SELECT count(*) FROM jobu_runs") == 2);
    if (retry) {
        REQUIRE(fixture.count("SELECT count(*) FROM jobu_runs WHERE state='retry_wait'") == 1);
        REQUIRE(fixture.count("SELECT count(*) FROM jobu_attempts WHERE state='running'") == 0);
        CHECK(fixture.server.requests().size() == 1);
    }
    else {
        fixture.running(JobType::Http, 2);
        fixture.server.release_responses();
        fixture.until([&] { return fixture.count("SELECT count(*) FROM jobu_runs WHERE state='succeeded'") == 1; });
    }
    fixture.crash();
    require_interrupted(fixture, running, retry);
    if (retry) {
        fixture.storage.require_run(scheduled);
    }
    else {
        auto completed = fixture.read_run(scheduled.run.id);
        REQUIRE(completed.run.state == RunState::Succeeded);
        REQUIRE(completed.attempts.size() == 1);
        CHECK(completed.attempts.front().attempt.outcome == AttemptOutcome::Succeeded);
    }
    fixture.unchanged_restart();
}

TEST_CASE("daemon crash recovery repairs recurrence and finishes owner suspension", "[jobud][recovery][integration]")
{
    auto const suspend_queue = GENERATE(false, true);
    auto const retry         = GENERATE(false, true);
    CAPTURE(suspend_queue, retry);
    CrashFixture fixture;
    auto         queue = recovery_queue(recovery_id(1),
                                        QueueState::Active,
                                        retry ? RecoveryPolicy::RetryInterrupted : RecoveryPolicy::FailInterrupted);
    auto         job   = fixture.job(JobType::Http, 0ms);
    job.schedule       = CronSchedule{.expression = "0 0 * * *", .timezone = "UTC"};
    auto seed          = fixture.storage.make_run(recovery_id(3), job);
    fixture.storage.insert_queue(queue);
    fixture.storage.insert_job(job);
    fixture.storage.insert_run(seed);

    fixture.start();
    fixture.running(JobType::Http);
    auto parameters = suspend_queue ? queue_selector_to_json(QueueSelector{queue.id}) : job_id_to_json(job.id);
    REQUIRE(parameters);
    fixture.rpc(suspend_queue ? "queue.suspend" : "job.suspend", *parameters);
    fixture.crash();
    auto const running = fixture.read_run(seed.run.id);
    REQUIRE(running.run.state == RunState::Running);
    JobRepository   jobs{fixture.storage.database, fixture.storage.registry};
    QueueRepository queues{fixture.storage.database, fixture.storage.registry};
    auto            before_job   = jobs.find_by_id(job.id, false);
    auto            before_queue = queues.find_by_id(queue.id, false);
    REQUIRE(before_job);
    REQUIRE(before_job->has_value());
    REQUIRE(before_queue);
    REQUIRE(before_queue->has_value());
    CHECK((**before_job).state == (suspend_queue ? JobState::Active : JobState::Suspending));
    CHECK((**before_queue).state == (suspend_queue ? QueueState::Suspending : QueueState::Active));

    fixture.start();
    REQUIRE(fixture.count("SELECT count(*) FROM jobu_attempts WHERE state='running'") == 0);
    REQUIRE(fixture.count("SELECT count(*) FROM jobu_runs") == (retry ? 1 : 2));
    CHECK(fixture.server.requests().size() == 1);
    fixture.crash();
    require_interrupted(fixture, running, retry, 0ms);
    auto after_job   = jobs.find_by_id(job.id, false);
    auto after_queue = queues.find_by_id(queue.id, false);
    REQUIRE(after_job);
    REQUIRE(after_job->has_value());
    REQUIRE(after_queue);
    REQUIRE(after_queue->has_value());
    CHECK((**after_job).state == (suspend_queue ? JobState::Active : JobState::Suspended));
    CHECK((**after_job).revision == (**before_job).revision + (suspend_queue ? 0 : 1));
    CHECK((**after_queue).state == (suspend_queue ? QueueState::Suspended : QueueState::Active));
    if (suspend_queue) {
        CHECK((**after_queue).updated_at == fixture.read_run(seed.run.id).attempts.back().attempt.completed_at);
    }

    RunRepository runs{fixture.storage.database, fixture.storage.registry};
    auto          successor = runs.find_schedule_owned(job.id);
    REQUIRE(successor);
    REQUIRE(successor->has_value());
    if (retry) {
        CHECK((**successor).id == seed.run.id);
        CHECK((**successor).state == RunState::RetryWait);
    }
    else {
        // The daemon uses the real UTC cron engine. Allow only ticks within the bounded recovery interval.
        SystemCronEngine cron;
        auto             earliest = cron.next_after(std::get<CronSchedule>(job.schedule), fixture.started_after);
        auto             latest   = cron.next_after(std::get<CronSchedule>(job.schedule), fixture.ready_before);
        REQUIRE(earliest);
        REQUIRE(latest);
        CHECK((**successor).id != seed.run.id);
        CHECK((**successor).state == RunState::Scheduled);
        CHECK((**successor).planned_at >= *earliest);
        CHECK((**successor).planned_at <= *latest);
        CHECK((**successor).planned_at == (**successor).runnable_at);
        CHECK((**successor).job_revision == (**before_job).revision);
        CHECK((**successor).payload == job.payload);
        auto expected            = fixture.storage.make_run((**successor).id, **before_job);
        expected.run.planned_at  = (**successor).planned_at;
        expected.run.runnable_at = (**successor).runnable_at;
        fixture.storage.require_run(expected);
    }
    fixture.unchanged_restart();
}

TEST_CASE("daemon upgrades version one before recovery and serving", "[jobud][recovery][schema][integration]")
{
    CrashFixture fixture{RecoveryFixtureSchema::VersionOne};
    auto         queue   = recovery_queue(recovery_id(1));
    auto         job     = fixture.storage.make_job(recovery_id(2), queue.id, JobType::Http);
    auto         running = fixture.storage.make_run(recovery_id(3), job, RunState::Running);
    fixture.storage.insert_queue(queue);
    fixture.storage.insert_job(job);
    fixture.storage.insert_run(running);

    // start() includes a real system.info round trip; the stopped parent never upgrades this database.
    fixture.start();
    CHECK(fixture.count("SELECT version FROM jobu_schema") == 2);
    CHECK(fixture.count("SELECT count(*) FROM sqlite_schema WHERE name IN ('jobu_runs_planned_id_idx', "
                        "'jobu_runs_queue_planned_id_idx', 'jobu_runs_job_planned_id_idx')") == 3);
    CHECK(fixture.count("SELECT count(*) FROM jobu_attempts WHERE state = 'running'") == 0);
    fixture.crash();
    require_interrupted(fixture, running, false);
    fixture.unchanged_restart();
}

TEST_CASE("daemon schema rejection leaves recovery rows untouched and never listens",
          "[jobud][recovery][schema][integration]")
{
    auto const* const corrupt = GENERATE("DROP INDEX jobu_runs_job_state_idx",
                                         "CREATE INDEX jobu_runs_job_planned_id_idx ON jobu_runs(id)",
                                         "UPDATE jobu_schema SET version = 3");
    CAPTURE(corrupt);
    CrashFixture fixture{RecoveryFixtureSchema::VersionOne};
    auto         queue   = recovery_queue(recovery_id(1));
    auto         job     = fixture.storage.make_job(recovery_id(2), queue.id, JobType::Http);
    auto         running = fixture.storage.make_run(recovery_id(3), job, RunState::Running);
    fixture.storage.insert_queue(queue);
    fixture.storage.insert_job(job);
    fixture.storage.insert_run(running);
    {
        jb::db::Query query{fixture.storage.database};
        REQUIRE(query.exec(corrupt));
    }
    auto const before = storage_snapshot(fixture.storage.database);
    fixture.require_schema_startup_failure();
    CHECK(storage_snapshot(fixture.storage.database) == before);
    fixture.storage.require_run(running);
    // Collision at the third added index must also roll back the first two DDL statements.
    jb::db::Query query{fixture.storage.database};
    REQUIRE(query.exec("SELECT count(*) FROM sqlite_schema WHERE name IN "
                       "('jobu_runs_planned_id_idx', 'jobu_runs_queue_planned_id_idx')"));
    auto next = query.next();
    REQUIRE(next);
    REQUIRE(*next);
    CHECK(query.value(0) == jb::db::Value{std::int64_t{0}});
}
