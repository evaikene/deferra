#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cerrno>
#include <chrono>
#include <csignal> // IWYU pragma: keep POSIX signal constants and kill.
#include <cstring>
#include <functional>
#include <optional>
#include <string>

#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

using Clock                     = std::chrono::steady_clock;
constexpr auto kWatchdogTimeout = std::chrono::seconds{10};

auto remaining_ms(Clock::time_point deadline, Clock::time_point now) -> int
{
    if (now >= deadline) {
        return 0;
    }
    return static_cast<int>(std::chrono::ceil<std::chrono::milliseconds>(deadline - now).count());
}

/// Test-only boundaries let retry/deadline cases be exercised without scheduler timing or sleeps.
struct ReleaseOperations {
    std::function<ssize_t(int)>  send          = [](int fd) { return ::send(fd, "G", 1, MSG_NOSIGNAL); };
    std::function<int(int, int)> wait_writable = [](int fd, int timeout) {
        pollfd descriptor{.fd = fd, .events = POLLOUT, .revents = 0};
        return ::poll(&descriptor, 1, timeout);
    };
    std::function<Clock::time_point()> now = [] { return Clock::now(); };
};

/// Returns zero on delivery, otherwise the original syscall errno or a watchdog timeout.
auto send_release(int fd, ReleaseOperations const& operations) -> int
{
    auto const deadline = operations.now() + kWatchdogTimeout;
    while (remaining_ms(deadline, operations.now()) > 0) {
        auto const sent = operations.send(fd);
        if (sent == 1) {
            return 0;
        }
        auto const error = sent < 0 ? errno : EIO;
        if (error == EINTR) {
            continue;
        }
        if (error != EAGAIN && error != EWOULDBLOCK) {
            return error;
        }

        // A nonblocking control socket may need another writable opportunity. Both send and poll
        // interruptions share the original deadline; repeated signals cannot extend the watchdog.
        auto const timeout = remaining_ms(deadline, operations.now());
        if (timeout == 0) {
            return ETIMEDOUT;
        }
        auto const ready = operations.wait_writable(fd, timeout);
        if (ready == 0) {
            return ETIMEDOUT;
        }
        if (ready < 0 && errno != EINTR) {
            return errno;
        }
    }
    return ETIMEDOUT;
}

struct ChildExit {
    std::optional<int> status;
    bool               watchdog_killed{false};
    int                observation_error{0};
};

