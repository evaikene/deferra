#include "application.hpp"
#include "byte_buffer.hpp"
#include "client.hpp"
#include "event_loop.hpp"
#include "json.hpp"
#include "local_socket.hpp"
#include "process.hpp"
#include "protocol.hpp"
#include "support/http_test_server.hpp"
#include "support/temporary_directory.hpp"
#include "uuid.hpp"

#include <catch2/catch_test_macros.hpp>
#include <sqlite3.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <csignal> // IWYU pragma: keep Provides POSIX signal constants.
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant> // IWYU pragma: keep std::get accesses the mutable JsonValue alternative.
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__APPLE__)
#  include <libproc.h>
#  include <sys/event.h>
#elif defined(__linux__)
#  include <poll.h>
#  include <sys/syscall.h>
#endif

using namespace jb::core;
using namespace std::chrono_literals;

namespace {

auto text(std::string value) -> JsonValue
{
    return {.data = std::move(value)};
}

/// @throws Catch::TestFailureException if a fixture or durable JSON document is invalid.
auto json(std::string_view value) -> JsonValue
{
    auto parsed = parse_json(value);
    REQUIRE(parsed);
    return std::move(*parsed);
}

void require_execution_environment()
{
    // The opt-in is supplied only by a disposable CI container, never inferred from UID 0 alone.
    auto const* enabled = std::getenv("JOBU_TEST_ALLOW_ROOT_CLI");
    if (::geteuid() == 0 && (!enabled || std::string_view{enabled} != "1")) {
        SKIP("root target execution requires JOBU_TEST_ALLOW_ROOT_CLI=1 in an isolated test environment");
    }
}

/// An owned FIFO makes helper readiness/release independent of scheduling delays.
class Channel {
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

    template <std::size_t Count>
    auto identities() const -> std::optional<std::array<pid_t, Count>>
    {
        std::array<pid_t, Count> values{};
        auto const               count = ::read(_fd, values.data(), sizeof(values));
        if (count < 0 && (errno == EAGAIN || errno == EINTR)) {
            return {};
        }
        REQUIRE(count == sizeof(values));
        return values;
    }

private:
    std::filesystem::path _path;
    int                   _fd{-1};
};

/// Native exit observation; macOS helpers self-expire if daemon failure prevents normal timeout cleanup.
class TargetWatch {
public:
    /// @throws Catch::TestFailureException when the live coordinated helper cannot be watched.
    explicit TargetWatch(pid_t pid)
        : _pid{pid}
    {
#if defined(__APPLE__)
        _fd = ::kqueue();
        REQUIRE(_fd >= 0);
        // Register before releasing the helper. The one-shot exit remains observable after the daemon reaps it.
        struct kevent change;
        EV_SET(&change, pid, EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT, 0, nullptr);
        int registered;
        do {
            registered = ::kevent(_fd, &change, 1, nullptr, 0, nullptr);
        } while (registered < 0 && errno == EINTR);
        if (registered < 0) {
            auto const error = errno;
            ::close(_fd);
            _fd = -1;
            FAIL("kevent registration failed with errno " << error);
        }
#else
        _fd = static_cast<int>(::syscall(SYS_pidfd_open, _pid, 0));
        REQUIRE(_fd >= 0);
#endif
    }

    ~TargetWatch()
    {
#if defined(__linux__)
        // Numeric PIDs may have been reused. A pidfd signal can only affect the original helper.
        static_cast<void>(::syscall(SYS_pidfd_send_signal, _fd, SIGKILL, nullptr, 0));
#endif
        // kqueue provides observation, not identity-safe signalling. Helper alarms bound failed-test lifetimes.
        ::close(_fd);
    }

    TargetWatch(TargetWatch const&)                    = delete;
    auto operator=(TargetWatch const&) -> TargetWatch& = delete;

