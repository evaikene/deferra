#include "support/temporary_directory.hpp"

#include "uuid.hpp"

#include <fmt/format.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

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
    fmt::print(stderr, "jobuctl-queue-management-test: {}\n", message);
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

auto run_jobuctl(std::filesystem::path const&            executable,
                 std::filesystem::path const&            socket_path,
                 std::initializer_list<std::string_view> arguments) -> CommandResult
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

        auto owned_arguments = std::vector<std::string>{
            executable.string(),
            "--socket",
            socket_path.string(),
        };
        owned_arguments.reserve(owned_arguments.size() + arguments.size());
        for (auto const argument : arguments) {
            owned_arguments.emplace_back(argument);
        }

        auto exec_arguments = std::vector<char*>{};
        exec_arguments.reserve(owned_arguments.size() + 1U);
        for (auto& argument : owned_arguments) {
            exec_arguments.push_back(argument.data());
        }
        exec_arguments.push_back(nullptr);
        ::execv(executable.c_str(), exec_arguments.data());
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

auto successful(CommandResult const& result) -> bool
{
    return result.completed && WIFEXITED(result.status) && WEXITSTATUS(result.status) == EXIT_SUCCESS;
}

auto failed(CommandResult const& result) -> bool
{
    return result.completed && WIFEXITED(result.status) && WEXITSTATUS(result.status) != EXIT_SUCCESS;
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

auto run_failure(std::filesystem::path const&            executable,
                 std::filesystem::path const&            socket_path,
                 std::initializer_list<std::string_view> arguments) -> std::optional<std::string>
{
    auto result = run_jobuctl(executable, socket_path, arguments);
    if (!failed(result)) {
        fmt::print(stderr, "{}", result.output);
        return std::nullopt;
    }
    return std::move(result.output);
}

auto queue_id_from_summary(std::string_view output) -> std::optional<std::string>
{
    constexpr auto prefix = std::string_view{"Queue "};
    if (!output.starts_with(prefix)) {
        return std::nullopt;
    }
    auto const separator = output.find(':', prefix.size());
    if (separator == std::string_view::npos) {
        return std::nullopt;
    }
    auto parsed = jb::core::Uuid::parse(output.substr(prefix.size(), separator - prefix.size()));
    if (!parsed) {
        return std::nullopt;
    }
    return parsed->to_string();
}

auto queue_summary(std::string_view id,
                   std::string_view name,
                   std::string_view state,
                   std::uint32_t    weight,
                   std::uint32_t    concurrency,
                   std::string_view recovery) -> std::string
{
    return fmt::format("Queue {}: name={}, state={}, weight={}, concurrency_limit={}, recovery_policy={}\n",
                       id,
                       name,
                       state,
                       weight,
                       concurrency,
                       recovery);
}

auto rejects_before_connect(std::filesystem::path const&            executable,
                            std::filesystem::path const&            socket_path,
                            std::initializer_list<std::string_view> arguments) -> bool
{
    auto output = run_failure(executable, socket_path, arguments);
    return output && output->find("Usage:\n") != std::string::npos;
}

} // anonymous namespace