struct ReleaseResult {
    int         error{0};
    std::string diagnostic;
};

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
    void release()
    {
        auto const result = try_release();
        INFO(result.diagnostic);
        REQUIRE(result.error == 0);
    }

    /// On failure, observe/reap the child and retain its output before the caller asserts.
    auto try_release(ReleaseOperations const&  operations   = {},
                     std::chrono::milliseconds exit_timeout = kWatchdogTimeout) -> ReleaseResult
    {
        auto const error = send_release(_channel[0], operations);
        if (error == 0) {
            return {};
        }
        auto const exit = collect_exit(exit_timeout);
        return {.error      = error,
                .diagnostic = "release errno=" + std::to_string(error) + " (" + std::strerror(error) + "); " +
                              describe_exit(exit) + "\nchild output:\n" + _output};
    }

    /// Collects once, draining output while observing exit so a full diagnostic socket cannot stall the child.
    auto collect_exit(std::chrono::milliseconds timeout = kWatchdogTimeout) -> ChildExit
    {
        if (_exit) {
            return *_exit;
        }
        ChildExit  result;
        auto const deadline = Clock::now() + timeout;
        bool       output_open{true};
        for (;;) {
            std::array<pollfd, 2> descriptors{
                {{.fd = _pidfd, .events = POLLIN, .revents = 0},
                 {.fd = output_open ? _channel[0] : -1, .events = POLLIN, .revents = 0}}
            };
            auto const remaining = remaining_ms(deadline, Clock::now());
            auto const ready     = ::poll(descriptors.data(), descriptors.size(), remaining);
            if (ready < 0 && errno == EINTR && remaining > 0) {
                continue;
            }
            if (ready < 0) {
                result.observation_error = errno;
                break;
            }
            if (descriptors[1].revents != 0) {
                output_open = read_output();
            }
            if ((descriptors[0].revents & POLLIN) != 0) {
                break;
            }
            if (ready == 0 || remaining == 0) {
                result.watchdog_killed = true;
                break;
            }
        }

        // Capture a natural exit first. Only an expired watchdog or failed observation kills the
        // still-owned child; the report keeps that intervention separate from the observed status.
        if (result.watchdog_killed || result.observation_error != 0) {
            ::kill(_pid, SIGKILL);
        }
        int   status{0};
        pid_t waited;
        do {
            waited = ::waitpid(_pid, &status, 0);
        } while (waited < 0 && errno == EINTR);
        if (waited == _pid) {
            result.status = status;
            _pid          = -1;
        }
        else {
            result.observation_error = errno;
        }
        while (read_output()) {
        }
        _exit = result;
        return result;
    }

    /// @throws Catch::TestFailureException if the child hangs, receives a fatal signal, or fails checks.
    void require_success()
    {
        auto const exit = collect_exit();
        INFO(describe_exit(exit));
        INFO(_output);
        REQUIRE_FALSE(exit.watchdog_killed);
        REQUIRE(exit.observation_error == 0);
        REQUIRE(exit.status);
        REQUIRE(WIFEXITED(*exit.status));
        REQUIRE(WEXITSTATUS(*exit.status) == 0);
    }

private:

    static auto describe_exit(ChildExit const& exit) -> std::string
    {
        std::string description = exit.watchdog_killed ? "watchdog sent SIGKILL; " : "";
        if (exit.observation_error != 0) {
            description += "child observation errno=" + std::to_string(exit.observation_error) + "; ";
        }
        if (!exit.status) {
            return description + "child status unavailable";
        }
        if (WIFEXITED(*exit.status)) {
            return description + "child exit=" + std::to_string(WEXITSTATUS(*exit.status));
        }
        if (WIFSIGNALED(*exit.status)) {
            return description + "child signal=" + std::to_string(WTERMSIG(*exit.status));
        }
        return description + "child wait status=" + std::to_string(*exit.status);
    }

    auto read_output() -> bool
    {
        std::array<char, 4096> bytes{};
        ssize_t                count;
        do {
            count = ::read(_channel[0], bytes.data(), bytes.size());
        } while (count < 0 && errno == EINTR);
        if (count <= 0) {
            return false;
        }
        _output.append(bytes.data(), static_cast<std::size_t>(count));
        return true;
    }

    pid_t                    _pid{-1};
    int                      _pidfd{-1};
    std::array<int, 2>       _channel{-1, -1};
    std::string              _output;
    std::optional<ChildExit> _exit;
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

TEST_CASE("Signal test release retries interrupted sends and writable waits")
{
    Child child;
    child.start("startup");
    child.await_ready();
    child.signal(SIGTERM);

    ReleaseOperations operations;
    auto const        native_send = operations.send;
    auto const        native_wait = operations.wait_writable;
    std::array        errors{EINTR, EAGAIN, EAGAIN};
    std::size_t       sends{0};
    int               waits{0};
    operations.send = [&](int fd) -> ssize_t {
        auto const attempt = sends++;
        if (attempt < errors.size()) {
            errno = errors[attempt];
            return -1;
        }
        return native_send(fd);
    };
    operations.wait_writable = [&](int fd, int timeout) {
        if (++waits == 1) {
            errno = EINTR;
            return -1;
        }
        return native_wait(fd, timeout);
    };

    // Inject only transient parent-side outcomes; the last send releases the real signalled child.
    auto const result = child.try_release(operations);
    INFO(result.diagnostic);
    REQUIRE(result.error == 0);
    REQUIRE(sends == 4);
    REQUIRE(waits == 2);
    child.require_success();
}

