#include "application.hpp"
#include "attempt_repository_priv.hpp"
#include "byte_buffer.hpp"
#include "client.hpp"
#include "cron.hpp"
#include "event_loop.hpp"
#include "framing.hpp"
#include "job_repository_priv.hpp"
#include "json.hpp"
#include "local_socket.hpp"
#include "management_json.hpp"
#include "process.hpp"
#include "protocol.hpp"
#include "protocol_priv.hpp"
#include "query.hpp"
#include "queue_repository_priv.hpp"
#include "run_repository_priv.hpp"
#include "statistics_json.hpp"
#include "support/catch_utils.hpp" // IWYU pragma: keep for Catch::StringMaker specializations
#include "support/http_test_server.hpp"
#include "support/process_exit_watch.hpp"
#include "support/protected_daemon_state.hpp"
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
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant> // IWYU pragma: keep std::get accesses the mutable JsonValue alternative.
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

auto json(std::string_view value) -> JsonValue
{
    auto parsed = parse_json(value);
    REQUIRE(parsed);
    return std::move(*parsed);
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
    CrashFixture()
        : report{storage.directory.path() / "report"}
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
        REQUIRE_FALSE(jb::test::protect_daemon_state(storage.database_file));
#ifdef __linux__
        // Restart must reacquire the surviving endpoint lock and prove the crashed socket stale.
        socket_path = storage.directory.path() / "daemon.sock";
#else
        // Native daemon endpoint ownership is still deferred to the macOS adaptation stage.
        socket_path = storage.directory.path() / ("daemon-" + std::to_string(++incarnation) + ".sock");
#endif
        exit.reset();
        log.clear();
        daemon = std::make_unique<Process>();
        daemon->standard_output.connect(&app, [&](ByteBuffer const& bytes) { log.append(as_string_view(bytes)); });
        daemon->standard_error.connect(&app, [&](ByteBuffer const& bytes) { log.append(as_string_view(bytes)); });
        daemon->finished.connect(&app, [&](ProcessExit const& value) { exit = value; });
        auto arguments = std::vector<std::string>{"--no-config",
                                                  "--database",
                                                  storage.database_file.string(),
                                                  "--socket",
                                                  socket_path.string(),
                                                  "--cli-concurrency",
                                                  "2",
                                                  "--http-concurrency",
                                                  "2"};
        if (!configuration.empty()) {
            auto const    path = storage.directory.path() / "logging.ini";
            std::ofstream file{path};
            file << configuration;
            file.close();
            REQUIRE(file);
            arguments[0] = "--config";
            arguments.insert(arguments.begin() + 1, path.string());
        }
        if (::geteuid() == 0) {
            arguments.emplace_back("--allow-root-daemon");
            if (allow_root_cli) {
                arguments.emplace_back("--allow-root-cli");
            }
        }

        started_after = std::chrono::time_point_cast<std::chrono::microseconds>(UtcClock::now());
        REQUIRE(daemon->start(
            {.executable = JOBUD_EXECUTABLE, .arguments = std::move(arguments), .termination_grace = 0ms}));
    }

    void start(bool allow_root_cli = false)
    {
        launch(allow_root_cli);
        // A crashed socket entry already exists before the replacement listener is ready.
        // Retry nonblocking connections within until's deadline; lifecycle logs may be filtered.
        bool                 connected{false};
        jb::net::LocalSocket probe;
        probe.connected.connect(&app, [&] { connected = true; });
        until([&] {
            if (!connected && probe.state() == jb::net::LocalSocketState::Unconnected) {
                probe.connect_to_server(socket_path);
            }
            return connected;
        });
        probe.abort();
        // A successful real client round trip proves recovery and scheduler startup have both returned.
        control_info();
        ready_before = UtcClock::now();

        sqlite3*   raw{};
        auto const opened = sqlite3_open_v2(storage.database_file.c_str(), &raw, SQLITE_OPEN_READONLY, nullptr);
        observer.reset(raw);
        REQUIRE(opened == SQLITE_OK);
        REQUIRE(sqlite3_busy_timeout(raw, 100) == SQLITE_OK);
    }

    void require_startup_failure(std::string_view expected_code)
    {
        launch(false);
        until([&] { return exit.has_value(); }, true);
        INFO(log);
        REQUIRE(exit->kind == ProcessExitKind::Exited);
        CHECK(exit->exit_code == EXIT_FAILURE);
        CHECK(log.find(expected_code) != std::string::npos);
        CHECK_FALSE(std::filesystem::exists(socket_path));
        // The installed sink survives failed resource startup and reports the final exit status.
        auto      lines = std::string_view{log};
        JsonValue last;
        while (!lines.empty()) {
            auto const end = lines.find('\n');
            REQUIRE(end != std::string_view::npos);
            auto value = parse_json(lines.substr(0, end));
            REQUIRE(value);
            CHECK(value->as_object().at("event").as_string() != "jobud.ready");
            last = std::move(*value);
            lines.remove_prefix(end + 1);
        }
        REQUIRE(last.is_object());
        CHECK(last.as_object().at("event").as_string() == "jobud.stopped");
        CHECK(last.as_object().at("fields").as_object().at("exit_status").as_uint() == EXIT_FAILURE);
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
#ifdef __APPLE__
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

    auto stop_and_logs() -> std::string
    {
        auto const pid = daemon->process_id();
        REQUIRE(pid);
        REQUIRE(::kill(static_cast<pid_t>(*pid), SIGTERM) == 0);
        until([&] { return exit.has_value(); }, true);
        REQUIRE(exit->kind == ProcessExitKind::Exited);
        REQUIRE(exit->exit_code == EXIT_SUCCESS);
        daemon.reset();
        observer.reset();
        REQUIRE(storage.database.open());
        return log;
    }

    auto rejected_configuration(std::string_view text) -> std::string
    {
        configuration = text;
        launch(false);
        until([&] { return exit.has_value(); }, true);
        REQUIRE(exit->kind == ProcessExitKind::Exited);
        REQUIRE(exit->exit_code == 2);
        CHECK_FALSE(std::filesystem::exists(socket_path));
        daemon.reset();
        REQUIRE(storage.database.open());
        return log;
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
        REQUIRE(output.find("API version: 1.4") != std::string::npos);
    }

    auto rpc(std::string_view method, JsonValue parameters) -> JsonValue
    {
        jb::net::LocalSocket socket;
        bool                 connected{false};
        socket.connected.connect(&app, [&] { connected = true; });
        socket.connect_to_server(socket_path);
        until([&] { return connected; });
        jb::rpc::Client                  client{socket};
        std::optional<JsonValue>         response;
        std::optional<jb::rpc::RpcError> error;
        client.result_received.connect(&app,
                                       [&](jb::rpc::RequestId const&, JsonValue const& value) { response = value; });
        client.error_received.connect(&app, [&](jb::rpc::RequestId const&, jb::rpc::RpcError const& value) {
            error = value;
        });
        REQUIRE(client.call(method, std::move(parameters)));
        until([&] { return response.has_value() || error.has_value(); });
        INFO((error ? error->message : ""));
        REQUIRE_FALSE(error);
        return std::move(*response);
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

    auto logged(std::string_view event) const -> bool { return log.find(event) != std::string::npos; }

    void padded_info()
    {
        jb::net::LocalSocket socket;
        bool                 connected = false;
        socket.connected.connect(&app, [&] { connected = true; });
        socket.connect_to_server(socket_path);
        until([&] { return connected; });

        // Whitespace enlarges the real frame without changing system.info's request contract.
        auto request = serialize_json(jb::rpc::detail::encode_request(std::uint64_t{1}, "system.info"));
        REQUIRE(request);
        auto body   = std::string((std::size_t{2} * 1024U * 1024U) + 1U, ' ') + *request;
        auto framed = jb::rpc::frame_message(body, {.max_body_bytes = std::size_t{3} * 1024U * 1024U});
        REQUIRE(framed);
        jb::rpc::StreamFramer    framer;
        std::optional<JsonValue> reply;
        socket.ready_read.connect(&app, [&] {
            auto bodies = framer.append(socket.read_all());
            REQUIRE(bodies);
            for (auto const& response : *bodies) {
                auto parsed = parse_json(response);
                REQUIRE(parsed);
                reply = std::move(*parsed);
            }
        });
        REQUIRE(socket.write(*framed) == framed->size());
        until([&] { return reply.has_value(); });
        CHECK(reply->as_object().contains("result"));
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
    std::string     configuration;

private:
    std::filesystem::path socket_path;
#ifndef __linux__
    unsigned incarnation{0};
#endif
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
    last.output               = jb::jobu::detail::AttemptOutput{.stdout_bytes = ByteBuffer{},
                                                                .stderr_bytes = ByteBuffer{},
                                                                .capture_lost = true};
    before.run.state          = retry ? RunState::RetryWait : RunState::Interrupted;
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
    auto const retry = policy == RecoveryPolicy::RetryInterrupted && !exhausted;
    CHECK(fixture.count(retry ? "SELECT count(*) FROM jobu_jobs WHERE state='active'"
                              : "SELECT count(*) FROM jobu_jobs WHERE state='failed'") == 1);
    fixture.crash();
    require_interrupted(fixture, running, retry);
    JobRepository jobs{fixture.storage.database, fixture.storage.registry};
    auto          current = jobs.find_by_id(job.id, false);
    REQUIRE(current);
    REQUIRE(*current);
    CHECK((*current)->state == (retry ? JobState::Active : JobState::Failed));
    CHECK((*current)->revision == job.revision + (retry ? 0U : 1U));
    if (!retry) {
        CHECK((*current)->updated_at == fixture.read_run(seed.run.id).run.completed_at);
    }
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
    expected.attempts.front().output               = jb::jobu::detail::AttemptOutput{.stdout_bytes = ByteBuffer{},
                                                                                     .stderr_bytes = ByteBuffer{},
                                                                                     .capture_lost = true};
    auto const& second                             = actual.attempts.back();
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

TEST_CASE("daemon validates the current schema before recovery and serving", "[jobud][recovery][schema][integration]")
{
    CrashFixture fixture;
    auto         queue   = recovery_queue(recovery_id(1));
    auto         job     = fixture.storage.make_job(recovery_id(2), queue.id, JobType::Http);
    auto         running = fixture.storage.make_run(recovery_id(3), job, RunState::Running);
    fixture.storage.insert_queue(queue);
    fixture.storage.insert_job(job);
    fixture.storage.insert_run(running);

    // start() includes a real system.info round trip after schema validation and recovery.
    fixture.start();
    CHECK(fixture.count("SELECT version FROM jobu_schema") == 4);
    CHECK(fixture.count("SELECT count(*) FROM sqlite_schema WHERE name IN ('jobu_runs_planned_id_idx', "
                        "'jobu_runs_queue_planned_id_idx', 'jobu_runs_job_planned_id_idx')") == 3);
    CHECK(fixture.count("SELECT count(*) FROM jobu_attempts WHERE state = 'running'") == 0);
    fixture.crash();
    require_interrupted(fixture, running, false);
    fixture.unchanged_restart();
}

TEST_CASE("daemon restart recovers mixed current-format work without losing references or replay records",
          "[jobud][recovery][phase8][integration]")
{
    CrashFixture fixture;
    fixture.server.enqueue_response({});
    fixture.start();

    auto active_queue = fixture.rpc("queue.create", json(R"({"name":"mixed-active"})"));
    auto held_queue   = fixture.rpc("queue.create", json(R"({"name":"mixed-held"})"));
    auto held_id      = Uuid::parse(held_queue.as_object().at("id").as_string());
    REQUIRE(held_id);
    auto held_selector = queue_selector_to_json(QueueSelector{*held_id});
    REQUIRE(held_selector);
    auto suspended = fixture.rpc("queue.suspend", *held_selector);
    CHECK(suspended.as_object().at("state").as_string() == "suspended");
    CHECK(active_queue.as_object().at("state").as_string() == "active");

    auto secret =
        fixture.rpc("secret.set",
                    json(R"({"name":"restart.token","value":{"encoding":"utf8","data":"private-restart-value"}})"));
    CHECK(secret.as_object().at("name").as_string() == "restart.token");
    CHECK_FALSE(secret.as_object().contains("value"));

    // The active one-time attempt stays in flight through the crash. The suspended queue holds a cron successor.
    auto  once_request = json(R"({"name":"mixed-once","queue_name":"mixed-active","type":"http",
        "schedule":{"kind":"once","at":"now"},"payload":{"url":"",
        "headers":[{"name":"X-Restart-Token","value":{"secret":"restart.token"}}]},
        "idempotency_key":"mixed-once-create"})");
    auto& once_payload = std::get<JsonValue::Object>(std::get<JsonValue::Object>(once_request.data).at("payload").data);
    once_payload.at("url")  = text(fixture.server.url());
    auto       once_created = fixture.rpc("job.create", once_request);
    auto const once_id_text = once_created.as_object().at("id").as_string();

    auto  cron_request = json(R"({"name":"mixed-cron","queue_name":"mixed-held","type":"http",
        "schedule":{"kind":"cron","expression":"0 0 1 1 *","timezone":"UTC"},
        "payload":{"url":""},"idempotency_key":"mixed-cron-create"})");
    auto& cron_payload = std::get<JsonValue::Object>(std::get<JsonValue::Object>(cron_request.data).at("payload").data);
    cron_payload.at("url")  = text(fixture.server.url());
    auto       cron_created = fixture.rpc("job.create", cron_request);
    auto const cron_id_text = cron_created.as_object().at("id").as_string();

    fixture.running(JobType::Http);
    auto once_runs = fixture.rpc("run.list", JsonValue{.data = JsonValue::Object{{"job_id", text(once_id_text)}}});
    REQUIRE(once_runs.as_object().at("items").as_array().size() == 1);
    auto run_id = Uuid::parse(once_runs.as_object().at("items").as_array().front().as_object().at("id").as_string());
    REQUIRE(run_id);
    CHECK(fixture.count("SELECT count(*) FROM jobu_runs WHERE state='scheduled'") == 1);
    fixture.crash();
    auto const interrupted = fixture.read_run(*run_id);

    fixture.start();
    CHECK(fixture.count("SELECT version FROM jobu_schema") == 4);
    CHECK(fixture.count("SELECT count(*) FROM jobu_runs") == 2);
    CHECK(fixture.count("SELECT count(*) FROM jobu_attempts WHERE state='running'") == 0);
    CHECK(fixture.server.requests().size() == 1);

    auto once_id = Uuid::parse(once_id_text);
    auto cron_id = Uuid::parse(cron_id_text);
    REQUIRE(once_id);
    REQUIRE(cron_id);
    auto once_selector = job_id_to_json(*once_id);
    auto cron_selector = job_id_to_json(*cron_id);
    REQUIRE(once_selector);
    REQUIRE(cron_selector);
    auto finished_once = fixture.rpc("job.get", *once_selector);
    auto retained_cron = fixture.rpc("job.get", *cron_selector);
    CHECK(finished_once.as_object().at("state").as_string() == "failed");
    CHECK(retained_cron.as_object().at("state").as_string() == "active");
    CHECK(finished_once.as_object().at("payload").as_object().at("headers").as_array().front().as_object().at(
              "value") == json(R"({"secret":"restart.token"})"));
    CHECK(fixture.rpc("queue.get", *held_selector).as_object().at("state").as_string() == "suspended");
    auto cron_runs = fixture.rpc("run.list", JsonValue{.data = JsonValue::Object{{"job_id", text(cron_id_text)}}});
    REQUIRE(cron_runs.as_object().at("items").as_array().size() == 1);
    CHECK(cron_runs.as_object().at("items").as_array().front().as_object().at("state").as_string() == "scheduled");

    // Saved create responses are observations of the earlier acceptance, not new execution after recovery.
    CHECK(fixture.rpc("job.create", once_request) == once_created);
    CHECK(fixture.rpc("job.create", cron_request) == cron_created);
    CHECK(fixture.count("SELECT count(*) FROM jobu_jobs") == 2);
    CHECK(fixture.count("SELECT count(*) FROM jobu_runs") == 2);
    CHECK(fixture.server.requests().size() == 1);

    fixture.crash();
    require_interrupted(fixture, interrupted, false);
    fixture.unchanged_restart();
}

TEST_CASE("daemon rejects an unfinished one-time owner with no work before serving", "[jobud][recovery][integration]")
{
    CrashFixture fixture;
    auto         queue = recovery_queue(recovery_id(1));
    fixture.storage.insert_queue(queue);
    auto broken = fixture.storage.make_job(recovery_id(2), queue.id, JobType::Http);
    fixture.storage.insert_job(broken);
    auto healthy = fixture.storage.make_job(recovery_id(4), queue.id, JobType::Http);
    fixture.storage.insert_job(healthy);
    auto running = fixture.storage.make_run(recovery_id(5), healthy, RunState::Running);
    fixture.storage.insert_run(running);

    auto before = storage_snapshot(fixture.storage.database);
    fixture.require_startup_failure("jobu.storage.invariant");
    CHECK(storage_snapshot(fixture.storage.database) == before);
    fixture.storage.require_run(running);
}

TEST_CASE("daemon schema rejection leaves recovery rows untouched and never listens",
          "[jobud][recovery][schema][integration]")
{
    auto const* const corrupt = GENERATE("DROP INDEX jobu_runs_job_state_idx",
                                         "ALTER TABLE jobu_jobs ADD COLUMN incompatible INTEGER",
                                         "UPDATE jobu_schema SET version = 2",
                                         "UPDATE jobu_schema SET version = 5");
    CAPTURE(corrupt);
    CrashFixture fixture;
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
    fixture.require_startup_failure("jobu.schema.");
    CHECK(storage_snapshot(fixture.storage.database) == before);
    fixture.storage.require_run(running);
}

TEST_CASE("Configured foreground daemon measures mixed work maintains history and preserves replay across restart",
          "[jobud][phase9][configuration][integration]")
{
    require_execution_environment(JobType::Cli);
    CrashFixture f;
    f.configuration = "schedule.default_timezone = Europe/Tallinn\n"
                      "defaults.retry.max_attempts = 1\n"
                      "history.default_retention = 1s\n"
                      "history.sweep_interval = 1s\n"
                      "history.batch_size = 1\n"
                      "telemetry.checkpoint_interval = 1s\n"
                      "rpc.header_limit_bytes = 1k\n"
                      "rpc.body_limit_bytes = 3m\n"
                      "rpc.queued_output_bytes = 4m\n"
                      "rpc.max_batch_entries = 2\n"
                      "rpc.max_connections = 4\n";

    // Old inherited history must expire; a finite default must preserve an unlimited override.
    auto queue                  = recovery_queue(recovery_id(1));
    queue.concurrency_limit     = 2;
    queue.runnable_wait_warning = 1ms;
    f.storage.insert_queue(queue);
    auto unlimited              = recovery_queue(recovery_id(2));
    unlimited.history_retention = 0s;
    f.storage.insert_queue(unlimited);
    for (auto id : {1U, 2U}) {
        auto job  = f.storage.make_job(recovery_id(id + 10), recovery_id(id));
        job.state = JobState::Succeeded;
        f.storage.insert_job(job);
        f.storage.insert_run(f.storage.make_run(recovery_id(id + 100), job, RunState::Succeeded));
    }
    f.start(true);
    f.padded_info();
    f.until([&] { return f.count("SELECT count(*) FROM jobu_runs WHERE state='succeeded'") == 1; });
    CHECK(f.count("SELECT count(*) FROM jobu_runs WHERE queue_id=x'00000000000070008000000000000002'") == 1);

    // Keep newly completed work while checking its statistics and restart replay. The inherited
    // finite policy above has already run through the production timer and repository.
    auto updated = update_queue_request_to_json({.queue = queue.id, .history_retention = std::chrono::seconds{0}},
                                                f.storage.registry);
    REQUIRE(updated);
    f.rpc("queue.update", *updated);

    auto cli = CreateJobRequest{
        .queue    = queue.id,
        .schedule = OnceSchedule{.planned_at = UtcClock::now()},
        .payload  = f.job(JobType::Cli).payload,
    };
    auto cli_request = create_job_request_to_json(cli, f.storage.registry);
    REQUIRE(cli_request);
    auto cli_created = f.rpc("job.create", *cli_request);
    CHECK(cli_created.as_object().at("attributes").as_object().at("retry.max_attempts").as_uint() == 1);
    std::optional<pid_t> target;
    f.until([&] {
        if (!target) {
            target = f.report.ready_pid();
        }
        return target.has_value();
    });

    auto http = CreateJobRequest{
        .queue    = queue.id,
        .type     = JobType::Http,
        .schedule = OnceSchedule{.planned_at = UtcClock::now()},
        .payload  = f.job(JobType::Http).payload,
    };
    auto http_request = create_job_request_to_json(http, f.storage.registry);
    REQUIRE(http_request);
    f.rpc("job.create", *http_request);
    f.until([&] { return f.server.requests().size() == 1; });
    REQUIRE(f.count("SELECT count(*) FROM jobu_runs WHERE state='running'") == 2);
    f.rpc("job.create", *http_request);

    // Held CLI/HTTP operations occupy both queue slots. Warning delivery and a persisted
    // checkpoint prove production observation also reaches the third, capacity-starved run.
    f.until([&] {
        return f.logged("jobud.run.delayed") &&
               f.count("SELECT count(*) FROM jobu_run_timing WHERE open_epoch IS NOT NULL "
                       "AND runnable_wait_us > 1000 AND delay_warned=1 AND measurement_status='complete'") == 1;
    });
    f.release.release();
    f.server.enqueue_response({});
    f.server.enqueue_response({});
    f.server.release_responses();
    f.until([&] { return f.count("SELECT count(*) FROM jobu_runs WHERE state='succeeded'") == 4; });

    auto query = queue_statistics_request_to_json(QueueStatisticsQuery{.selector = queue.id});
    REQUIRE(query);
    auto        stats = f.rpc("queue.stats", *query);
    auto const& group = stats.as_object().at("groups").as_array().front().as_object();
    CHECK(group.at("runnable_wait_coverage").as_object().at("complete").as_uint() == 3);
    CHECK(group.at("runnable_wait_ms").as_object().at("samples").as_uint() == 3);
    CHECK(stats.as_object().at("measurement").as_object().at("runnable_wait").as_string() == "monotonic_observed");

    auto cron = CreateJobRequest{
        .queue           = queue.id,
        .schedule        = CronScheduleInput{.expression = "@daily"},
        .payload         = JsonValue{.data = JsonValue::Object{{"command", text("/true")}}},
        .idempotency_key = "configured-cron-replay",
    };
    auto cron_request = create_job_request_to_json(cron, f.storage.registry);
    REQUIRE(cron_request);
    auto accepted = f.rpc("job.create", *cron_request);
    CHECK(accepted.as_object().at("schedule").as_object().at("timezone").as_string() == "Europe/Tallinn");
    CHECK(accepted.as_object().at("attributes").as_object().at("retry.max_attempts").as_uint() == 1);
    f.until([&] { return f.logged("jobud.retention.completed"); });
    auto output = f.stop_and_logs();
    CHECK(output.find("jobud.ready") < output.find("jobud.retention.completed"));
    CHECK(output.find("jobud.run.delayed") != std::string::npos);
    CHECK(output.find("jobud.stopped") != std::string::npos);

    f.configuration = "schedule.default_timezone = UTC\n"
                      "defaults.retry.max_attempts = 2\n"
                      "history.default_retention = 0\n";
    f.start();
    CHECK(f.rpc("job.create", *cron_request) == accepted);
    cron.idempotency_key = "configured-cron-new-defaults";
    auto new_request     = create_job_request_to_json(cron, f.storage.registry);
    REQUIRE(new_request);
    auto new_job = f.rpc("job.create", *new_request);
    CHECK(new_job.as_object().at("schedule").as_object().at("timezone").as_string() == "UTC");
    CHECK(new_job.as_object().at("attributes").as_object().at("retry.max_attempts").as_uint() == 2);
    f.stop_and_logs();
}

TEST_CASE("Configured daemon logs readiness and final graceful exit in JSON and text", "[jobud][logging][integration]")
{
    auto const   format = GENERATE(std::string{"json"}, std::string{"text"});
    CrashFixture fixture;
    fixture.configuration = "logging.format = " + format + "\nlogging.level = info\n";
    fixture.start();
    // Wait for the asynchronous initial sweep so lifecycle ordering is independent of process speed.
    fixture.until([&] { return fixture.logged("jobud.retention.completed"); });
    auto const               output    = fixture.stop_and_logs();
    auto                     remaining = std::string_view{output};
    std::vector<std::string> events;
    while (!remaining.empty()) {
        auto const end = remaining.find('\n');
        REQUIRE(end != std::string_view::npos);
        auto const line = remaining.substr(0, end);
        if (format == "json") {
            auto value = parse_json(line);
            REQUIRE(value);
            auto const& fields = value->as_object();
            events.push_back(fields.at("event").as_string());
            if (events.back() == "jobud.stopped") {
                CHECK(fields.at("fields").as_object().at("exit_status").as_uint() == 0);
            }
        }
        else {
            auto const start = line.find("event=\"");
            REQUIRE(start != std::string_view::npos);
            auto const event = line.substr(start + 7);
            events.emplace_back(event.substr(0, event.find('"')));
        }
        remaining.remove_prefix(end + 1);
    }
    auto expected = std::vector<std::string>{"jobud.starting"};
#ifdef __linux__
    if (::geteuid() == 0) {
        expected.emplace_back("jobud.unsafe.root_daemon");
    }
#endif
    expected.insert(expected.end(),
                    {"jobud.recovery.completed", "jobud.ready", "jobud.retention.completed", "jobud.stopped"});
    REQUIRE(events == expected);
}

TEST_CASE("Configured daemon filters lifecycle output without changing readiness", "[jobud][logging][integration]")
{
    CrashFixture fixture;
    fixture.configuration = "logging.level = error\n";
    fixture.start();
    CHECK(fixture.stop_and_logs().empty());
}

TEST_CASE("Rejected daemon configuration does not print sentinel values", "[jobud][logging][configuration]")
{
    auto const   text = GENERATE(std::string{"cli.concurrency = stage916-private-command\n"},
                                 std::string{"http.proxy = stage916-private-url-credentials\n"},
                                 std::string{"defaults.retry.mode = stage916-private-payload\n"});
    CrashFixture fixture;
    auto const   output = fixture.rejected_configuration(text);
    CHECK(output.find("stage916-private-") == std::string::npos);
    CHECK(output.find("jobud.config.") != std::string::npos);
}

TEST_CASE("Rendered daemon CLI and HTTP failure logs omit execution sentinels", "[jobud][logging][integration]")
{
    auto const type = GENERATE(JobType::Cli, JobType::Http);
    require_execution_environment(type);
    CrashFixture fixture;
    fixture.configuration                        = "logging.level = debug3\n";
    auto queue                                   = recovery_queue(recovery_id(1));
    auto job                                     = fixture.job(type);
    job.name                                     = "stage916-private-job-name";
    job.attributes.at("retry.max_attempts").data = std::int64_t{1};
    if (type == JobType::Cli) {
        job.payload.data = JsonValue::Object{
            {"command",     text("/stage916-private-command-missing")                                   },
            {"arguments",   {.data = JsonValue::Array{text("stage916-private-argument")}}               },
            {"environment", {.data = JsonValue::Object{{"TOKEN", text("stage916-private-environment")}}}},
        };
    }
    else {
        job.payload.data = JsonValue::Object{
            {"url",     text(fixture.server.url("/stage916-private-url-credentials"))                              },
            {"method",  text("POST")                                                                               },
            {"headers",
             {.data =
                  JsonValue::Array{{.data = JsonValue::Object{{"name", text("Authorization")},
                                                              {"value", text("Bearer stage916-private-header")}}}}}},
            {"body",
             {.data = JsonValue::Object{{"encoding", text("utf8")}, {"data", text("stage916-private-payload")}}}   },
        };
        fixture.server.enqueue_response({
            .status_code = 503,
            .reason      = "Unavailable",
            .body        = ByteBuffer{as_bytes("stage916-private-response").begin(),
                                      as_bytes("stage916-private-response").end()}
        });
        fixture.server.release_responses();
    }
    auto run = fixture.storage.make_run(recovery_id(3), job);
    fixture.storage.insert_queue(queue);
    fixture.storage.insert_job(job);
    fixture.storage.insert_run(run);
    fixture.start(type == JobType::Cli);
    fixture.until([&] { return fixture.count("SELECT count(*) FROM jobu_runs WHERE state='failed'") == 1; });
    auto const output = fixture.stop_and_logs();
    CHECK(output.find("stage916-private-") == std::string::npos);
    auto remaining = std::string_view{output};
    while (!remaining.empty()) {
        auto const end = remaining.find('\n');
        REQUIRE(end != std::string_view::npos);
        REQUIRE(parse_json(remaining.substr(0, end)));
        remaining.remove_prefix(end + 1);
    }
}