auto main(int argc, char* argv[]) -> int
{
    if (argc != 3) {
        return fail("expected jobud and jobuctl arguments");
    }

    jb::test::TemporaryDirectory directory;
    auto const                   database_path = directory.path() / "jobu.sqlite";
    auto const                   socket_path   = directory.path() / "jobud-fresh.sock";

    if (!rejects_before_connect(argv[2], socket_path, {"queue", "get", "--id", "bad", "--name", "default"}) ||
        !rejects_before_connect(argv[2], socket_path, {"queue", "list", "--limit", "0"}) ||
        !rejects_before_connect(argv[2],
                                socket_path,
                                {"queue", "create", "default", "--weight", "1", "--weight", "2"}) ||
        !rejects_before_connect(argv[2], socket_path, {"queue", "update", "--name", "default"}) ||
        !rejects_before_connect(argv[2], socket_path, {"queue", "list", "--unknown"}) ||
        !rejects_before_connect(argv[2], socket_path, {"queue", "create", "default", "--weight"})) {
        return fail("jobuctl accepted invalid queue command arguments");
    }

    auto daemon = spawn_jobud(argv[1], socket_path, database_path);
    if (!daemon) {
        return fail("unable to start jobud");
    }
    if (!wait_for_listener(*daemon, socket_path)) {
        return fail("jobud did not begin listening before the deadline");
    }

    auto created = run_success(argv[2],
                               socket_path,
                               {"queue",
                                "create",
                                "default",
                                "--weight",
                                "2",
                                "--concurrency-limit",
                                "3",
                                "--recovery-policy",
                                "retry_interrupted",
                                "--idempotency-key",
                                "create-default"});
    if (!created) {
        return fail("jobuctl could not create a queue");
    }
    auto const queue_id = queue_id_from_summary(*created);
    if (!queue_id) {
        fmt::print(stderr, "{}", *created);
        return fail("queue create did not print a canonical queue UUID");
    }
    auto const active_summary = queue_summary(*queue_id, "default", "active", 2, 3, "retry_interrupted");
    if (*created != active_summary) {
        fmt::print(stderr, "{}", *created);
        return fail("queue create printed an unexpected summary");
    }

    auto replayed = run_success(argv[2],
                                socket_path,
                                {"queue",
                                 "create",
                                 "default",
                                 "--weight",
                                 "2",
                                 "--concurrency-limit",
                                 "3",
                                 "--recovery-policy",
                                 "retry_interrupted",
                                 "--idempotency-key",
                                 "create-default"});
    if (!replayed || *replayed != active_summary) {
        return fail("queue create did not replay the original durable result");
    }

    auto conflict =
        run_failure(argv[2], socket_path, {"queue", "create", "different", "--idempotency-key", "create-default"});
    if (!conflict || conflict->find("(jobu.idempotency.conflict)") == std::string::npos) {
        fmt::print(stderr, "{}", conflict.value_or(""));
        return fail("remote application failure did not expose its stable domain code");
    }

    auto by_name = run_success(argv[2], socket_path, {"queue", "get", "--name", "default"});
    auto by_id   = run_success(argv[2], socket_path, {"queue", "get", "--id", *queue_id});
    auto listed  = run_success(argv[2], socket_path, {"queue", "list"});
    if (!by_name || *by_name != active_summary || !by_id || *by_id != active_summary || !listed ||
        *listed != active_summary) {
        return fail("queue get/list did not return the created queue");
    }

    auto other = run_success(argv[2], socket_path, {"queue", "create", "other"});
    if (!other) {
        return fail("jobuctl could not create a second queue");
    }
    auto const other_id = queue_id_from_summary(*other);
    if (!other_id) {
        return fail("second queue create did not print a canonical UUID");
    }
    auto const other_summary = queue_summary(*other_id, "other", "active", 1, 1, "fail_interrupted");
    if (*other != other_summary) {
        return fail("second queue create printed an unexpected summary");
    }

    auto first_page = run_success(argv[2], socket_path, {"queue", "list", "--limit", "1"});
    if (!first_page || *first_page != active_summary + fmt::format("Next after: {}\n", *queue_id)) {
        fmt::print(stderr, "{}", first_page.value_or(""));
        return fail("bounded queue list did not print its continuation cursor");
    }
    auto second_page = run_success(argv[2], socket_path, {"queue", "list", "--after", *queue_id});
    if (!second_page || *second_page != other_summary) {
        fmt::print(stderr, "{}", second_page.value_or(""));
        return fail("queue list did not continue after the supplied UUID");
    }

    auto updated = run_success(
        argv[2],
        socket_path,
        {"queue", "update", "--name", "default", "--new-name", "renamed", "--weight", "4", "--concurrency-limit", "5"});
    auto const updated_summary = queue_summary(*queue_id, "renamed", "active", 4, 5, "retry_interrupted");
    if (!updated || *updated != updated_summary) {
        return fail("queue update did not print the committed replacement fields");
    }

    auto missing = run_failure(argv[2], socket_path, {"queue", "get", "--name", "default"});
    if (!missing || missing->find("(jobu.queue.not_found)") == std::string::npos) {
        return fail("renamed queue lookup did not surface the stable not-found code");
    }

    auto       suspended         = run_success(argv[2], socket_path, {"queue", "suspend", "--id", *queue_id});
    auto const suspended_summary = queue_summary(*queue_id, "renamed", "suspended", 4, 5, "retry_interrupted");
    if (!suspended || *suspended != suspended_summary) {
        return fail("queue suspend did not print the durable suspended state");
    }
    auto resumed = run_success(argv[2], socket_path, {"queue", "resume", "--name", "renamed"});
    if (!resumed || *resumed != updated_summary) {
        return fail("queue resume did not print the durable active state");
    }
    suspended = run_success(argv[2], socket_path, {"queue", "suspend", "--name", "renamed"});
    if (!suspended || *suspended != suspended_summary) {
        return fail("queue did not suspend again before deletion");
    }

    auto deleted = run_success(argv[2], socket_path, {"queue", "delete", "--name", "renamed"});
    if (!deleted || *deleted != "Deleted queue name=renamed\n") {
        return fail("queue delete did not acknowledge the supplied selector");
    }

    daemon->terminate();

    auto const reopen_socket = directory.path() / "jobud-reopen.sock";
    auto       reopened      = spawn_jobud(argv[1], reopen_socket, database_path);
    if (!reopened) {
        return fail("unable to restart jobud");
    }
    if (!wait_for_listener(*reopened, reopen_socket)) {
        return fail("jobud did not reopen the durable queue database before the deadline");
    }

    auto const deleted_summary = queue_summary(*queue_id, "renamed", "deleted", 4, 5, "retry_interrupted");
    auto       after_restart   = run_success(argv[2], reopen_socket, {"queue", "list", "--include-deleted"});
    if (!after_restart || after_restart->find(deleted_summary) == std::string::npos ||
        after_restart->find(other_summary) == std::string::npos) {
        fmt::print(stderr, "{}", after_restart.value_or(""));
        return fail("queue states did not survive daemon restart");
    }

    auto recreated = run_success(argv[2], reopen_socket, {"queue", "create", "renamed"});
    if (!recreated) {
        return fail("jobuctl could not reuse a soft-deleted queue name");
    }
    auto const recreated_id = queue_id_from_summary(*recreated);
    if (!recreated_id || *recreated_id == *queue_id ||
        *recreated != queue_summary(*recreated_id, "renamed", "active", 1, 1, "fail_interrupted")) {
        return fail("queue name reuse did not create a distinct active queue");
    }

    reopened->terminate();
    auto const cleanup_error = directory.cleanup();
    if (cleanup_error) {
        return fail("temporary directory cleanup failed");
    }
    return EXIT_SUCCESS;
}
