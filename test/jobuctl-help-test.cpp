#include "support/temporary_directory.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cerrno>
#include <chrono>
#include <csignal> // IWYU pragma: keep POSIX kill signal constants.
#include <cstddef>
#include <fcntl.h>
#include <poll.h>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {

class Pipe {
public:
    /// @throws Catch::TestFailureException when the fixture cannot create a pipe.
    Pipe() { REQUIRE(::pipe(_fds.data()) == 0); }

    ~Pipe()
    {
        close_read();
        close_write();
    }

    Pipe(Pipe const&)                    = delete;
    auto operator=(Pipe const&) -> Pipe& = delete;

    auto read_fd() const -> int { return _fds[0]; }

    auto write_fd() const -> int { return _fds[1]; }

    void close_read()
    {
        if (_fds[0] >= 0) {
            ::close(_fds[0]);
            _fds[0] = -1;
        }
    }

    void close_write()
    {
        if (_fds[1] >= 0) {
            ::close(_fds[1]);
            _fds[1] = -1;
        }
    }

private:
    std::array<int, 2> _fds{-1, -1};
};

class Child {
public:
    explicit Child(pid_t pid)
        : _pid{pid}
    {}

    ~Child()
    {
        if (_pid > 0) {
            // Only an unreaped child is signalled; its PID cannot have been reused.
            static_cast<void>(::kill(_pid, SIGKILL));
            auto status = 0;
            while (::waitpid(_pid, &status, 0) < 0 && errno == EINTR) {
            }
        }
    }

    Child(Child const&)                    = delete;
    auto operator=(Child const&) -> Child& = delete;

    /// @throws Catch::TestFailureException when the child cannot be reaped.
    auto wait() -> int
    {
        auto status = 0;
        auto result = pid_t{};
        do {
            result = ::waitpid(_pid, &status, 0);
        } while (result < 0 && errno == EINTR);
        REQUIRE(result == _pid);
        _pid = -1;
        REQUIRE(WIFEXITED(status));
        return WEXITSTATUS(status);
    }

private:
    pid_t _pid;
};

struct Output {
    int         code;
    std::string out;
    std::string err;
};

/// @throws Catch::TestFailureException when pipe reading fails.
auto drain(int fd, std::string& text) -> bool
{
    auto buffer = std::array<char, 4096>{};
    for (;;) {
        auto const count = ::read(fd, buffer.data(), buffer.size());
        if (count > 0) {
            text.append(buffer.data(), static_cast<std::size_t>(count));
        }
        else if (count == 0) {
            return true;
        }
        else if (errno != EINTR) {
            REQUIRE((errno == EAGAIN || errno == EWOULDBLOCK));
            return false;
        }
    }
}

/// Run with either a closed stdin or an empty pipe whose writer stays open until the child exits.
/// The latter makes an accidental input read block; poll/child alarms bound fixture failures without sleeps.
/// @throws Catch::TestFailureException when setup, the watchdog, or child completion fails.
auto run(std::vector<std::string> arguments, bool blocked_stdin = true) -> Output
{
    arguments.insert(arguments.begin(), JOBUCTL_EXECUTABLE);
    auto argv = std::vector<char*>{};
    for (auto& argument : arguments) {
        argv.push_back(argument.data());
    }
    argv.push_back(nullptr);

    Pipe       input;
    Pipe       output;
    Pipe       error;
    auto const pid = ::fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        if (::dup2(output.write_fd(), STDOUT_FILENO) < 0 || ::dup2(error.write_fd(), STDERR_FILENO) < 0 ||
            (blocked_stdin && ::dup2(input.read_fd(), STDIN_FILENO) < 0)) {
            ::_exit(126);
        }
        if (!blocked_stdin) {
            ::close(STDIN_FILENO);
        }
        input.close_read();
        input.close_write();
        output.close_read();
        output.close_write();
        error.close_read();
        error.close_write();
        ::alarm(5);
        ::execv(argv.front(), argv.data());
        ::_exit(127);
    }
    Child child{pid};
    input.close_read();
    output.close_write();
    error.close_write();
    REQUIRE(::fcntl(output.read_fd(), F_SETFL, O_NONBLOCK) == 0);
    REQUIRE(::fcntl(error.read_fd(), F_SETFL, O_NONBLOCK) == 0);

    auto result      = Output{};
    auto descriptors = std::array{
        pollfd{.fd = output.read_fd(), .events = POLLIN, .revents = 0},
        pollfd{.fd = error.read_fd(),  .events = POLLIN, .revents = 0},
    };
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
    while (descriptors[0].fd >= 0 || descriptors[1].fd >= 0) {
        auto const remaining =
            std::chrono::ceil<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        REQUIRE(remaining.count() > 0);
        auto const ready = ::poll(descriptors.data(), descriptors.size(), static_cast<int>(remaining.count()));
        if (ready < 0 && errno == EINTR) {
            continue;
        }
        REQUIRE(ready > 0);
        for (auto index = std::size_t{0}; index < descriptors.size(); ++index) {
            auto& descriptor = descriptors[index];
            if (descriptor.fd >= 0 && descriptor.revents != 0 &&
                drain(descriptor.fd, index == 0 ? result.out : result.err)) {
                descriptor.fd = -1;
            }
        }
    }
    result.code = child.wait();
    return result;
}

} // namespace