    /// @throws Catch::TestFailureException when native exit observation fails.
    auto terminated() -> bool
    {
        // Retain consumed NOTE_EXIT events while the caller waits for another group member.
        if (_terminated) {
            return true;
        }
#if defined(__APPLE__)
        struct kevent   event;
        struct timespec timeout{};
        int             ready;
        do {
            ready = ::kevent(_fd, nullptr, 0, &event, 1, &timeout);
        } while (ready < 0 && errno == EINTR);
        REQUIRE(ready >= 0);
        if (ready == 1) {
            REQUIRE(event.filter == EVFILT_PROC);
            REQUIRE(event.ident == static_cast<std::uintptr_t>(_pid));
            REQUIRE((event.fflags & NOTE_EXIT) != 0);
            _terminated = true;
        }
#else
        pollfd     descriptor{.fd = _fd, .events = POLLIN, .revents = 0};
        auto const ready = ::poll(&descriptor, 1, 0);
        REQUIRE(ready >= 0);
        _terminated = ready == 1 && (descriptor.revents & POLLIN) != 0;
#endif
        return _terminated;
    }

private:
    pid_t _pid;
    int   _fd{-1};
    bool  _terminated{false};
};

struct DurableAttempt {
    std::string                job_id;
    std::string                run_id;
    std::string                run_state;
    std::string                attempt_state;
    std::string                outcome;
    JsonValue                  result;
    std::optional<std::string> output;
    std::optional<std::string> diagnostic;
    bool                       stdout_truncated{false};
    bool                       stderr_truncated{false};
    bool                       capture_lost{false};
};

auto column_bytes(sqlite3_stmt* statement, int column) -> std::string
{
    auto const* data = static_cast<char const*>(sqlite3_column_blob(statement, column));
    auto const  size = sqlite3_column_bytes(statement, column);
    return data ? std::string{data, static_cast<std::size_t>(size)} : std::string{};
}

auto column_uuid(sqlite3_stmt* statement, int column) -> std::string
{
    Uuid::Storage bytes{};
    REQUIRE(sqlite3_column_bytes(statement, column) == bytes.size());
    std::memcpy(bytes.data(), sqlite3_column_blob(statement, column), bytes.size());
    return Uuid{bytes}.to_string();
}

/// Real daemon/client lifecycle with atomic read-only observation of durable completion.
/// @throws Catch::TestFailureException when setup, RPC, or a bounded readiness check fails.
class DaemonFixture {
public:
    explicit DaemonFixture(std::uint32_t cli_concurrency = 2, bool allow_root = true)
    {
        auto arguments = std::vector<std::string>{"--socket",
                                                  socket_path.string(),
                                                  "--database",
                                                  database_path.string(),
                                                  "--cli-concurrency",
                                                  std::to_string(cli_concurrency),
                                                  "--http-concurrency",
                                                  "1"};
        if (::geteuid() == 0 && allow_root) {
            arguments.emplace_back("--allow-root-cli");
        }
        daemon.standard_output.connect(&app, [&](ByteBuffer const& chunk) { log.append(as_string_view(chunk)); });
        daemon.standard_error.connect(&app, [&](ByteBuffer const& chunk) { log.append(as_string_view(chunk)); });
        daemon.finished.connect(&app, [&](ProcessExit const&) { exited = true; });
        REQUIRE(daemon.start({
            .executable        = JOBUD_EXECUTABLE,
            .arguments         = std::move(arguments),
            .environment       = {{"HOME", "/ambient-home"},
                                  {"PATH", "/ambient-path"},
                                  {"AMBIENT_SECRET", "daemon-ambient-marker"}},
            .termination_grace = 0ms
        }));
        until([&] {
            std::error_code error;
            return std::filesystem::is_socket(socket_path, error);
        });
        sqlite3*   raw{};
        auto const opened = sqlite3_open_v2(database_path.c_str(), &raw, SQLITE_OPEN_READONLY, nullptr);
        database.reset(raw);
        REQUIRE(opened == SQLITE_OK);
        REQUIRE(sqlite3_busy_timeout(database.get(), 100) == SQLITE_OK);
        sqlite3_stmt* version{};
        REQUIRE(sqlite3_prepare_v2(database.get(), "SELECT version FROM jobu_schema", -1, &version, nullptr) ==
                SQLITE_OK);
        auto statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>{version, sqlite3_finalize};
        REQUIRE(sqlite3_step(version) == SQLITE_ROW);
        REQUIRE(sqlite3_column_int(version, 0) == 2);
        REQUIRE(sqlite3_step(version) == SQLITE_DONE);
    }

