#include "support/temporary_directory.hpp"

#include "database.hpp"
#include "query.hpp"
#include "sqlite/sqlite_driver.hpp"

#include <fmt/format.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace {

using namespace std::chrono_literals;

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
    fmt::print(stderr, "jobuctl-system-info-test: {}\n", message);
    return EXIT_FAILURE;
}

auto spawn_jobud(std::filesystem::path const& executable,
                 std::filesystem::path const& socket_path,
                 std::filesystem::path const& database_path) -> std::optional<ChildProcess>
{
    auto const pid = ::fork();
    if (pid < 0) {
        return std::nullopt;
    }
    if (pid == 0) {
        ::execl(executable.c_str(),
                executable.c_str(),
                "--socket",
                socket_path.c_str(),
                "--database",
                database_path.c_str(),
                static_cast<char*>(nullptr));
        ::_exit(127);
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

auto wait_for_exit(ChildProcess& child) -> std::optional<int>
{
    auto const deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (auto status = child.try_wait()) {
            return status;
        }
        std::this_thread::sleep_for(10ms);
    }
    return std::nullopt;
}

auto mark_schema_newer(std::filesystem::path const& database_path) -> bool
{
    auto database = jb::db::Database{
        std::make_unique<jb::db::sqlite::Driver>(jb::db::sqlite::Options{.database_file = database_path})};
    if (!database.open()) {
        return false;
    }

    auto updated  = false;
    auto finished = false;
    {
        jb::db::Query query{database};
        auto          executed = query.exec("UPDATE jobu_schema SET version = 2 WHERE singleton = 1");
        updated                = executed && query.num_rows_affected() == 1;
        finished               = static_cast<bool>(query.finish());
    }
    auto const closed = database.close();
    return updated && finished && closed;
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

auto run_jobuctl(std::filesystem::path const& executable, std::filesystem::path const& socket_path) -> CommandResult
{
    int output_pipe[2];
    if (::pipe(output_pipe) < 0) {
        return {};
    }

    auto const pid = ::fork();
    if (pid < 0) {
        static_cast<void>(::close(output_pipe[0]));
        static_cast<void>(::close(output_pipe[1]));
        return {};
    }
    if (pid == 0) {
        static_cast<void>(::close(output_pipe[0]));
        if (::dup2(output_pipe[1], STDOUT_FILENO) < 0 || ::dup2(output_pipe[1], STDERR_FILENO) < 0) {
            ::_exit(127);
        }
        static_cast<void>(::close(output_pipe[1]));
        ::execl(executable.c_str(),
                executable.c_str(),
                "--socket",
                socket_path.c_str(),
                "system",
                "info",
                static_cast<char*>(nullptr));
        ::_exit(127);
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

} // anonymous namespace

auto main(int argc, char* argv[]) -> int
{
    if (argc != 4) {
        return fail("expected jobud, jobuctl, and project version arguments");
    }

    jb::test::TemporaryDirectory directory;
    auto const                   database_path = directory.path() / "jobu.sqlite";
    auto const                   socket_path   = directory.path() / "jobud-fresh.sock";
    auto                         daemon        = spawn_jobud(argv[1], socket_path, database_path);
    if (!daemon) {
        return fail("unable to start jobud");
    }
    if (!wait_for_listener(*daemon, socket_path)) {
        return fail("jobud did not begin listening before the deadline");
    }
    if (!std::filesystem::is_regular_file(database_path)) {
        return fail("jobud did not create the database file");
    }

    auto const command = run_jobuctl(argv[2], socket_path);
    if (!command.completed) {
        return fail("jobuctl did not finish before the deadline");
    }
    if (!WIFEXITED(command.status) || WEXITSTATUS(command.status) != EXIT_SUCCESS) {
        fmt::print(stderr, "{}", command.output);
        return fail("jobuctl exited unsuccessfully");
    }
    if (command.output.find("Daemon version: " + std::string{argv[3]}) == std::string::npos ||
        command.output.find("API version: 1.0") == std::string::npos ||
        command.output.find("system.info") == std::string::npos) {
        fmt::print(stderr, "{}", command.output);
        return fail("jobuctl output did not contain the expected system information");
    }
    if (daemon->try_wait()) {
        return fail("jobud exited while serving system.info");
    }

    daemon->terminate();

    auto const reopen_socket = directory.path() / "jobud-reopen.sock";
    auto       reopened      = spawn_jobud(argv[1], reopen_socket, database_path);
    if (!reopened) {
        return fail("unable to restart jobud");
    }
    if (!wait_for_listener(*reopened, reopen_socket)) {
        return fail("jobud did not reopen the database before the deadline");
    }

    auto const reopened_command = run_jobuctl(argv[2], reopen_socket);
    if (!reopened_command.completed || !WIFEXITED(reopened_command.status) ||
        WEXITSTATUS(reopened_command.status) != EXIT_SUCCESS) {
        fmt::print(stderr, "{}", reopened_command.output);
        return fail("jobuctl could not query the reopened daemon");
    }
    reopened->terminate();

    if (!mark_schema_newer(database_path)) {
        return fail("unable to mark the database schema as newer");
    }

    auto const newer_socket = directory.path() / "jobud-newer.sock";
    auto       newer        = spawn_jobud(argv[1], newer_socket, database_path);
    if (!newer) {
        return fail("unable to start jobud against the newer schema");
    }
    auto const newer_status = wait_for_exit(*newer);
    if (!newer_status) {
        return fail("jobud did not reject the newer schema before the deadline");
    }
    if (!WIFEXITED(*newer_status) || WEXITSTATUS(*newer_status) == EXIT_SUCCESS) {
        return fail("jobud did not exit unsuccessfully for the newer schema");
    }
    if (std::filesystem::exists(newer_socket)) {
        return fail("jobud created a socket before rejecting the newer schema");
    }

    auto const cleanup_error = directory.cleanup();
    if (cleanup_error) {
        return fail("temporary directory cleanup failed");
    }
    return EXIT_SUCCESS;
}
