#include "support/http_test_server.hpp"
#include "support/temporary_directory.hpp"

#include "byte_buffer.hpp"

#include <fmt/format.h>

#include <sqlite3.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#if defined(__APPLE__)
#  include <crt_externs.h>
#endif
#include <fcntl.h>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <optional>
#include <spawn.h>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

auto process_environment() noexcept -> char**
{
#if defined(__APPLE__)
    // macOS exposes the process environment through this accessor instead of declaring environ in unistd.h.
    return *_NSGetEnviron();
#else
    return environ;
#endif
}

class ChildProcess {
public:
    explicit ChildProcess(pid_t pid)
        : _pid{pid}
    {}

    ~ChildProcess() { terminate(); }

    ChildProcess(ChildProcess const&)                    = delete;
    auto operator=(ChildProcess const&) -> ChildProcess& = delete;

    ChildProcess(ChildProcess&& other) noexcept
        : _pid{std::exchange(other._pid, -1)}
        , _status{std::exchange(other._status, std::nullopt)}
    {}

    auto operator=(ChildProcess&& other) noexcept -> ChildProcess&
    {
        if (this != &other) {
            terminate();
            _pid    = std::exchange(other._pid, -1);
            _status = std::exchange(other._status, std::nullopt);
        }
        return *this;
    }

    [[nodiscard]] auto try_wait() -> std::optional<int>
    {
        if (_status || _pid < 0) {
            return _status;
        }

        auto status = 0;
        auto result = pid_t{};
        do {
            result = ::waitpid(_pid, &status, WNOHANG);
        } while (result < 0 && errno == EINTR);

        if (result == _pid) {
            _pid    = -1;
            _status = status;
        }
        return _status;
    }

    void terminate() noexcept
    {
        if (_pid < 0) {
            return;
        }

        static_cast<void>(::kill(_pid, SIGTERM));
        auto const deadline = std::chrono::steady_clock::now() + 500ms;
        while (std::chrono::steady_clock::now() < deadline) {
            if (try_wait()) {
                return;
            }
            std::this_thread::sleep_for(10ms);
        }

        static_cast<void>(::kill(_pid, SIGKILL));
        auto status = 0;
        while (::waitpid(_pid, &status, 0) < 0 && errno == EINTR) {
        }
        _pid    = -1;
        _status = status;
    }

private:
    pid_t              _pid{-1};
    std::optional<int> _status;
};

struct CommandResult {
    bool        completed{false};
    int         status{0};
    std::string output;
};

auto fail(std::string_view message) -> int
{
    fmt::print(stderr, "jobud-http-integration-test: {}\n", message);
    return EXIT_FAILURE;
}

void read_available(int fd, std::string& output, bool& eof)
{
    char buffer[4096];
    while (!eof) {
        auto const bytes = ::read(fd, buffer, sizeof(buffer));
        if (bytes > 0) {
            output.append(buffer, static_cast<std::size_t>(bytes));
            continue;
        }
        if (bytes == 0) {
            eof = true;
            return;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        }
        eof = true;
    }
}

auto run_command(std::filesystem::path const& executable, std::vector<std::string> arguments) -> CommandResult
{
    int output_pipe[2];
    if (::pipe(output_pipe) < 0) {
        return {};
    }

    arguments.insert(arguments.begin(), executable.string());
    auto exec_arguments = std::vector<char*>{};
    exec_arguments.reserve(arguments.size() + 1U);
    for (auto& argument : arguments) {
        exec_arguments.push_back(argument.data());
    }
    exec_arguments.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    if (::posix_spawn_file_actions_init(&actions) != 0) {
        static_cast<void>(::close(output_pipe[0]));
        static_cast<void>(::close(output_pipe[1]));
        return {};
    }
    auto const actions_ready = ::posix_spawn_file_actions_addclose(&actions, output_pipe[0]) == 0 &&
                               ::posix_spawn_file_actions_adddup2(&actions, output_pipe[1], STDOUT_FILENO) == 0 &&
                               ::posix_spawn_file_actions_adddup2(&actions, output_pipe[1], STDERR_FILENO) == 0 &&
                               ::posix_spawn_file_actions_addclose(&actions, output_pipe[1]) == 0;
    auto       pid           = pid_t{};
    auto       spawned =
        actions_ready
            ? ::posix_spawn(&pid, executable.c_str(), &actions, nullptr, exec_arguments.data(), process_environment())
            : EINVAL;
    static_cast<void>(::posix_spawn_file_actions_destroy(&actions));
    if (spawned != 0) {
        static_cast<void>(::close(output_pipe[0]));
        static_cast<void>(::close(output_pipe[1]));
        return {};
    }

    static_cast<void>(::close(output_pipe[1]));
    auto const flags = ::fcntl(output_pipe[0], F_GETFL, 0);
    if (flags < 0 || ::fcntl(output_pipe[0], F_SETFL, flags | O_NONBLOCK) < 0) {
        static_cast<void>(::close(output_pipe[0]));
        ChildProcess child{pid};
        return {};
    }

    ChildProcess  command{pid};
    CommandResult result;
    auto          eof      = false;
    auto const    deadline = std::chrono::steady_clock::now() + 8s;
    while (std::chrono::steady_clock::now() < deadline) {
        read_available(output_pipe[0], result.output, eof);
        auto const status = command.try_wait();
        if (status && eof) {
            result.completed = true;
            result.status    = *status;
            break;
        }
        std::this_thread::sleep_for(10ms);
    }
    static_cast<void>(::close(output_pipe[0]));
    return result;
}