TEST_CASE("Signal test release keeps one deadline across interruptions")
{
    ReleaseOperations operations;
    Clock::time_point now{};
    operations.now = [&] { return now; };
    int sends{0};
    int waits{0};
    int send_error{EINTR};
    SECTION("send interrupted until the deadline")
    {}
    SECTION("writable wait interrupted at the deadline")
    {
        send_error = EAGAIN;
    }

    operations.send = [&](int) -> ssize_t {
        ++sends;
        if (send_error == EINTR) {
            now += kWatchdogTimeout;
        }
        errno = send_error;
        return -1;
    };
    operations.wait_writable = [&](int, int timeout) {
        CHECK(timeout == 10000);
        ++waits;
        now   += kWatchdogTimeout;
        errno  = EINTR;
        return -1;
    };
    REQUIRE(send_release(-1, operations) == ETIMEDOUT);
    REQUIRE(sends == 1);
    REQUIRE(waits == (send_error == EAGAIN ? 1 : 0));
}

TEST_CASE("Signal test release reports reaped peer exits and buffered output")
{
    Child child;
    bool  signalled{false};
    SECTION("nonzero natural exit")
    {
        child.start("--release-exit");
    }
    SECTION("signal termination")
    {
        signalled = true;
        child.start("startup");
        child.await_ready();
        child.signal(SIGKILL);
    }

    // Observe exit before attempting the send: peer closure is established, not inferred from a delay.
    auto const exit = child.collect_exit();
    REQUIRE(exit.status);
    REQUIRE_FALSE(exit.watchdog_killed);
    REQUIRE(exit.observation_error == 0);
    auto const result = child.try_release();
    INFO(result.diagnostic);
    REQUIRE(result.error == EPIPE);
    REQUIRE(result.diagnostic.find("watchdog") == std::string::npos);
    if (signalled) {
        REQUIRE(result.diagnostic.find("child signal=" + std::to_string(SIGKILL)) != std::string::npos);
        REQUIRE(result.diagnostic.find("!relay-ready!") != std::string::npos);
    }
    else {
        REQUIRE(result.diagnostic.find("child exit=23") != std::string::npos);
        REQUIRE(result.diagnostic.find("helper exited before release") != std::string::npos);
    }
}

TEST_CASE("Signal test release preserves permission errors and identifies watchdog cleanup")
{
    Child child;
    child.start("startup");
    child.await_ready();
    ReleaseOperations operations;
    int               sends{0};
    operations.send = [&](int) -> ssize_t {
        ++sends;
        errno = EPERM;
        return -1;
    };

    // The ready child is blocked awaiting release. Expire only the failure-observation watchdog,
    // so the report must distinguish our SIGKILL from a signal-relay crash and preserve EPERM.
    auto const result = child.try_release(operations, std::chrono::milliseconds{0});
    INFO(result.diagnostic);
    REQUIRE(result.error == EPERM);
    REQUIRE(sends == 1);
    REQUIRE(result.diagnostic.find("release errno=" + std::to_string(EPERM)) != std::string::npos);
    REQUIRE(result.diagnostic.find("watchdog sent SIGKILL") != std::string::npos);
    REQUIRE(result.diagnostic.find("!relay-ready!") != std::string::npos);
    auto const exit = child.collect_exit();
    REQUIRE(exit.status);
    REQUIRE(exit.observation_error == 0);
    REQUIRE(WIFSIGNALED(*exit.status));
    REQUIRE(WTERMSIG(*exit.status) == SIGKILL);
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
    SECTION("retirement with admitted handlers and surviving workers")
    {
        scenario = "retirement waits for admitted handlers";
    }
    SECTION("late handler and descriptor reuse after relay destruction")
    {
        scenario = "late handler cannot write a reused descriptor";
    }
    SECTION("exec inheritance and SIGCHLD preservation")
    {
        scenario = "descriptor inheritance";
    }
    Child child;
    child.start(scenario);
    child.require_success();
}