    ~DaemonFixture()
    {
        // This fixture lets bounded attempts finish before Process destruction. It does not assert
        // Phase 7's immediate signal-shutdown behavior with active work; that has a separate test stage.
        auto const deadline = Clock::now() + 8s;
        while (Clock::now() < deadline && has_running_attempt()) {
            if (app.process_events(EventFlag::All, 10) == ProcessEventsResult::Failed) {
                break;
            }
        }
        daemon.standard_output.disconnect_all();
        daemon.standard_error.disconnect_all();
        daemon.finished.disconnect_all();
    }

    template <typename Predicate>
    void until(Predicate predicate)
    {
        auto const deadline = Clock::now() + 8s;
        while (!predicate() && !exited && Clock::now() < deadline) {
            REQUIRE(app.process_events(EventFlag::All, 10) != ProcessEventsResult::Failed);
        }
        INFO(log);
        REQUIRE_FALSE(exited);
        REQUIRE(predicate());
    }

    auto control(std::vector<std::string> arguments) -> std::string
    {
        std::string                output;
        std::optional<ProcessExit> exit;
        Process                    client;
        client.standard_output.connect(&app, [&](ByteBuffer const& chunk) { output.append(as_string_view(chunk)); });
        client.standard_error.connect(&app, [&](ByteBuffer const& chunk) { output.append(as_string_view(chunk)); });
        client.finished.connect(&app, [&](ProcessExit const& value) { exit = value; });
        arguments.insert(arguments.begin(), {"--socket", socket_path.string()});
        REQUIRE(client.start({.executable        = JOBUCTL_EXECUTABLE,
                              .arguments         = std::move(arguments),
                              .timeout           = 5s,
                              .termination_grace = 0ms}));
        until([&] { return exit.has_value(); });
        INFO(output);
        REQUIRE(exit->kind == ProcessExitKind::Exited);
        REQUIRE(exit->exit_code == 0);
        return output;
    }

    auto rpc(std::string_view method, JsonValue params) -> JsonValue
    {
        // The production framing/client stack supplies attributes absent from the intentionally small CLI surface.
        bool                 connected{false};
        jb::net::LocalSocket socket;
        socket.connected.connect(&app, [&] { connected = true; });
        socket.connect_to_server(socket_path);
        until([&] { return connected; });
        std::optional<JsonValue>         response;
        std::optional<jb::rpc::RpcError> error;
        jb::rpc::Client                  client{socket};
        client.result_received.connect(&app,
                                       [&](jb::rpc::RequestId const&, JsonValue const& value) { response = value; });
        client.error_received.connect(&app, [&](jb::rpc::RequestId const&, jb::rpc::RpcError const& value) {
            error = value;
        });
        REQUIRE(client.call(method, std::move(params)));
        until([&] { return response || error; });
        INFO((error ? error->message : ""));
        REQUIRE_FALSE(error);
        return std::move(*response);
    }