[[nodiscard]] auto successful(CommandResult const& result) -> bool
{
    return result.completed && WIFEXITED(result.status) && WEXITSTATUS(result.status) == EXIT_SUCCESS;
}

[[nodiscard]] auto failed(CommandResult const& result) -> bool
{
    return result.completed && WIFEXITED(result.status) && WEXITSTATUS(result.status) != EXIT_SUCCESS;
}

auto run_jobuctl(std::filesystem::path const&            executable,
                 std::filesystem::path const&            socket_path,
                 std::initializer_list<std::string_view> arguments) -> CommandResult
{
    auto owned_arguments = std::vector<std::string>{"--socket", socket_path.string()};
    owned_arguments.reserve(owned_arguments.size() + arguments.size());
    for (auto const argument : arguments) {
        owned_arguments.emplace_back(argument);
    }
    return run_command(executable, std::move(owned_arguments));
}

auto run_success(std::filesystem::path const&            executable,
                 std::filesystem::path const&            socket_path,
                 std::initializer_list<std::string_view> arguments) -> std::optional<std::string>
{
    auto result = run_jobuctl(executable, socket_path, arguments);
    if (!successful(result)) {
        fmt::print(stderr, "{}", result.output);
        return std::nullopt;
    }
    return std::move(result.output);
}

auto spawn_jobud(std::filesystem::path const& executable,
                 std::filesystem::path const& socket_path,
                 std::filesystem::path const& database_path,
                 std::uint32_t                http_concurrency) -> std::optional<ChildProcess>
{
    auto arguments = std::vector<std::string>{
        executable.string(),
        "--socket",
        socket_path.string(),
        "--database",
        database_path.string(),
        "--http-concurrency",
        fmt::format("{}", http_concurrency),
    };
    auto exec_arguments = std::vector<char*>{};
    exec_arguments.reserve(arguments.size() + 1U);
    for (auto& argument : arguments) {
        exec_arguments.push_back(argument.data());
    }
    exec_arguments.push_back(nullptr);

    auto pid = pid_t{};
    auto spawned =
        ::posix_spawn(&pid, executable.c_str(), nullptr, nullptr, exec_arguments.data(), process_environment());
    if (spawned != 0) {
        return std::nullopt;
    }
    return ChildProcess{pid};
}

auto wait_for_listener(ChildProcess& daemon, std::filesystem::path const& socket_path) -> bool
{
    auto const deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
        std::error_code error;
        if (std::filesystem::is_socket(socket_path, error)) {
            return true;
        }
        if (daemon.try_wait()) {
            return false;
        }
        std::this_thread::sleep_for(10ms);
    }
    return false;
}

auto rejects_usage(std::filesystem::path const& jobud,
                   std::filesystem::path const& socket_path,
                   std::filesystem::path const& database_path,
                   std::vector<std::string>     extra_arguments) -> bool
{
    auto arguments = std::vector<std::string>{"--socket", socket_path.string(), "--database", database_path.string()};
    arguments.insert(arguments.end(), extra_arguments.begin(), extra_arguments.end());
    auto result = run_command(jobud, std::move(arguments));
    return failed(result) && result.output.find("Usage: jobud") != std::string::npos &&
           !std::filesystem::exists(socket_path) && !std::filesystem::exists(database_path);
}