TEST_CASE("jobuctl executable prints local help with blocked or closed stdin", "[jobuctl][help]")
{
    for (auto const blocked : {false, true}) {
        for (auto const& arguments : std::vector<std::vector<std::string>>{
                 {},
                 {"--help"},
                 {"-h"},
                 {"queue"},
                 {"queue", "--help"},
                 {"queue", "add", "--help"},
                 {"help", "queue", "add"},
                 {"job", "create", "--help"},
                 {"queue", "get", "--id", "not-a-uuid", "--socket", "/nonexistent/jobu.sock", "--help"}
        }) {
            CAPTURE(blocked, arguments);
            auto result = run(arguments, blocked);
            CHECK(result.code == 0);
            CHECK(result.out.starts_with("Usage:\n"));
            CHECK(result.err.empty());
        }
        auto version = run({"--version"}, blocked);
        CHECK(version.code == 0);
        CHECK(version.out.starts_with("jobuctl "));
        CHECK(version.err.empty());
    }
    auto alias = run({"queue", "add", "--help"});
    CHECK(alias.out.find("queue create NAME") != std::string::npos);
    CHECK(alias.out.find("Alias: queue add") != std::string::npos);
}

TEST_CASE("jobuctl executable rejects invalid help syntax with contextual stderr", "[jobuctl][help]")
{
    for (auto const& arguments : std::vector<std::vector<std::string>>{
             {"unknown", "--help"},
             {"queue", "unknown", "--help"},
             {"queue", "get", "--name", "--help"},
             {"queue", "list", "--unknown", "--help"},
             {"queue", "list", "--", "--help"},
             {"queue", "list"},
             {"job", "run-now", "--help"},
             {"secret", "set", "--stdin", "--help"}
    }) {
        CAPTURE(arguments);
        auto result = run(arguments);
        CHECK(result.code == 2);
        CHECK(result.out.empty());
        CHECK(result.err.find("Usage:\n") != std::string::npos);
    }
    auto group = run({"queue", "unknown", "--help"});
    CHECK(group.err.find("queue COMMAND") != std::string::npos);
    auto leaf = run({"queue", "get", "--name", "--help"});
    CHECK(leaf.err.find("queue get") != std::string::npos);
}

TEST_CASE("jobuctl local selection does not open supplied file paths", "[jobuctl][help]")
{
    jb::test::TemporaryDirectory directory;
    auto const                   fifo = directory.path() / "unread-input";
    REQUIRE(::mkfifo(fifo.c_str(), 0600) == 0);

    // Opening this FIFO for input would block: there is deliberately no writer.
    auto help = run({"queue", "add", "--socket", fifo.string(), "--help"});
    CHECK(help.code == 0);
    CHECK(help.err.empty());

    // Request-file support is not available yet; unknown options still fail without touching their values.
    for (auto const& path : {fifo, directory.path() / "missing-input"}) {
        auto invalid = run({"queue", "create", "--request-file", path.string(), "--help"});
        CHECK(invalid.code == 2);
        CHECK(invalid.out.empty());
        CHECK(invalid.err.find("unknown option") != std::string::npos);
    }
}