    void queue(std::string name, std::uint32_t concurrency = 4)
    {
        auto request = json(R"({"name":"","concurrency_limit":4,"defaults":{"job.timeout":5000,
            "cli.termination_grace":0,"retry.max_attempts":1,"output.capture":"always"}})");
        std::get<JsonValue::Object>(request.data).at("name") = text(std::move(name));
        std::get<JsonValue::Object>(request.data).at("concurrency_limit").data =
            static_cast<std::uint64_t>(concurrency);
        static_cast<void>(rpc("queue.create", std::move(request)));
    }

    void cli(std::string              name,
             std::string              queue_name,
             std::vector<std::string> arguments,
             std::vector<std::string> options = {})
    {
        auto command = std::vector<std::string>{"job",
                                                "create",
                                                "--name",
                                                std::move(name),
                                                "--queue-name",
                                                std::move(queue_name),
                                                "--type",
                                                "cli",
                                                "--at",
                                                "2000-01-01T00:00:00Z",
                                                "--command",
                                                PROCESS_TEST_HELPER};
        for (auto& argument : arguments) {
            command.emplace_back("--arg");
            command.push_back(std::move(argument));
        }
        command.insert(command.end(), options.begin(), options.end());
        static_cast<void>(control(std::move(command)));
    }

    void http(std::string name, std::string queue_name, std::string url)
    {
        static_cast<void>(control({"job",
                                   "create",
                                   "--name",
                                   std::move(name),
                                   "--queue-name",
                                   std::move(queue_name),
                                   "--type",
                                   "http",
                                   "--at",
                                   "2000-01-01T00:00:00Z",
                                   "--url",
                                   std::move(url)}));
    }

    auto state(std::string const& name) const -> DurableAttempt
    {
        // One statement gives an atomic view of run, attempt, result, and both binary output columns.
        constexpr auto sql =
            "SELECT j.id,r.id,r.state,a.state,a.outcome,a.result_json,o.stdout_blob,o.stderr_blob,"
            "o.stdout_truncated,o.stderr_truncated,o.capture_lost,r.result_json,a.attempt_number FROM jobu_jobs j "
            "JOIN jobu_runs r ON r.job_id=j.id LEFT JOIN jobu_attempts a ON a.run_id=r.id "
            "LEFT JOIN jobu_attempt_output o ON o.run_id=a.run_id AND o.attempt_number=a.attempt_number WHERE j.name=?";
        sqlite3_stmt* raw{};
        REQUIRE(sqlite3_prepare_v2(database.get(), sql, -1, &raw, nullptr) == SQLITE_OK);
        auto statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>{raw, sqlite3_finalize};
        REQUIRE(sqlite3_bind_text(raw, 1, name.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK);
        REQUIRE(sqlite3_step(raw) == SQLITE_ROW);
        DurableAttempt value{.job_id        = column_uuid(raw, 0),
                             .run_id        = column_uuid(raw, 1),
                             .run_state     = column_bytes(raw, 2),
                             .attempt_state = column_bytes(raw, 3),
                             .outcome       = column_bytes(raw, 4)};
        if (sqlite3_column_type(raw, 5) != SQLITE_NULL) {
            value.result = json(column_bytes(raw, 5));
        }
        if (sqlite3_column_type(raw, 6) != SQLITE_NULL) {
            value.output     = column_bytes(raw, 6);
            value.diagnostic = column_bytes(raw, 7);
        }
        value.stdout_truncated = sqlite3_column_int(raw, 8) != 0;
        value.stderr_truncated = sqlite3_column_int(raw, 9) != 0;
        value.capture_lost     = sqlite3_column_int(raw, 10) != 0;
        if (!value.attempt_state.empty()) {
            CHECK(sqlite3_column_int(raw, 12) == 1);
        }
        if (value.attempt_state == "completed") {
            CHECK(json(column_bytes(raw, 11)) == value.result);
        }
        REQUIRE(sqlite3_step(raw) == SQLITE_DONE);
        return value;
    }

    auto complete(std::string const& name, std::string_view expected = "succeeded") -> DurableAttempt
    {
        until([&] { return state(name).attempt_state == "completed"; });
        auto value = state(name);
        CHECK(value.run_state == expected);
        CHECK(value.outcome == expected);
        return value;
    }

    auto thread_count() const -> std::size_t
    {
        auto const pid = daemon.process_id();
        REQUIRE(pid);
#if defined(__APPLE__)
        // Query the owned, still-running daemon directly; no debugger/task-port entitlement is needed.
        struct proc_taskinfo info{};
        auto const           size = ::proc_pidinfo(static_cast<int>(*pid), PROC_PIDTASKINFO, 0, &info, sizeof(info));
        REQUIRE(size == sizeof(info));
        REQUIRE(info.pti_threadnum > 0);
        return static_cast<std::size_t>(info.pti_threadnum);
#else
        auto const  path = std::filesystem::path{"/proc"} / std::to_string(*pid) / "task";
        std::size_t count{0};
        for ([[maybe_unused]] auto const& entry : std::filesystem::directory_iterator{path}) {
            ++count;
        }
        return count;
#endif
    }

    void responsive()
    {
        CHECK(control({"system", "info"}).find("1.2") != std::string::npos);
        CHECK(log.find("daemon-ambient-marker") == std::string::npos);
        CHECK(log.find("literal $x = value") == std::string::npos);
        if (::geteuid() == 0) {
            CHECK(log.find("UNSAFE: --allow-root-cli enables command execution as root") != std::string::npos);
        }
    }

    Application                                        app{0, nullptr};
    jb::test::TemporaryDirectory                       directory;
    std::filesystem::path                              socket_path{directory.path() / "daemon.sock"};
    std::filesystem::path                              database_path{directory.path() / "daemon.sqlite"};
    std::string                                        log;
    bool                                               exited{false};
    Process                                            daemon;
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> database{nullptr, sqlite3_close};

private:
    auto has_running_attempt() const -> bool
    {
        if (!database) {
            return false;
        }
        sqlite3_stmt* raw{};
        auto const    prepared  = sqlite3_prepare_v2(database.get(),
                                                     "SELECT 1 FROM jobu_attempts WHERE state='running' LIMIT 1",
                                                     -1,
                                                     &raw,
                                                     nullptr);
        auto          statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>{raw, sqlite3_finalize};
        return prepared == SQLITE_OK && sqlite3_step(raw) == SQLITE_ROW;
    }
};