auto rejects_client_options(std::filesystem::path const& jobud,
                            std::filesystem::path const& socket_path,
                            std::filesystem::path const& database_path,
                            std::string_view             option,
                            std::string_view             value) -> bool
{
    auto result = run_command(jobud,
                              {
                                  "--socket",
                                  socket_path.string(),
                                  "--database",
                                  database_path.string(),
                                  std::string{option},
                                  std::string{value},
                              });
    return failed(result) && result.output.find("net.http.invalid_options") != std::string::npos &&
           !std::filesystem::exists(socket_path);
}

enum class DurableProbeResult : std::uint8_t {
    Waiting,
    Complete,
    Invalid,
};

auto column_text(sqlite3_stmt* statement, int column) -> std::string_view
{
    auto const* text = sqlite3_column_text(statement, column);
    auto const  size = sqlite3_column_bytes(statement, column);
    return text && size >= 0 ? std::string_view{reinterpret_cast<char const*>(text), static_cast<std::size_t>(size)}
                             : std::string_view{};
}

auto column_blob(sqlite3_stmt* statement, int column) -> std::string_view
{
    auto const* bytes = sqlite3_column_blob(statement, column);
    auto const  size  = sqlite3_column_bytes(statement, column);
    return bytes && size >= 0 ? std::string_view{static_cast<char const*>(bytes), static_cast<std::size_t>(size)}
                              : std::string_view{};
}

auto probe_durable_completion(sqlite3* database, std::string& reason) -> DurableProbeResult
{
    constexpr auto sql =
        "SELECT runs.state, attempts.state, attempts.outcome, attempts.result_json, output.stdout_blob, "
        "output.stderr_blob, output.stdout_truncated, output.stderr_truncated, output.capture_lost "
        "FROM jobu_runs AS runs JOIN jobu_attempts AS attempts ON attempts.run_id = runs.id "
        "LEFT JOIN jobu_attempt_output AS output ON output.run_id = attempts.run_id "
        "AND output.attempt_number = attempts.attempt_number ORDER BY runs.id, attempts.attempt_number";

    sqlite3_stmt* raw_statement{nullptr};
    auto const    prepared = sqlite3_prepare_v2(database, sql, -1, &raw_statement, nullptr);
    auto statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>{raw_statement, sqlite3_finalize};
    if (prepared == SQLITE_BUSY || prepared == SQLITE_LOCKED) {
        return DurableProbeResult::Waiting;
    }
    if (prepared != SQLITE_OK) {
        reason = sqlite3_errmsg(database);
        return DurableProbeResult::Invalid;
    }

    auto const stepped = sqlite3_step(statement.get());
    if (stepped == SQLITE_DONE || stepped == SQLITE_BUSY || stepped == SQLITE_LOCKED) {
        return DurableProbeResult::Waiting;
    }
    if (stepped != SQLITE_ROW) {
        reason = sqlite3_errmsg(database);
        return DurableProbeResult::Invalid;
    }

    auto const run_state     = column_text(statement.get(), 0);
    auto const attempt_state = column_text(statement.get(), 1);
    if (run_state != "failed" || attempt_state != "completed") {
        return DurableProbeResult::Waiting;
    }

    auto const outcome = column_text(statement.get(), 2);
    auto const result  = column_text(statement.get(), 3);
    auto const body    = column_blob(statement.get(), 4);
    auto const headers = column_blob(statement.get(), 5);
    auto const valid   = outcome == "failed" && result.find(R"("outcome":"unexpected_status")") != std::string::npos &&
                         result.find(R"("status":404)") != std::string::npos && body == "daemon-output" &&
                         headers.find("X-Stage: daemon") != std::string::npos &&
                         sqlite3_column_int(statement.get(), 6) == 0 && sqlite3_column_int(statement.get(), 7) == 0 &&
                         sqlite3_column_int(statement.get(), 8) == 0;
    if (!valid) {
        reason = "terminal attempt or output fields were unexpected";
        return DurableProbeResult::Invalid;
    }
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        reason = "more than one attempt was visible";
        return DurableProbeResult::Invalid;
    }
    return DurableProbeResult::Complete;
}

