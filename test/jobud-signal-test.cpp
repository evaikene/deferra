#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cerrno>
#include <chrono>
#include <csignal> // IWYU pragma: keep POSIX signal constants and kill.
#include <string>

#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

/// Parent-owned watchdog and child reaping. No test changes the runner's signal dispositions.
class Child final {
public:

    Child() = default;

    ~Child()
    {
        if (_pid > 0) {
            // An unreaped child retains its PID even after exit, so this cannot target a reused PID.
            ::kill(_pid, SIGKILL);
            while (::waitpid(_pid, nullptr, 0) < 0 && errno == EINTR) {
            }
        }
        for (auto const fd : {_channel[0], _channel[1], _pidfd}) {
            if (fd >= 0) {
                ::close(fd);
            }
        }
    }

    Child(Child const&)                    = delete;
    auto operator=(Child const&) -> Child& = delete;

    /// @throws Catch::TestFailureException if subprocess setup fails.
    void start(std::string scenario)
    {
        REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, _channel.data()) == 0);
        posix_spawn_file_actions_t actions;
        REQUIRE(::posix_spawn_file_actions_init(&actions) == 0);

        struct ActionsGuard {
            posix_spawn_file_actions_t* actions;

            ~ActionsGuard() { ::posix_spawn_file_actions_destroy(actions); }
        } guard{&actions};

        REQUIRE(::posix_spawn_file_actions_adddup2(&actions, _channel[1], STDIN_FILENO) == 0);
        REQUIRE(::posix_spawn_file_actions_adddup2(&actions, _channel[1], STDOUT_FILENO) == 0);
        REQUIRE(::posix_spawn_file_actions_adddup2(&actions, _channel[1], STDERR_FILENO) == 0);

        std::string          executable{JOBUD_SIGNAL_TEST_HELPER};
        std::string          reporter_option{"--reporter"};
        std::string          reporter{"compact"};
        std::array<char*, 5> arguments{executable.data(),
                                       scenario.data(),
                                       reporter_option.data(),
                                       reporter.data(),
                                       nullptr};
        REQUIRE(::posix_spawn(&_pid, executable.c_str(), &actions, nullptr, arguments.data(), ::environ) == 0);
        ::close(_channel[1]);
        _channel[1] = -1;
        _pidfd      = static_cast<int>(::syscall(SYS_pidfd_open, _pid, 0));
        REQUIRE(_pidfd >= 0);
        REQUIRE(::fcntl(_channel[0], F_SETFL, O_NONBLOCK) == 0);
    }

    /// @throws Catch::TestFailureException if readiness is absent or the child fails early.
    void await_ready()
    {
        auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
        while (_output.find("!relay-ready!\n") == std::string::npos) {
            auto const remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now())
                    .count();
            INFO(_output);
            REQUIRE(remaining > 0);
            pollfd     descriptor{.fd = _channel[0], .events = POLLIN, .revents = 0};
            auto const ready = ::poll(&descriptor, 1, static_cast<int>(remaining));
            if (ready < 0 && errno == EINTR) {
                continue;
            }
            REQUIRE(ready > 0);
            REQUIRE(read_output());
        }
    }

    /// @throws Catch::TestFailureException if a live, unreaped child cannot be signalled.
    void signal(int value) const { REQUIRE(::kill(_pid, value) == 0); }

    /// @throws Catch::TestFailureException if startup cannot be released.
    void release() const { REQUIRE(::send(_channel[0], "G", 1, MSG_NOSIGNAL) == 1); }

    /// @throws Catch::TestFailureException if the child hangs, receives a fatal signal, or fails checks.
    void require_success()
    {
        // Waiting on pidfd bounds exit itself, including cleanup after the last helper assertion.
        pollfd descriptor{.fd = _pidfd, .events = POLLIN, .revents = 0};
        int    ready;
        do {
            ready = ::poll(&descriptor, 1, 10000);
        } while (ready < 0 && errno == EINTR);
        while (read_output()) {
        }
        INFO(_output);
        REQUIRE(ready == 1);
        int        status{0};
        auto const waited = ::waitpid(_pid, &status, 0);
        REQUIRE(waited == _pid);
        _pid = -1;
        REQUIRE(WIFEXITED(status));
        REQUIRE(WEXITSTATUS(status) == 0);
    }

private:

    auto read_output() -> bool
    {
        std::array<char, 4096> bytes{};
        auto const             count = ::read(_channel[0], bytes.data(), bytes.size());
        if (count <= 0) {
            return false;
        }
        _output.append(bytes.data(), static_cast<std::size_t>(count));
        return true;
    }

    pid_t              _pid{-1};
    int                _pidfd{-1};
    std::array<int, 2> _channel{-1, -1};
    std::string        _output;
};

} // namespace

TEST_CASE("Daemon signal relay observes SIGTERM and SIGINT before event-loop construction")
{
    int signal{SIGTERM};
    SECTION("SIGTERM")
    {}
    SECTION("SIGINT")
    {
        signal = SIGINT;
    }
    Child child;
    child.start("startup");
    child.await_ready();
    child.signal(signal);
    child.release();
    child.require_success();
}

TEST_CASE("Daemon signal relay notifies the running event loop once")
{
    int signal{SIGTERM};
    SECTION("SIGTERM")
    {}
    SECTION("SIGINT")
    {
        signal = SIGINT;
    }
    Child child;
    child.start("loop");
    child.await_ready();
    child.signal(signal);
    child.require_success();
}

TEST_CASE("Daemon signal relay coalesces repeated signals with a saturated pipe")
{
    Child child;
    child.start("coalescing");
    child.await_ready();
    for (int index = 0; index < 64; ++index) {
        child.signal(index % 2 == 0 ? SIGTERM : SIGINT);
    }
    child.release();
    child.require_success();
}

TEST_CASE("Daemon signal relay restores process state across setup and teardown boundaries")
{
    std::string scenario;
    SECTION("setup rollback")
    {
        scenario = "setup";
    }
    SECTION("cleanup retry")
    {
        scenario = "cleanup";
    }
    SECTION("failed watch removal and retained callback")
    {
        scenario = "watch lifetime";
    }
    SECTION("worker delivery after event-loop teardown")
    {
        scenario = "worker lifetime";
    }
    SECTION("concurrent worker delivery")
    {
        scenario = "concurrent workers";
    }
    SECTION("exec inheritance and SIGCHLD preservation")
    {
        scenario = "descriptor inheritance";
    }
    Child child;
    child.start(scenario);
    child.require_success();
}