void check_capture(DurableAttempt const& value,
                   std::string const&    output,
                   std::string const&    diagnostic,
                   std::size_t           stdout_total,
                   std::size_t           stderr_total)
{
    REQUIRE(value.output);
    REQUIRE(value.diagnostic);
    CHECK(*value.output == output);
    CHECK(*value.diagnostic == diagnostic);
    CHECK_FALSE(value.capture_lost);
    CHECK(value.stdout_truncated == (output.size() < stdout_total));
    CHECK(value.stderr_truncated == (diagnostic.size() < stderr_total));
    auto const& result = value.result.as_object();
    CHECK(result.at("type").as_string() == "cli");
    CHECK_FALSE(result.at("capture_lost").as_bool());
    for (auto const& [channel, captured, total] : {
             std::tuple{"stdout", output.size(),     stdout_total},
             std::tuple{"stderr", diagnostic.size(), stderr_total}
    }) {
        auto expected =
            json("{\"captured_bytes\":" + std::to_string(captured) + ",\"total_bytes\":" + std::to_string(total) +
                 ",\"truncated\":" + (captured < total ? "true}" : "false}"));
        CAPTURE(channel);
        auto actual_json = serialize_json(result.at(channel));
        REQUIRE(actual_json);
        INFO(*actual_json);
        CHECK(result.at(channel) == expected);
    }
}

} // namespace

TEST_CASE("daemon executes complete jobuctl CLI payload and persists exact context", "[jobud][cli][integration]")
{
    require_execution_environment();
    DaemonFixture fixture;
    fixture.queue("context");
    fixture.cli("context",
                "context",
                {"inspect-daemon", "", "-option", "literal $x = value"},
                {"--working-directory",
                 fixture.directory.path().string(),
                 "--env",
                 "PROCESS_MARKER=literal $x = value",
                 "--env",
                 "EMPTY=",
                 "--unset-env",
                 "HOME",
                 "--expected-exit-code",
                 "37"});
    auto const value    = fixture.complete("context");
    auto       expected = json(R"({"arguments":["","-option","literal $x = value"],"cwd":"","environment":{
        "EMPTY":"","PROCESS_MARKER":"literal $x = value","JOBU_JOB_ID":"","JOBU_RUN_ID":"","JOBU_ATTEMPT":"1"},
        "stdin_eof":true})");
    // getcwd reports the physical directory even when the requested path uses macOS's /tmp symlink.
    std::get<JsonValue::Object>(expected.data).at("cwd") =
        text(std::filesystem::canonical(fixture.directory.path()).string());
    auto& environment = std::get<JsonValue::Object>(std::get<JsonValue::Object>(expected.data).at("environment").data);
    environment.at("JOBU_JOB_ID") = text(value.job_id);
    environment.at("JOBU_RUN_ID") = text(value.run_id);
    auto serialized               = serialize_json(expected);
    REQUIRE(serialized);
    check_capture(value, *serialized, std::string{"e\0r\n", 4}, serialized->size(), 4);
    CHECK(value.result.as_object().at("outcome").as_string() == "success");
    CHECK(value.result.as_object().at("exit_code").as_uint() == 37);
    fixture.responsive();
}