auto wait_for_durable_completion(std::filesystem::path const& database_path, std::string& reason) -> bool
{
    sqlite3*   raw_database{nullptr};
    auto const opened   = sqlite3_open_v2(database_path.c_str(), &raw_database, SQLITE_OPEN_READONLY, nullptr);
    auto       database = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>{raw_database, sqlite3_close};
    if (opened != SQLITE_OK) {
        reason = raw_database ? sqlite3_errmsg(raw_database) : "unable to allocate a SQLite connection";
        return false;
    }
    static_cast<void>(sqlite3_busy_timeout(database.get(), 100));

    auto const deadline = std::chrono::steady_clock::now() + 8s;
    while (std::chrono::steady_clock::now() < deadline) {
        auto const probe = probe_durable_completion(database.get(), reason);
        if (probe == DurableProbeResult::Complete) {
            return true;
        }
        if (probe == DurableProbeResult::Invalid) {
            return false;
        }
        // The durable state above is the correctness predicate; this delay only bounds probe frequency.
        std::this_thread::sleep_for(10ms);
    }
    reason = "durable completion did not commit before the deadline";
    return false;
}

auto bytes(std::string_view value) -> jb::core::ByteBuffer
{
    return {reinterpret_cast<std::byte const*>(value.data()),
            reinterpret_cast<std::byte const*>(value.data() + value.size())};
}

} // anonymous namespace

auto main(int argc, char* argv[]) -> int
{
    if (argc != 3) {
        return fail("expected jobud and jobuctl arguments");
    }

    jb::test::TemporaryDirectory directory;
    auto const                   jobud   = std::filesystem::path{argv[1]};
    auto const                   jobuctl = std::filesystem::path{argv[2]};

    if (!rejects_usage(jobud,
                       directory.path() / "zero.sock",
                       directory.path() / "zero.sqlite",
                       {"--http-concurrency", "0"}) ||
        !rejects_usage(jobud,
                       directory.path() / "overflow.sock",
                       directory.path() / "overflow.sqlite",
                       {"--http-concurrency", "4294967296"}) ||
        !rejects_usage(jobud,
                       directory.path() / "duplicate.sock",
                       directory.path() / "duplicate.sqlite",
                       {"--http-concurrency", "1", "--http-concurrency", "2"})) {
        return fail("jobud accepted invalid HTTP concurrency options");
    }

    if (!rejects_client_options(jobud,
                                directory.path() / "proxy.sock",
                                directory.path() / "proxy.sqlite",
                                "--http-proxy",
                                "ftp://example.test") ||
        !rejects_client_options(jobud,
                                directory.path() / "ca.sock",
                                directory.path() / "ca.sqlite",
                                "--http-ca-bundle",
                                (directory.path() / "missing-ca.pem").string())) {
        return fail("jobud did not reject invalid HTTP client options before listening");
    }

    jb::test::HttpTestServer server;
    server.enqueue_response({
        .status_code = 404,
        .reason      = "Not Found",
        .headers     = {{.name = "X-Stage", .value = "daemon"}},
        .body        = bytes("daemon-output"),
    });
    server.release_responses();

    auto const database_path = directory.path() / "jobu.sqlite";
    auto const socket_path   = directory.path() / "jobud.sock";
    auto       daemon        = spawn_jobud(jobud, socket_path, database_path, 1);
    if (!daemon) {
        return fail("unable to start jobud");
    }
    if (!wait_for_listener(*daemon, socket_path)) {
        return fail("jobud did not begin listening before the deadline");
    }
    if (!run_success(jobuctl, socket_path, {"system", "info"})) {
        return fail("jobud did not complete the readiness RPC");
    }
    if (!run_success(jobuctl, socket_path, {"queue", "create", "daemon-http"})) {
        return fail("jobuctl could not create the HTTP queue");
    }

    auto const url = server.url("/stage-5-16");
    if (!run_success(jobuctl,
                     socket_path,
                     {"job",
                      "create",
                      "--queue-name",
                      "daemon-http",
                      "--type",
                      "http",
                      "--at",
                      "2000-01-01T00:00:00.000000Z",
                      "--url",
                      url})) {
        return fail("jobuctl could not create the due HTTP job");
    }

    if (!server.wait_for_requests(1, 5s)) {
        return fail("the management mutation did not wake HTTP scheduling");
    }
    auto const requests = server.requests();
    if (requests.size() != 1U || requests.front().method != "GET" || requests.front().target != "/stage-5-16") {
        return fail("jobud sent an unexpected HTTP request");
    }

    auto durable_error = std::string{};
    if (!wait_for_durable_completion(database_path, durable_error)) {
        fmt::print(stderr, "{}\n", durable_error);
        return fail("jobud did not commit the expected HTTP completion and output");
    }
    if (daemon->try_wait()) {
        return fail("jobud exited after completing the HTTP attempt");
    }
    daemon->terminate();

    auto const cleanup_error = directory.cleanup();
    if (cleanup_error) {
        return fail("temporary directory cleanup failed");
    }
    return EXIT_SUCCESS;
}