TEST_CASE("daemon preserves binary first and last capture and discarded-capture metadata", "[jobud][cli][integration]")
{
    require_execution_environment();
    DaemonFixture fixture;
    fixture.queue("capture");
    for (auto const* mode : {"always", "on_error", "none"}) {
        auto  request     = json(R"({"name":"","queue_name":"capture","type":"cli",
            "schedule":{"kind":"once","at":"2000-01-01T00:00:00Z"},
            "attributes":{"output.capture":"","output.stdout_limit":9,"output.stderr_limit":8},
            "payload":{"command":"","arguments":["output","131073","131079"],"expected_exit_codes":[37]}})");
        auto& fields      = std::get<JsonValue::Object>(request.data);
        fields.at("name") = text(mode);
        std::get<JsonValue::Object>(fields.at("attributes").data).at("output.capture") = text(mode);
        std::get<JsonValue::Object>(fields.at("payload").data).at("command")           = text(PROCESS_TEST_HELPER);
        static_cast<void>(fixture.rpc("job.create", std::move(request)));
        auto const value    = fixture.complete(mode);
        // The expected head/tail bytes derive from the helper's documented channel pattern, not the capture buffer.
        auto       retained = [](std::size_t total, std::size_t limit, std::size_t channel) {
            std::string bytes;
            auto const  head = (limit + 1) / 2;
            for (std::size_t index = 0; index < limit; ++index) {
                auto const offset = index < head ? index : total - limit + index;
                bytes.push_back(static_cast<char>((offset + (channel * 73)) % 251));
            }
            return bytes;
        };
        if (std::string_view{mode} == "always") {
            check_capture(value, retained(131073, 9, 0), retained(131079, 8, 1), 131073, 131079);
        }
        else {
            CHECK_FALSE(value.output);
            CHECK_FALSE(value.diagnostic);
        }
        auto expected = json(std::string_view{mode} == "none"
                                 ? R"({"type":"cli","outcome":"success","exit_code":37,"capture_lost":false,
                  "stdout":{"captured_bytes":0,"total_bytes":131073,"truncated":true},
                  "stderr":{"captured_bytes":0,"total_bytes":131079,"truncated":true}})"
                                 : R"({"type":"cli","outcome":"success","exit_code":37,"capture_lost":false,
                  "stdout":{"captured_bytes":9,"total_bytes":131073,"truncated":true},
                  "stderr":{"captured_bytes":8,"total_bytes":131079,"truncated":true}})");
        CHECK(value.result == expected);
    }
#if defined(__linux__)
    fixture.cli("hardening", "capture", {"no-new-privileges"});
    auto const hardened = fixture.complete("hardening");
    check_capture(hardened, "NoNewPrivs: 1\n", "", 14, 0);
#endif
    fixture.responsive();
}

TEST_CASE("daemon overlaps CLI targets and HTTP with independent global slots", "[jobud][cli][integration]")
{
    require_execution_environment();
    DaemonFixture            fixture{2};
    jb::test::HttpTestServer server;
    server.enqueue_response({});
    fixture.queue("overlap");
    auto const idle_threads = fixture.thread_count();
#if defined(__linux__)
    REQUIRE(idle_threads == 1);
#endif
    // macOS runtime initialization can create background threads. CLI overlap must not add per-attempt waiters.
    Channel report1{fixture.directory.path() / "report1"};
    Channel report2{fixture.directory.path() / "report2"};
    Channel release1{fixture.directory.path() / "release1"};
    Channel release2{fixture.directory.path() / "release2"};
    fixture.cli("first", "overlap", {"daemon-wait", report1.path(), release1.path()});
    fixture.cli("second", "overlap", {"daemon-wait", report2.path(), release2.path()});
    std::optional<std::array<pid_t, 1>> first;
    std::optional<std::array<pid_t, 1>> second;
    fixture.until([&] {
        if (!first) {
            first = report1.identities<1>();
        }
        return first.has_value();
    });
    TargetWatch first_watch{first->front()};
    fixture.until([&] {
        if (!second) {
            second = report2.identities<1>();
        }
        return second.has_value();
    });
    TargetWatch second_watch{second->front()};
    fixture.cli("third", "overlap", {"exit", "0"});
    fixture.http("http", "overlap", server.url());
    REQUIRE(server.wait_for_requests(1, 3s));
    CHECK(fixture.state("first").attempt_state == "running");
    CHECK(fixture.state("second").attempt_state == "running");
    CHECK(fixture.state("http").attempt_state == "running");
    CHECK(fixture.state("third").run_state == "scheduled");
    CHECK_FALSE(first_watch.terminated());
    CHECK_FALSE(second_watch.terminated());
    CHECK(fixture.thread_count() == idle_threads);
    server.release_responses();
    static_cast<void>(fixture.complete("http"));
    CHECK(fixture.state("third").run_state == "scheduled");
    release1.release();
    static_cast<void>(fixture.complete("first"));
    static_cast<void>(fixture.complete("third"));
    CHECK_FALSE(second_watch.terminated());
    release2.release();
    static_cast<void>(fixture.complete("second"));
    fixture.responsive();
}

TEST_CASE("daemon enforces queue capacity across runner families", "[jobud][cli][integration]")
{
    require_execution_environment();
    DaemonFixture            fixture;
    jb::test::HttpTestServer server;
    server.enqueue_response({});
    fixture.queue("serial", 1);
    fixture.queue("independent");
    Channel report{fixture.directory.path() / "report"};
    Channel release{fixture.directory.path() / "release"};
    fixture.cli("leader", "serial", {"daemon-wait", report.path(), release.path()});
    std::optional<std::array<pid_t, 1>> identity;
    fixture.until([&] {
        if (!identity) {
            identity = report.identities<1>();
        }
        return identity.has_value();
    });
    TargetWatch target{identity->front()};
    fixture.http("follower", "serial", server.url());
    // A separate eligible job completing proves scheduling progressed while the shared queue remained full.
    fixture.cli("barrier", "independent", {"exit", "0"});
    static_cast<void>(fixture.complete("barrier"));
    CHECK(fixture.state("leader").run_state == "running");
    CHECK(fixture.state("follower").run_state == "scheduled");
    CHECK(server.requests().empty());
    release.release();
    static_cast<void>(fixture.complete("leader"));
    REQUIRE(server.wait_for_requests(1, 3s));
    server.release_responses();
    static_cast<void>(fixture.complete("follower"));
    fixture.responsive();
}

TEST_CASE("daemon timeout kills the helper group before committing completion", "[jobud][cli][integration]")
{
    require_execution_environment();
    DaemonFixture fixture{1};
    fixture.queue("timeout", 1);
    Channel report{fixture.directory.path() / "group"};
    fixture.cli("timeout", "timeout", {"daemon-group-wait", report.path()});
    std::optional<std::array<pid_t, 2>> identities;
    fixture.until([&] {
        if (!identities) {
            identities = report.identities<2>();
        }
        return identities.has_value();
    });
    TargetWatch leader{(*identities)[0]};
    TargetWatch descendant{(*identities)[1]};
    CHECK_FALSE(leader.terminated());
    CHECK_FALSE(descendant.terminated());
    fixture.cli("follower", "timeout", {"exit", "0"});
    CHECK(fixture.state("follower").run_state == "scheduled");
    auto const value = fixture.complete("timeout", "failed");
    fixture.until([&] { return leader.terminated() && descendant.terminated(); });
    CHECK(value.result.as_object().at("outcome").as_string() == "timeout");
    CHECK(value.result.as_object().at("signal").as_uint() == SIGKILL);
    CHECK_FALSE(value.result.as_object().contains("exit_code"));
    check_capture(value, "", "", 0, 0);
    static_cast<void>(fixture.complete("follower"));
    fixture.responsive();
}

TEST_CASE("root daemon denies CLI targets without the unsafe override", "[jobud][cli][integration][root]")
{
    if (::geteuid() != 0) {
        SKIP("real root daemon denial requires a root test process");
    }
    DaemonFixture            fixture{1, false};
    jb::test::HttpTestServer server;
    server.enqueue_response({});
    server.release_responses();
    fixture.queue("root");
    auto const sentinel = fixture.directory.path() / "must-not-execute";
    fixture.cli("denied", "root", {"marker", sentinel.string()});
    fixture.http("http", "root", server.url());
    static_cast<void>(fixture.complete("http"));
    CHECK(fixture.state("denied").run_state == "scheduled");
    CHECK(fixture.state("denied").attempt_state.empty());
    CHECK_FALSE(std::filesystem::exists(sentinel));
    CHECK(fixture.log.find("UNSAFE:") == std::string::npos);
    CHECK(fixture.control({"system", "info"}).find("1.2") != std::string::npos);
}
