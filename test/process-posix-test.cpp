#include "process.hpp"

#include "event_loop.hpp"
#include "event_loop_backend.hpp"
#if defined(__linux__)
#  include "event_loop_backend_epoll_priv.hpp"
#  include <sys/epoll.h>
#else
#  include "event_loop_backend_kqueue_priv.hpp"
#  include <sys/event.h>
#endif
#include "process_posix_priv.hpp"
#include "support/catch_utils.hpp" // IWYU pragma: keep for Catch::StringMaker specializations
#include "support/fake_event_loop_backend.hpp"
#include "support/temporary_directory.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal> // IWYU pragma: keep Provides POSIX signal sets and dispositions.
#include <cstdint>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace jb::core;
using namespace jb::core::priv;
using namespace std::chrono_literals;

namespace {
auto helper(std::vector<std::string> arguments = {"exit", "37"}) -> ProcessStartInfo
{
    return {.executable = PROCESS_TEST_HELPER, .arguments = std::move(arguments)};
}

auto descriptor_count() -> std::size_t
{
#if defined(__APPLE__)
    auto const* path = "/dev/fd";
#else
    auto const* path = "/proc/self/fd";
#endif
    return static_cast<std::size_t>(
        std::distance(std::filesystem::directory_iterator{path}, std::filesystem::directory_iterator{}));
}

/// @throws Catch::TestFailureException when the test did not obtain a positive owned PID.
void check_reaped(pid_t pid)
{
    REQUIRE(pid > 0);
    CHECK(::waitpid(pid, nullptr, WNOHANG) == -1);
    CHECK(errno == ECHILD);
}

/// Native readiness and bounded watchdogs drive integration; success never depends on elapsed time.
struct Run {
    std::unique_ptr<EventLoop> loop;
    ScopedCurrentEventLoop     current;
    int                        starts{0};
    int                        finishes{0};
    std::optional<ProcessExit> result;
    ProcessState               expected_started_state{ProcessState::Running};
    std::array<ByteBuffer, 2>  bytes;
    Object                     receiver;
    Process                    process;

    /// @throws Catch::TestFailureException when the test backend cannot initialize.
    explicit Run(std::unique_ptr<Backend> backend = make_backend())
        : loop(EventLoopTestAccess::make_event_loop(std::move(backend)))
        , current(loop.get())
    {
        REQUIRE(loop->is_valid());
        auto started  = process.started.connect(&receiver, [this] {
            ++starts;
            CHECK(process.state() == expected_started_state);
            CHECK(EventLoop::current() == loop.get());
        });
        auto finished = process.finished.connect(&receiver, [this](ProcessExit const& exit) {
            ++finishes;
            result = exit;
            CHECK(process.state() == ProcessState::NotRunning);
            CHECK_FALSE(process.process_id());
            CHECK(EventLoop::current() == loop.get());
        });
        auto collect  = [this](std::size_t index, ByteBuffer const& chunk) {
            CHECK(starts == finishes + 1);
            CHECK_FALSE(chunk.empty());
            CHECK(chunk.size() <= std::size_t{64} * 1024);
            CHECK(EventLoop::current() == loop.get());
            bytes[index].insert(bytes[index].end(), chunk.begin(), chunk.end());
        };
        auto output =
            process.standard_output.connect(&receiver, [collect](ByteBuffer const& chunk) { collect(0, chunk); });
        auto error =
            process.standard_error.connect(&receiver, [collect](ByteBuffer const& chunk) { collect(1, chunk); });
    }

    /// @throws Catch::TestFailureException when readiness fails or the watchdog expires.
    void until(std::function<bool()> const& predicate, EventFlags flags = EventFlag::All) const
    {
        auto const deadline = Clock::now() + 3s;
        while (!predicate() && Clock::now() < deadline) {
            REQUIRE(loop->process_events(flags, 10) != ProcessEventsResult::Failed);
        }
        REQUIRE(predicate());
    }

    /// @throws Catch::TestFailureException when launch, completion, or ownership assertions fail.
    auto execute(ProcessStartInfo info = helper()) -> ProcessExit
    {
        auto const previous_starts   = starts;
        auto const previous_finishes = finishes;
        result.reset();
        auto accepted = process.start(std::move(info));
        if (!accepted) {
            UNSCOPED_INFO("start error: " << accepted.error().code << " " << accepted.error().detail);
        }
        REQUIRE(accepted);
        CHECK(starts == previous_starts);
        CHECK(finishes == previous_finishes);
        CHECK(process.state() == ProcessState::Starting);
        auto const pid       = static_cast<pid_t>(*process.process_id());
        auto       duplicate = process.start(helper());
        REQUIRE_FALSE(duplicate);
        CHECK(duplicate.error().code == "core.process.invalid_state");
        until([this] { return result.has_value(); });
        CHECK(finishes == previous_finishes + 1);
        check_reaped(pid);
        REQUIRE(loop->process_events(EventFlag::All, 0) != ProcessEventsResult::Failed);
        CHECK(finishes == previous_finishes + 1);
        return *result;
    }
};

/// Parent syscall faults and observations never execute framework code in the child branch.
class ScriptedOperations : public ProcessOperations {
public:
    struct SignalCall {
        bool  group;
        pid_t id;
        int   signal;
    };

    enum class Failure : std::uint8_t {
        None,
        Open,
        Pipe,
        Gate,
        Fork,
        Group,
        Send,
        ShortSend,
        DeadChild
    };

    ScriptedOperations()
        : options{ProcessOperations::child_options()}
    {}

    Failure                  failure{Failure::None};
    ProcessChildOptions      options;
    pid_t                    observed_pid{-1};
    int                      fork_calls{0};
    int                      send_calls{0};
    int                      status_fd{-1};
    std::array<int, 2>       output_fds{-1, -1};
    int                      pipe_calls{0};
    int                      fail_pipe_call{0};
    EventLoop*               loop{nullptr};
    bool                     watches_before_release{false};
    bool                     blocked_at_creation{false};
    bool                     restored_at_release{false};
    bool                     send_interrupted{false};
    std::function<void()>    before_send;
    std::optional<TimePoint> now;
    std::deque<TimePoint>    now_values;
    std::deque<int>          group_signal_errors;
    std::vector<SignalCall>  signal_calls;
    std::vector<int>         wait_options;
    std::vector<char>        lifecycle_calls;

    auto open_null(int flags) noexcept -> int override
    {
        pipe_calls = 0;
        if (failure == Failure::Open) {
            errno = EMFILE;
            return -1;
        }
        return ProcessOperations::open_null(flags);
    }

    auto make_pipe(int* pair) noexcept -> int override
    {
        ++pipe_calls;
        if (failure == Failure::Pipe || pipe_calls == fail_pipe_call) {
            errno = EMFILE;
            return -1;
        }
        auto const result = ProcessOperations::make_pipe(pair);
        if (result == 0) {
            // Fake backends own no native descriptor. Normalize here so the recorded test seam follows the
            // descriptor that Process watches instead of a low number that production normalization closes.
            for (int i = 0; i < 2; ++i) {
                if (pair[i] > 3) {
                    continue;
                }
                auto const normalized = ::fcntl(pair[i], F_DUPFD_CLOEXEC, 4);
                if (normalized < 0) {
                    auto const error = errno;
                    ::close(pair[0]);
                    ::close(pair[1]);
                    errno = error;
                    return -1;
                }
                ::close(pair[i]);
                pair[i] = normalized;
            }

            // Every run creates exec status first, then stdout and stderr.
            auto const index = (pipe_calls - 1) % 3;
            if (index == 0) {
                status_fd = pair[0];
            }
            else {
                output_fds[static_cast<std::size_t>(index - 1)] = pair[0];
            }
        }
        return result;
    }

    auto make_gate(int* pair) noexcept -> int override
    {
        if (failure == Failure::Gate) {
            errno = EMFILE;
            return -1;
        }
        return ProcessOperations::make_gate(pair);
    }

    auto create_child() noexcept -> pid_t override
    {
        ++fork_calls;
        sigset_t mask{};
        ::pthread_sigmask(SIG_SETMASK, nullptr, &mask);
        blocked_at_creation = sigismember(&mask, SIGHUP) == 1 && sigismember(&mask, SIGUSR1) == 1;
        if (failure == Failure::Fork) {
            errno = EAGAIN;
            return -1;
        }
        auto const pid = ProcessOperations::create_child();
        if (pid > 0) {
            observed_pid = pid;
        }
        return pid;
    }

    auto establish_group(pid_t pid) noexcept -> int override
    {
        if (failure == Failure::Group) {
            errno = EPERM;
            return -1;
        }
        return ProcessOperations::establish_group(pid);
    }

    /// @throws std::exception from the parent-only before_send probe, before any gate byte is sent.
    auto release_gate(int fd, pid_t pid) -> ssize_t override
    {
        ++send_calls;
        if (before_send) {
            before_send();
        }
        sigset_t mask{};
        ::pthread_sigmask(SIG_SETMASK, nullptr, &mask);
        restored_at_release = sigismember(&mask, SIGHUP) == 0;
        if (loop) {
            watches_before_release = EventLoopTestAccess::fd_callback(*loop, status_fd) &&
                                     EventLoopTestAccess::process_callback(*loop, pid) &&
                                     EventLoopTestAccess::fd_callback(*loop, output_fds[0]) &&
                                     EventLoopTestAccess::fd_callback(*loop, output_fds[1]);
        }
        if (send_interrupted && send_calls == 1) {
            errno = EINTR;
            return -1;
        }
        if (failure == Failure::Send) {
            errno = EPIPE;
            return -1;
        }
        if (failure == Failure::ShortSend) {
            return 0;
        }
        if (failure == Failure::DeadChild) {
            ::kill(pid, SIGKILL);
            // Observe without reaping: Process remains the sole reaping owner, while socket peer closure is certain.
            siginfo_t info{};
            while (::waitid(P_PID, static_cast<id_t>(pid), &info, WEXITED | WNOWAIT) < 0 && errno == EINTR) {
            }
        }
        return ProcessOperations::release_gate(fd, pid);
    }

    auto child_options() noexcept -> ProcessChildOptions override { return options; }

    auto monotonic_now() noexcept -> TimePoint override
    {
        if (!now_values.empty()) {
            auto const value = now_values.front();
            now_values.pop_front();
            return value;
        }
        if (now) {
            return *now;
        }
        return ProcessOperations::monotonic_now();
    }

    auto signal_group(pid_t group_id, int signal) noexcept -> int override
    {
        signal_calls.push_back({.group = true, .id = group_id, .signal = signal});
        lifecycle_calls.push_back(signal == SIGTERM ? 'T' : 'K');
        if (!group_signal_errors.empty()) {
            auto const error = group_signal_errors.front();
            group_signal_errors.pop_front();
            if (error != 0) {
                errno = error;
                return -1;
            }
        }
        return ProcessOperations::signal_group(group_id, signal);
    }

    auto signal_process(pid_t process_id, int signal) noexcept -> int override
    {
        signal_calls.push_back({.group = false, .id = process_id, .signal = signal});
        lifecycle_calls.push_back('D');
        return ProcessOperations::signal_process(process_id, signal);
    }

    auto wait_process(pid_t process_id, int* status, int options) noexcept -> pid_t override
    {
        wait_options.push_back(options);
        lifecycle_calls.push_back('W');
        return ProcessOperations::wait_process(process_id, status, options);
    }
};

class OutputOperations final : public ScriptedOperations {
public:
    std::array<std::deque<int>, 2> read_errors;
    std::array<bool, 2>            hold{};
    std::array<bool, 2>            refill{};
    std::array<bool, 2>            eof{};
    std::array<std::size_t, 2>     synthetic_remaining{};
    std::array<std::size_t, 2>     read_calls{};
    std::array<std::size_t, 2>     read_bytes{};
    std::size_t                    max_request{0};
    bool                           refill_failed{false};

    auto read_output(int fd, void* buffer, std::size_t size) noexcept -> ssize_t override
    {
        auto const index = fd == output_fds[0] ? std::size_t{0} : std::size_t{1};
        ++read_calls[index];
        max_request = std::max(max_request, size);
        if (!read_errors[index].empty()) {
            errno = read_errors[index].front();
            read_errors[index].pop_front();
            return -1;
        }
        if (hold[index]) {
            errno = EAGAIN;
            return -1;
        }
        if (synthetic_remaining[index] != 0) {
            auto const count = std::min(size, synthetic_remaining[index]);
            auto*      bytes = static_cast<std::byte*>(buffer);
            for (std::size_t i = 0; i < count; ++i) {
                bytes[i] = static_cast<std::byte>((read_bytes[index] + i + (index * 73)) % 251);
            }
            synthetic_remaining[index] -= count;
            read_bytes[index]          += count;
            return static_cast<ssize_t>(count);
        }
        if (refill[index]) {
            // Test-only coordination keeps the native endless writer supplied between reads. The production
            // operation remains a nonblocking read; this watchdog never constitutes the success predicate.
            pollfd item{.fd = fd, .events = POLLIN, .revents = 0};
            int    ready;
            do {
                ready = ::poll(&item, 1, 3000);
            } while (ready < 0 && errno == EINTR);
            if (ready <= 0) {
                refill_failed = true;
                errno         = EIO;
                return -1;
            }
        }
        auto const count = ProcessOperations::read_output(fd, buffer, size);
        if (count > 0) {
            read_bytes[index] += static_cast<std::size_t>(count);
        }
        eof[index] = count == 0;
        return count;
    }
};

/// @throws Catch::TestFailureException if the child cannot be observed without taking Process's reaping ownership.
void observe_exit(pid_t pid)
{
    siginfo_t info{};
    int       result;
    do {
        result = ::waitid(P_PID, static_cast<id_t>(pid), &info, WEXITED | WNOWAIT);
    } while (result < 0 && errno == EINTR);
    REQUIRE(result == 0);
}

/// @throws Catch::TestFailureException if the child does not reach its injected stopped state.
void observe_stop(pid_t pid)
{
    siginfo_t info{};
    int       result;
    do {
        result = ::waitid(P_PID, static_cast<id_t>(pid), &info, WSTOPPED | WNOWAIT);
    } while (result < 0 && errno == EINTR);

    REQUIRE(result == 0);
    CHECK(info.si_code == CLD_STOPPED);
    CHECK(info.si_status == SIGSTOP);
}

/// @throws Catch::TestFailureException when deterministic helper coordination does not arrive before the watchdog.
void read_exact(int fd, void* destination, std::size_t size)
{
    auto*       bytes    = static_cast<char*>(destination);
    std::size_t received = 0;
    while (received < size) {
        pollfd item{.fd = fd, .events = POLLIN, .revents = 0};
        REQUIRE(::poll(&item, 1, 3000) == 1);
        auto const count = ::read(fd, bytes + received, size - received);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        REQUIRE(count > 0);
        received += static_cast<std::size_t>(count);
    }
}

/// Identity-stable observation of a helper descendant that this test process cannot reap.
class ProcessTerminationWatch final {
public:
    /// @throws Catch::TestFailureException when the PID is invalid or cannot be watched.
    explicit ProcessTerminationWatch(pid_t pid)
        : _pid(pid)
    {
        REQUIRE(pid > 0);

#if defined(__APPLE__)
        _fd = ::kqueue();
        REQUIRE(_fd >= 0);
        struct kevent change;
        EV_SET(&change, pid, EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT, 0, nullptr);
        int registered;
        do {
            registered = ::kevent(_fd, &change, 1, nullptr, 0, nullptr);
        } while (registered < 0 && errno == EINTR);
        if (registered == 0) {
            return;
        }
        auto const error = errno;
        ::close(_fd);
        _fd = -1;
#else
        EpollProcessOperations operations;
        _fd = operations.open_pidfd(pid);
        if (_fd >= 0) {
            return;
        }

        auto const error = errno;
#endif
        UNSCOPED_INFO("descendant watch for " << pid << " failed with errno " << error);
        REQUIRE(error == ESRCH);
    }

    ~ProcessTerminationWatch()
    {
        if (_fd >= 0) {
            ::close(_fd);
        }
    }

    ProcessTerminationWatch(ProcessTerminationWatch const&)                    = delete;
    auto operator=(ProcessTerminationWatch const&) -> ProcessTerminationWatch& = delete;

    /// @throws Catch::TestFailureException when the process does not terminate within the watchdog interval.
    void check_terminated() const
    {
        if (_fd < 0) {
            return;
        }

#if defined(__APPLE__)
        struct kevent   event;
        struct timespec timeout{.tv_sec = 3, .tv_nsec = 0};
        int             ready;
        do {
            ready = ::kevent(_fd, nullptr, 0, &event, 1, &timeout);
        } while (ready < 0 && errno == EINTR);
        REQUIRE(ready == 1);
        CHECK(event.filter == EVFILT_PROC);
        CHECK(event.ident == static_cast<std::uintptr_t>(_pid));
        CHECK((event.fflags & NOTE_EXIT) != 0);
#else
        pollfd item{.fd = _fd, .events = POLLIN, .revents = 0};
        int    ready;
        do {
            ready = ::poll(&item, 1, 3000);
        } while (ready < 0 && errno == EINTR);

        if (ready < 0) {
            UNSCOPED_INFO("poll(pidfd for " << _pid << ") failed with errno " << errno);
        }
        REQUIRE(ready == 1);
        CHECK((item.revents & POLLIN) != 0);
#endif
    }

private:
    pid_t _pid;
    int   _fd{-1};
};

auto group_signal_count(ScriptedOperations const& operations, int signal) -> std::size_t
{
    return static_cast<std::size_t>(std::ranges::count_if(operations.signal_calls, [signal](auto const& call) {
        return call.group && call.signal == signal;
    }));
}

auto matches_pattern(ByteBuffer const& bytes, std::size_t channel, std::size_t offset = 0) -> bool
{
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (bytes[i] != static_cast<std::byte>((offset + i + (channel * 73)) % 251)) {
            return false;
        }
    }
    return true;
}

/// The target opens this FIFO by path after exec, so deterministic test coordination does not weaken close-from.
class PostExecChannel final {
public:
    /// @throws Catch::TestFailureException when the private FIFO cannot be created or opened.
    PostExecChannel()
        : _path{_directory.path() / "channel"}
    {
        REQUIRE(::mkfifo(_path.c_str(), 0600) == 0);
        _fd = ::open(_path.c_str(), O_RDWR | O_CLOEXEC);
        REQUIRE(_fd >= 0);
    }

    ~PostExecChannel()
    {
        if (_fd >= 0) {
            ::close(_fd);
        }
    }

    PostExecChannel(PostExecChannel const&)                    = delete;
    auto operator=(PostExecChannel const&) -> PostExecChannel& = delete;

    [[nodiscard]] auto path() const -> std::string { return _path.string(); }

    [[nodiscard]] auto descriptor() const noexcept -> int { return _fd; }

    /// @throws Catch::TestFailureException when the expected target report does not arrive.
    void read(void* destination, std::size_t size) const { read_exact(_fd, destination, size); }

    /// @throws Catch::TestFailureException when target permission cannot be delivered.
    void permit() const { REQUIRE(::write(_fd, "x", 1) == 1); }

private:
    jb::test::TemporaryDirectory _directory;
    std::filesystem::path        _path;
    int                          _fd{-1};
};

/// Deliberately non-CLOEXEC descriptors used to prove Process closes unrelated inherited state.
class InheritedPipe final {
public:
    /// @throws Catch::TestFailureException when the pipe cannot be created above the reserved status descriptor.
    explicit InheritedPipe(int minimum_descriptor = 4)
    {
        REQUIRE(::pipe(_fds.data()) == 0);
        for (auto& fd : _fds) {
            if (fd >= minimum_descriptor) {
                continue;
            }
            auto const normalized = ::fcntl(fd, F_DUPFD, minimum_descriptor);
            REQUIRE(normalized >= 0);
            ::close(fd);
            fd = normalized;
        }
    }

    ~InheritedPipe()
    {
        for (auto fd : _fds) {
            ::close(fd);
        }
    }

    InheritedPipe(InheritedPipe const&)                    = delete;
    auto operator=(InheritedPipe const&) -> InheritedPipe& = delete;

    [[nodiscard]] auto arguments() const -> std::vector<std::string>
    {
        return {"descriptors-closed", std::to_string(_fds[0]), std::to_string(_fds[1])};
    }

private:
    std::array<int, 2> _fds{-1, -1};
};

/// Restores the process-wide descriptor limit even when a test assertion aborts its section.
class DescriptorLimitScope final {
public:
    /// @throws Catch::TestFailureException when the original limit cannot be observed.
    DescriptorLimitScope() { REQUIRE(::getrlimit(RLIMIT_NOFILE, &_original) == 0); }

    ~DescriptorLimitScope()
    {
        if (::setrlimit(RLIMIT_NOFILE, &_original) != 0) {
            std::terminate();
        }
    }

    DescriptorLimitScope(DescriptorLimitScope const&)                    = delete;
    auto operator=(DescriptorLimitScope const&) -> DescriptorLimitScope& = delete;

    /// @throws Catch::TestFailureException when the requested soft limit cannot be installed.
    void lower_soft_to(rlim_t limit)
    {
        REQUIRE(limit < _original.rlim_cur);
        auto lowered     = _original;
        lowered.rlim_cur = limit;
        REQUIRE(::setrlimit(RLIMIT_NOFILE, &lowered) == 0);
    }

private:
    rlimit _original{};
};

class SignalScope final {
public:
    /// @throws Catch::TestFailureException when the original signal state cannot be observed.
    explicit SignalScope(int signal)
        : _signal(signal)
    {
        REQUIRE(::sigaction(signal, nullptr, &_action) == 0);
        REQUIRE(::pthread_sigmask(SIG_SETMASK, nullptr, &_mask) == 0);
    }

    ~SignalScope()
    {
        CHECK(::sigaction(_signal, &_action, nullptr) == 0);
        CHECK(::pthread_sigmask(SIG_SETMASK, &_mask, nullptr) == 0);
    }

    SignalScope(SignalScope const&)                    = delete;
    auto operator=(SignalScope const&) -> SignalScope& = delete;

private:
    int              _signal;
    struct sigaction _action{};
    sigset_t         _mask{};
};

/// @throws Catch::TestFailureException when installing the test signal disposition fails.
void install_handler(int signal, void (*handler)(int))
{
    struct sigaction action{};
    action.sa_handler = handler;
    REQUIRE(sigemptyset(&action.sa_mask) == 0);
    REQUIRE(::sigaction(signal, &action, nullptr) == 0);
}

volatile sig_atomic_t signal_hits{0};

void record_signal(int /*signal*/) noexcept
{
    signal_hits = 1;
}

auto root_identity() noexcept -> uid_t
{
    return 0;
}

auto unavailable_close_range(unsigned int /*first*/, unsigned int /*last*/) noexcept -> int
{
    errno = ENOSYS;
    return -1;
}

auto failed_close_range(unsigned int /*first*/, unsigned int /*last*/) noexcept -> int
{
    errno = EIO;
    return -1;
}

auto failed_privilege_hardening() noexcept -> int
{
    errno = EPERM;
    return -1;
}
} // namespace

TEST_CASE("POSIX Process executes absolute and ordered PATH candidates without missing immediate exits",
          "[core][process][posix]")
{
    Run        run;
    auto const before = descriptor_count();
    for (int i = 0; i < 20; ++i) {
        auto result = run.execute(helper({"exit", std::to_string(i)}));
        CHECK(result.kind == ProcessExitKind::Exited);
        CHECK(result.exit_code == i);
        CHECK_FALSE(result.signal_number);
        CHECK_FALSE(result.start_error);
    }
    auto       request          = helper();
    auto const path             = std::filesystem::path{request.executable};
    request.executable          = path.filename();
    request.environment["PATH"] = "/no-such-process-dir:" + path.parent_path().string();
    CHECK(run.execute(std::move(request)).exit_code == 37);
    CHECK(run.starts == 21);
    CHECK(descriptor_count() == before);
}

TEST_CASE("POSIX Process reports signal and asynchronous exec or directory failures safely", "[core][process][posix]")
{
    Run  run;
    auto signaled = run.execute(helper({"signal", std::to_string(SIGTERM)}));
    CHECK(signaled.kind == ProcessExitKind::Signaled);
    CHECK(signaled.signal_number == SIGTERM);
    CHECK_FALSE(signaled.exit_code);
    CHECK_FALSE(signaled.start_error);
    jb::test::TemporaryDirectory directory;
    auto const                   file = directory.path() / "sensitive-command-marker";
    {
        std::ofstream stream{file};
        stream << "not executable";
    }
    for (bool bad_directory : {false, true}) {
        auto info = helper();
        if (bad_directory) {
            info.working_directory = file;
        }
        else {
            info.executable = file;
        }
        auto const started = run.starts;
        auto       exit    = run.execute(std::move(info));
        CHECK(exit.kind == ProcessExitKind::StartFailed);
        REQUIRE(exit.start_error);
        CHECK(exit.start_error->code == (bad_directory ? "core.process.chdir_failed" : "core.process.exec_failed"));
        CHECK(exit.start_error->detail.find("sensitive-command-marker") == std::string::npos);
        CHECK_FALSE(exit.exit_code);
        CHECK_FALSE(exit.signal_number);
        CHECK(run.starts == started);
    }
    auto missing = run.execute({.executable = "/no-such-process-sensitive-marker"});
    REQUIRE(missing.start_error);
    CHECK(missing.start_error->category == ErrorCategory::NotFound);
    CHECK(missing.start_error->code == "core.process.exec_failed");
}

TEST_CASE("POSIX Process PATH remembers permission denial but continues to a usable candidate",
          "[core][process][posix]")
{
    jb::test::TemporaryDirectory directory;
    auto                         info       = helper();
    auto const                   executable = std::filesystem::path{info.executable};
    info.executable                         = executable.filename();
    {
        std::ofstream stream{directory.path() / info.executable};
        stream << "not executable";
    }
    info.environment["PATH"] = directory.path().string() + ":/no-such-process-dir";
    Run  run;
    auto failed = run.execute(info);
    REQUIRE(failed.start_error);
    CHECK(failed.start_error->category == ErrorCategory::PermissionDenied);
    info.environment["PATH"] += ":" + executable.parent_path().string();
    CHECK(run.execute(std::move(info)).exit_code == 37);
}

TEST_CASE("POSIX Process installs exact argv environment cwd and clean target signal state", "[core][process][posix]")
{
    SignalScope hup{SIGHUP};
    SignalScope usr{SIGUSR1};
    install_handler(SIGHUP, record_signal);
    install_handler(SIGUSR1, SIG_IGN);
    sigset_t blocked{};
    REQUIRE(sigemptyset(&blocked) == 0);
    REQUIRE(sigaddset(&blocked, SIGUSR1) == 0);
    REQUIRE(::pthread_sigmask(SIG_BLOCK, &blocked, nullptr) == 0);
    jb::test::TemporaryDirectory directory;
    Run                          run;
    // getcwd reports the physical path, including macOS's /var -> /private/var resolution.
    auto const                   physical_directory = std::filesystem::canonical(directory.path());
    auto                         info               = helper({"inspect", "", "-option", physical_directory.string()});
    info.working_directory                          = directory.path();
    info.environment["PROCESS_MARKER"]              = "literal $x = value";
    auto const result                               = run.execute(std::move(info));
    CAPTURE(result.exit_code.value_or(-1), result.signal_number.value_or(-1));
    CHECK(result.exit_code == 0);
    sigset_t after{};
    REQUIRE(::pthread_sigmask(SIG_SETMASK, nullptr, &after) == 0);
    CHECK(sigismember(&after, SIGUSR1) == 1);
}

TEST_CASE("POSIX Process closes unrelated descriptors through native and bounded fallback paths",
          "[core][process][posix][security]")
{
    for (bool force_fallback : {false, true}) {
        CAPTURE(force_fallback);
        Run           run;
        InheritedPipe inherited;
        auto          operations = std::make_shared<ScriptedOperations>();
        if (force_fallback) {
            operations->options.close_range = unavailable_close_range;
        }
        ProcessTestAccess::set_operations(run.process, operations);

        auto const result = run.execute(helper(inherited.arguments()));
        CHECK(result.kind == ProcessExitKind::Exited);
        CHECK(result.exit_code == 0);
    }

    SECTION("fallback reaches live descriptors above a lowered allocation limit")
    {
        DescriptorLimitScope limits;
        InheritedPipe        inherited{100};
        limits.lower_soft_to(64);

        Run  run;
        auto operations                 = std::make_shared<ScriptedOperations>();
        operations->options.close_range = unavailable_close_range;
        ProcessTestAccess::set_operations(run.process, operations);

        auto const result = run.execute(helper(inherited.arguments()));
        CHECK(result.kind == ProcessExitKind::Exited);
        CHECK(result.exit_code == 0);
    }

    Run  run;
    auto operations                 = std::make_shared<ScriptedOperations>();
    operations->options.close_range = failed_close_range;
    ProcessTestAccess::set_operations(run.process, operations);
    auto const result = run.execute();
    REQUIRE(result.start_error);
    CHECK(result.start_error->code == "core.process.child_setup_failed");
    CHECK(result.start_error->detail == "child.descriptor_cleanup:" + std::to_string(EIO));
    CHECK(run.starts == 0);
}

TEST_CASE("POSIX Process normalizes closed conventional descriptors and respects the platform atfork contract",
          "[core][process][posix]")
{
    Run run;
    for (int mask : {1, 2, 4, 8, 15}) {
        for (bool missing : {false, true}) {
            auto const result = run.execute(helper({"closed", std::to_string(mask), missing ? "1" : "0"}));
            CAPTURE(mask, missing, result.exit_code.value_or(-1), result.signal_number.value_or(-1));
            CHECK(result.exit_code == 0);
        }
    }
    CHECK(run.execute(helper({"atfork"})).exit_code == 0);
}

TEST_CASE("POSIX Process gates execution on watch acceptance and restores parent masks", "[core][process][posix]")
{
    SignalScope mask{SIGHUP};
    sigset_t    empty{};
    REQUIRE(sigemptyset(&empty) == 0);
    REQUIRE(::pthread_sigmask(SIG_SETMASK, &empty, nullptr) == 0);
    Run  run;
    auto operations              = std::make_shared<ScriptedOperations>();
    operations->loop             = run.loop.get();
    operations->send_interrupted = true;
    jb::test::TemporaryDirectory directory;
    auto const                   marker = directory.path() / "executed";
    operations->before_send             = [&] { CHECK_FALSE(std::filesystem::exists(marker)); };
    ProcessTestAccess::set_operations(run.process, operations);
    CHECK(run.execute(helper({"marker", marker.string()})).exit_code == 37);
    CHECK(std::filesystem::exists(marker));
    CHECK(operations->watches_before_release);
    CHECK(operations->blocked_at_creation);
    CHECK(operations->restored_at_release);
    CHECK(operations->send_calls == 2);
}

TEST_CASE("POSIX Process rejects parent setup faults without signals descriptors or zombies", "[core][process][posix]")
{
    using Failure = ScriptedOperations::Failure;
    for (auto failure : {Failure::Open,
                         Failure::Pipe,
                         Failure::Gate,
                         Failure::Fork,
                         Failure::Group,
                         Failure::Send,
                         Failure::ShortSend,
                         Failure::DeadChild}) {
        CAPTURE(static_cast<int>(failure));
        Run  run;
        auto operations     = std::make_shared<ScriptedOperations>();
        operations->failure = failure;
        ProcessTestAccess::set_operations(run.process, operations);
        auto const count = descriptor_count();
        sigset_t   before{};
        REQUIRE(::pthread_sigmask(SIG_SETMASK, nullptr, &before) == 0);
        auto result = run.process.start(helper());
        REQUIRE_FALSE(result);
        auto const* expected = "core.process.child_setup_failed";
        if (failure == Failure::Open || failure == Failure::Pipe || failure == Failure::Gate) {
            expected = "core.process.resource_setup_failed";
        }
        else if (failure == Failure::Fork) {
            expected = "core.process.fork_failed";
        }
        CHECK(result.error().code == expected);
        sigset_t after{};
        REQUIRE(::pthread_sigmask(SIG_SETMASK, nullptr, &after) == 0);
        for (int signal = 1; signal < NSIG; ++signal) {
            CHECK(sigismember(&before, signal) == sigismember(&after, signal));
        }
        CHECK(run.process.state() == ProcessState::NotRunning);
        CHECK_FALSE(run.process.process_id());
        CHECK(EventLoopTestAccess::active_process_count(*run.loop) == 0);
        CHECK(EventLoopTestAccess::active_timer_count(*run.loop) == 0);
        CHECK_FALSE(EventLoopTestAccess::fd_callback(*run.loop, operations->status_fd));
        REQUIRE(run.loop->process_events(EventFlag::All, 0) != ProcessEventsResult::Failed);
        CHECK(run.starts == 0);
        CHECK(run.finishes == 0);
        CHECK(descriptor_count() == count);
        if (operations->observed_pid > 0) {
            check_reaped(operations->observed_pid);
        }
        operations->failure = Failure::None;
        CHECK(run.execute().exit_code == 37);
    }
}

TEST_CASE("POSIX Process unwinds parent setup without releasing the target or retaining ownership",
          "[core][process][posix]")
{
    Run  run;
    auto operations         = std::make_shared<ScriptedOperations>();
    operations->before_send = [] { throw std::runtime_error{"parent setup probe"}; };
    ProcessTestAccess::set_operations(run.process, operations);
    jb::test::TemporaryDirectory directory;
    auto const                   marker      = directory.path() / "executed";
    auto const                   descriptors = descriptor_count();

    // Inject a non-allocation exception after all watches exist; never catch or simulate std::bad_alloc recovery.
    REQUIRE_THROWS_AS(run.process.start(helper({"marker", marker.string()})), std::runtime_error);
    CHECK(run.process.state() == ProcessState::NotRunning);
    CHECK_FALSE(run.process.process_id());
    CHECK_FALSE(std::filesystem::exists(marker));
    CHECK_FALSE(EventLoopTestAccess::fd_callback(*run.loop, operations->status_fd));
    CHECK(EventLoopTestAccess::active_process_count(*run.loop) == 0);
    CHECK(EventLoopTestAccess::active_timer_count(*run.loop) == 0);
    CHECK(descriptor_count() == descriptors);
    check_reaped(operations->observed_pid);
    CHECK(run.loop->process_events(EventFlag::All, 0) == ProcessEventsResult::Stopped);
    CHECK(run.starts == 0);
    CHECK(run.finishes == 0);

    operations->before_send = {};
    CHECK(run.execute().exit_code == 37);
}

TEST_CASE("POSIX Process child failures map safe setup and authoritative identity stages", "[core][process][posix]")
{
    Run  run;
    auto operations = std::make_shared<ScriptedOperations>();
    ProcessTestAccess::set_operations(run.process, operations);
    for (auto stage : {ProcessChildStage::Group,
                       ProcessChildStage::Descriptors,
                       ProcessChildStage::DescriptorCleanup,
                       ProcessChildStage::Signals}) {
        operations->options.fail_stage = stage;
        auto const starts              = run.starts;
        auto       result              = run.execute();
        REQUIRE(result.start_error);
        CHECK(result.kind == ProcessExitKind::StartFailed);
        CHECK(result.start_error->code == "core.process.child_setup_failed");
        CHECK(run.starts == starts);
    }
    operations->options.fail_stage    = ProcessChildStage::None;
    operations->options.effective_uid = root_identity;
    auto info                         = helper();
    info.require_non_root             = true;
    auto result                       = run.execute(std::move(info));
    REQUIRE(result.start_error);
    CHECK(result.start_error->code == "core.process.security_failed");
    CHECK(result.start_error->category == ErrorCategory::PermissionDenied);
    CHECK(run.execute().exit_code == 37);
}

TEST_CASE("POSIX Process inherited handlers cannot run before reset and pre-exec signal death is observable",
          "[core][process][posix]")
{
    Run  run;
    auto operations = std::make_shared<ScriptedOperations>();
    ProcessTestAccess::set_operations(run.process, operations);
    for (int signal : {SIGHUP, SIGUSR1}) {
        SignalScope scope{signal};
        install_handler(signal, record_signal);
        for (bool before_reset : {false, true}) {
            operations->options.signal_before_reset = before_reset ? signal : 0;
            operations->options.signal_before_exec  = before_reset ? 0 : signal;
            auto const starts                       = run.starts;
            auto       result                       = run.execute();
            CHECK(result.kind == ProcessExitKind::Signaled);
            CHECK(result.signal_number == signal);
            CHECK_FALSE(result.start_error);
            // Clean EOF reports only launch-channel resolution, not proof of target execution.
            CHECK(run.starts == starts + 1);
        }
    }
}

TEST_CASE("POSIX Process dead gate neither generates SIGPIPE nor consumes host pending signals",
          "[core][process][posix]")
{
    SignalScope scope{SIGPIPE};
    install_handler(SIGPIPE, record_signal);
    sigset_t pipe_signal{};
    REQUIRE(sigemptyset(&pipe_signal) == 0);
    REQUIRE(sigaddset(&pipe_signal, SIGPIPE) == 0);
    for (int pending_mode : {0, 1, 2}) {
        CAPTURE(pending_mode);
        signal_hits = 0;
        REQUIRE(::pthread_sigmask(pending_mode == 0 ? SIG_UNBLOCK : SIG_BLOCK, &pipe_signal, nullptr) == 0);
        Run  run;
        auto operations       = std::make_shared<ScriptedOperations>();
        operations->failure   = ScriptedOperations::Failure::DeadChild;
        pthread_t const owner = ::pthread_self();
        if (pending_mode == 1) {
            REQUIRE(::pthread_kill(owner, SIGPIPE) == 0);
        }
        if (pending_mode == 2) {
            operations->before_send = [owner] {
                // Join is a deterministic barrier: the thread-directed host signal exists during gate release.
                std::thread sender{[owner] { CHECK(::pthread_kill(owner, SIGPIPE) == 0); }};
                sender.join();
            };
        }
        ProcessTestAccess::set_operations(run.process, operations);
        auto result = run.process.start(helper());
        REQUIRE_FALSE(result);
        CHECK(result.error().code == "core.process.child_setup_failed");
        CHECK(signal_hits == 0);
        sigset_t pending{};
        REQUIRE(::sigpending(&pending) == 0);
        CHECK(sigismember(&pending, SIGPIPE) == (pending_mode == 0 ? 0 : 1));
        if (pending_mode != 0) {
            int signal{};
            // The test consumes only the host signal it deliberately queued, before restoring the original mask.
            REQUIRE(::sigwait(&pipe_signal, &signal) == 0);
            CHECK(signal == SIGPIPE);
        }
        check_reaped(operations->observed_pid);
    }
}

TEST_CASE("POSIX Process rejects watch failure before releasing the child", "[core][process][posix]")
{
    jb::test::TemporaryDirectory directory;
    auto const                   marker = directory.path() / "executed";
    for (int mode : {0, 1, 2}) {
        auto  backend       = std::make_unique<FakeEventLoopBackend>();
        auto* fake          = backend.get();
        fake->add_fd_result = mode != 0;
        fake->add_process_result =
            mode == 1 ? ProcessRegistrationResult::Unsupported : ProcessRegistrationResult::Failed;
        Run  run{std::move(backend)};
        auto operations = std::make_shared<ScriptedOperations>();
        ProcessTestAccess::set_operations(run.process, operations);
        auto const count  = descriptor_count();
        auto       result = run.process.start(helper({"marker", marker.string()}));
        REQUIRE_FALSE(result);
        CHECK(result.error().code == (mode == 1 ? "core.process.monitor_unsupported" : "core.process.watch_failed"));
        CHECK(operations->send_calls == 0);
        CHECK_FALSE(std::filesystem::exists(marker));
        CHECK(EventLoopTestAccess::active_process_count(*run.loop) == 0);
        CHECK_FALSE(EventLoopTestAccess::fd_callback(*run.loop, operations->status_fd));
        CHECK(descriptor_count() == count);
        check_reaped(operations->observed_pid);
        CHECK(run.starts == 0);
        CHECK(run.finishes == 0);
    }
}

TEST_CASE("POSIX Process old callbacks stay inert after failed removal restart and destruction",
          "[core][process][posix]")
{
    auto  backend               = std::make_unique<FakeEventLoopBackend>();
    auto* fake                  = backend.get();
    fake->remove_fd_result      = false;
    fake->remove_process_result = false;
    auto                   loop = EventLoopTestAccess::make_event_loop(std::move(backend));
    ScopedCurrentEventLoop current{loop.get()};
    Object                 receiver;
    int                    finished{0};
    auto                   process    = std::make_unique<Process>();
    auto                   connection = process->finished.connect(&receiver, [&](ProcessExit const&) { ++finished; });
    auto                   operations = std::make_shared<OutputOperations>();
    ProcessTestAccess::set_operations(*process, operations);
    std::vector<FdCallback> stale_fds;
    std::vector<Task>       stale_processes;
    for (int run = 0; run < 3; ++run) {
        REQUIRE(process->start(helper()));
        auto const pid = operations->observed_pid;
        auto const fd  = operations->status_fd;
        stale_fds.push_back(EventLoopTestAccess::fd_callback(*loop, fd));
        for (auto output_fd : operations->output_fds) {
            stale_fds.push_back(EventLoopTestAccess::fd_callback(*loop, output_fd));
        }
        stale_processes.push_back(EventLoopTestAccess::process_callback(*loop, pid));
        siginfo_t info{};
        int       waited;
        do {
            waited = ::waitid(P_PID, static_cast<id_t>(pid), &info, WEXITED | WNOWAIT);
        } while (waited < 0 && errno == EINTR);
        REQUIRE(waited == 0);
        // Process exit retires both callbacks; subsequent events were already returned in the same poll batch.
        fake->ready_events = {
            {.kind = ReadyEventKind::Process,    .ident = pid           },
            {.ident = fd,                        .events = FdEvent::Read},
            {.ident = operations->output_fds[0], .events = FdEvent::Read},
            {.ident = operations->output_fds[1], .events = FdEvent::Read},
            {.kind = ReadyEventKind::Process,    .ident = pid           }
        };
        CHECK(loop->process_events(EventFlag::All, 0) == ProcessEventsResult::Stopped);
        CHECK(finished == run + 1);
        check_reaped(pid);
        auto const reads = operations->read_calls;
        for (auto const& callback : stale_fds) {
            callback(fd, FdEvent::Read);
        }
        for (auto const& callback : stale_processes) {
            callback();
        }
        CHECK(finished == run + 1);
        CHECK(operations->read_calls == reads);
    }
    REQUIRE(process->start(helper()));
    auto const active_pid = operations->observed_pid;
    auto const active_fd  = operations->status_fd;
    auto const reads      = operations->read_calls;
    // Old callbacks must not resolve or reap a newly accepted run.
    for (auto const& callback : stale_fds) {
        callback(active_fd, FdEvent::Read);
    }
    for (auto const& callback : stale_processes) {
        callback();
    }
    CHECK(process->state() == ProcessState::Starting);
    CHECK(finished == 3);
    CHECK(operations->read_calls == reads);
    stale_fds.push_back(EventLoopTestAccess::fd_callback(*loop, active_fd));
    for (auto output_fd : operations->output_fds) {
        stale_fds.push_back(EventLoopTestAccess::fd_callback(*loop, output_fd));
    }
    stale_processes.push_back(EventLoopTestAccess::process_callback(*loop, active_pid));
    process.reset();
    check_reaped(active_pid);
    for (auto const& callback : stale_fds) {
        callback(active_fd, FdEvent::Read);
    }
    for (auto const& callback : stale_processes) {
        callback();
    }
    fake->ready_events = {
        {.ident = active_fd,                 .events = FdEvent::Read},
        {.ident = operations->output_fds[0], .events = FdEvent::Read},
        {.ident = operations->output_fds[1], .events = FdEvent::Read},
        {.kind = ReadyEventKind::Process,    .ident = active_pid    }
    };
    CHECK(loop->process_events(EventFlag::All, 0) == ProcessEventsResult::Stopped);
    CHECK(finished == 3);
    CHECK(operations->read_calls == reads);
}

namespace {
#if defined(__linux__)
class RetainedProcessWatch final : public EpollProcessOperations {
public:
    int removals{0};

    auto control(int poller, int operation, int fd, epoll_event* event) noexcept -> int override
    {
        if (operation == EPOLL_CTL_DEL) {
            ++removals;
            errno = EIO;
            return -1;
        }
        return EpollProcessOperations::control(poller, operation, fd, event);
    }
};
#else
class RetainedProcessWatch final : public KqueueProcessOperations {
public:
    int removals{0};

    auto control(int poller, std::int64_t pid, std::uint16_t flags) -> int override
    {
        if ((flags & EV_DELETE) != 0U) {
            ++removals;
            errno = EIO;
            return -1;
        }
        return KqueueProcessOperations::control(poller, pid, flags);
    }
};
#endif
} // namespace

TEST_CASE("POSIX Process retained delivered watch does not redispatch or block unrelated work",
          "[core][process][posix]")
{
    auto native = std::make_shared<RetainedProcessWatch>();
#if defined(__linux__)
    Run run{make_epoll_backend(native)};
#else
    Run run{make_kqueue_backend(native)};
#endif
    auto operations = std::make_shared<ScriptedOperations>();
    Task stale;
    operations->before_send = [&] {
        stale = EventLoopTestAccess::process_callback(*run.loop, operations->observed_pid);
    };
    ProcessTestAccess::set_operations(run.process, operations);
    CHECK(run.execute().exit_code == 37);
    CHECK(native->removals == 1);
    REQUIRE(stale);
    for (int i = 0; i < 10; ++i) {
        stale();
        CHECK(run.loop->process_events(EventFlag::All, 0) == ProcessEventsResult::Stopped);
    }
    CHECK(native->removals == 1);
    bool task_ran{false};
    bool timer_ran{false};
    REQUIRE(run.loop->post([&] { task_ran = true; }));
    REQUIRE(run.loop->post_at(Clock::now(), [&] { timer_ran = true; }));
    CHECK(run.loop->process_events(EventFlag::All, 0) == ProcessEventsResult::Stopped);
    CHECK(task_ran);
    CHECK(timer_ran);
    CHECK(run.execute().exit_code == 37);
}

TEST_CASE("POSIX Process direct finished slots can restart safely", "[core][process][posix]")
{
    Run  run;
    int  count{0};
    auto connection = run.process.finished.connect(&run.receiver, [&](ProcessExit const&) {
        CHECK(run.bytes[0].size() == 8193);
        CHECK(run.bytes[1].size() == 4097);
        CHECK(matches_pattern(run.bytes[0], 0));
        CHECK(matches_pattern(run.bytes[1], 1));
        run.bytes = {};
        if (++count < 3) {
            CHECK(run.process.start(helper({"output", "8193", "4097"})));
        }
    });
    REQUIRE(run.process.start(helper({"output", "8193", "4097"})));
    run.until([&] { return count == 3; });
    CHECK(run.starts == 3);
    CHECK(run.finishes == 3);
}

TEST_CASE("POSIX Process direct lifecycle slots defer destruction without leaking the child", "[core][process][posix]")
{
    for (bool on_started : {false, true}) {
        EventLoop              loop;
        ScopedCurrentEventLoop current{&loop};
        Object                 receiver;
        auto*                  process = new Process;
        bool                   destroyed{false};
        bool                   slot_returned{false};
        auto                   destroy  = process->destroyed.connect(&receiver, [&] { destroyed = true; });
        auto                   schedule = [&] {
            process->delete_later();
            slot_returned = true;
        };
        auto            start  = process->started.connect(&receiver, [&] {
            if (on_started) {
                schedule();
            }
        });
        auto            finish = process->finished.connect(&receiver, [&](ProcessExit const&) {
            if (!on_started) {
                schedule();
            }
        });
        // A blocked helper keeps direct-child ownership active when started schedules deletion.
        PostExecChannel gate;
        auto            accepted = process->start(on_started ? helper({"wait", gate.path()}) : helper());
        REQUIRE(accepted);
        auto const pid      = static_cast<pid_t>(*process->process_id());
        auto const deadline = Clock::now() + 3s;
        while (!destroyed && Clock::now() < deadline) {
            REQUIRE(loop.process_events(EventFlag::All, 10) != ProcessEventsResult::Failed);
        }
        CHECK(destroyed);
        CHECK(slot_returned);
        if (!destroyed) {
            delete process;
        }
        check_reaped(pid);
    }
}

TEST_CASE("POSIX Process enforces and safely reports strict privilege hardening", "[core][process][posix][security]")
{
#if defined(__linux__)
    SECTION("target observes Linux no-new-privileges")
    {
        Run  run;
        auto info                   = helper({"no-new-privileges"});
        info.prevent_privilege_gain = true;
        auto const result           = run.execute(std::move(info));
        CHECK(result.kind == ProcessExitKind::Exited);
        CHECK(result.exit_code == 0);
        CHECK(std::ranges::equal(run.bytes[0], as_bytes("NoNewPrivs: 1\n")));
    }

#endif

    SECTION("unsupported strict hardening rejects before native setup")
    {
        Run  run;
        auto operations                                = std::make_shared<ScriptedOperations>();
        operations->options.enable_privilege_hardening = nullptr;
        ProcessTestAccess::set_operations(run.process, operations);
        auto info                   = helper();
        info.prevent_privilege_gain = true;
        auto result                 = run.process.start(std::move(info));
        REQUIRE_FALSE(result);
        CHECK(result.error().code == "core.process.security_unsupported");
        CHECK(result.error().category == ErrorCategory::Unsupported);
        CHECK(result.error().detail == "hardening.unsupported:0");
        CHECK(operations->fork_calls == 0);
        CHECK(run.starts == 0);
        CHECK(run.finishes == 0);
    }

    SECTION("available hardening failure prevents target execution")
    {
        Run                          run;
        auto                         operations = std::make_shared<ScriptedOperations>();
        jb::test::TemporaryDirectory directory;
        auto const                   marker            = directory.path() / "executed";
        operations->options.enable_privilege_hardening = failed_privilege_hardening;
        ProcessTestAccess::set_operations(run.process, operations);
        auto info                   = helper({"marker", marker.string()});
        info.prevent_privilege_gain = true;
        auto const result           = run.execute(std::move(info));
        REQUIRE(result.start_error);
        CHECK(result.start_error->code == "core.process.security_failed");
        CHECK(result.start_error->category == ErrorCategory::PermissionDenied);
        CHECK(result.start_error->detail == "child.hardening:" + std::to_string(EPERM));
        CHECK_FALSE(std::filesystem::exists(marker));
        CHECK(run.starts == 0);
    }
}

TEST_CASE("POSIX Process cancellation preserves its first cause and escalates after complete grace",
          "[core][process][posix][lifecycle]")
{
    SECTION("TERM-handling target remains owned until group cleanup")
    {
        Run        run;
        auto       operations = std::make_shared<ScriptedOperations>();
        auto const base       = Clock::now();
        operations->now       = base;
        ProcessTestAccess::set_operations(run.process, operations);
        auto info              = helper({"term", "0"});
        info.termination_grace = 10s;
        REQUIRE(run.process.start(std::move(info)));
        run.until([&] { return run.starts == 1 && run.bytes[0] == ByteBuffer{std::byte{'R'}}; });

        REQUIRE(run.process.stop(ProcessStopReason::Cancelled));
        CHECK(run.process.state() == ProcessState::Stopping);
        REQUIRE(run.process.stop(ProcessStopReason::Cancelled));
        auto conflict = run.process.stop(ProcessStopReason::Interrupted);
        REQUIRE_FALSE(conflict);
        CHECK(conflict.error().code == "core.process.stop_conflict");
        run.until([&] { return EventLoopTestAccess::active_process_count(*run.loop) == 0; }, EventFlag::Watchers);
        CHECK(run.process.state() == ProcessState::Stopping);
        CHECK(run.process.process_id());
        CHECK(run.finishes == 0);
        CHECK(group_signal_count(*operations, SIGTERM) == 1);
        CHECK(group_signal_count(*operations, SIGKILL) == 0);

        operations->now = base + 10s;
        EventLoopTestAccess::fire_timers(*run.loop, *operations->now);
        REQUIRE(run.result);
        CHECK(run.result->kind == ProcessExitKind::Cancelled);
        CHECK(run.result->exit_code == 42);
        CHECK(run.bytes[0] == ByteBuffer{std::byte{'R'}, std::byte{'T'}});
        CHECK(group_signal_count(*operations, SIGKILL) == 1);
        CHECK(std::ranges::count(operations->wait_options, 0) == 1);
    }

    SECTION("Ignored TERM reaches KILL and explicit delivery failure is retryable")
    {
        Run  run;
        auto operations = std::make_shared<ScriptedOperations>();
        operations->group_signal_errors.push_back(EPERM);
        ProcessTestAccess::set_operations(run.process, operations);
        auto info              = helper({"term", "1"});
        info.termination_grace = 10s;
        REQUIRE(run.process.start(std::move(info)));
        run.until([&] { return run.bytes[0] == ByteBuffer{std::byte{'R'}}; });

        auto rejected = run.process.stop();
        REQUIRE_FALSE(rejected);
        CHECK(rejected.error().code == "core.process.signal_failed");
        CHECK(run.process.state() == ProcessState::Running);
        REQUIRE(run.process.stop());
        CHECK(run.process.state() == ProcessState::Stopping);
        EventLoopTestAccess::fire_timers(*run.loop, TimePoint::max());
        run.until([&] { return run.result.has_value(); });
        CHECK(run.result->kind == ProcessExitKind::Cancelled);
        CHECK(run.result->signal_number == SIGKILL);
        CHECK(group_signal_count(*operations, SIGTERM) == 2);
        CHECK(group_signal_count(*operations, SIGKILL) == 1);
    }

    SECTION("ESRCH accepts the stop transition")
    {
        Run  run;
        auto operations = std::make_shared<ScriptedOperations>();
        operations->group_signal_errors.push_back(ESRCH);
        ProcessTestAccess::set_operations(run.process, operations);
        auto info              = helper({"term", "1"});
        info.termination_grace = Duration::zero();
        REQUIRE(run.process.start(std::move(info)));
        run.until([&] { return run.bytes[0] == ByteBuffer{std::byte{'R'}}; });
        REQUIRE(run.process.stop());
        CHECK(run.process.state() == ProcessState::Stopping);
        run.until([&] { return run.result.has_value(); });
        CHECK(run.result->kind == ProcessExitKind::Cancelled);
        CHECK(run.result->signal_number == SIGKILL);
    }
}

TEST_CASE("POSIX Process timeout starts at the accepted launch deadline", "[core][process][posix][lifecycle]")
{
    SECTION("Runtime timeout shares TERM-to-KILL escalation")
    {
        Run        run;
        auto       operations = std::make_shared<ScriptedOperations>();
        auto const base       = Clock::now();
        operations->now       = base;
        ProcessTestAccess::set_operations(run.process, operations);
        auto info              = helper({"term", "1"});
        info.timeout           = 5s;
        info.termination_grace = 2s;
        REQUIRE(run.process.start(std::move(info)));
        run.until([&] { return run.bytes[0] == ByteBuffer{std::byte{'R'}}; });

        operations->now = base + 5s;
        EventLoopTestAccess::fire_timers(*run.loop, *operations->now);
        CHECK(run.process.state() == ProcessState::Stopping);
        auto conflict = run.process.stop();
        REQUIRE_FALSE(conflict);
        CHECK(conflict.error().code == "core.process.stop_conflict");
        CHECK(group_signal_count(*operations, SIGTERM) == 1);

        operations->now = base + 7s;
        EventLoopTestAccess::fire_timers(*run.loop, *operations->now);
        run.until([&] { return run.result.has_value(); });
        CHECK(run.result->kind == ProcessExitKind::TimedOut);
        CHECK(run.result->signal_number == SIGKILL);
        CHECK(group_signal_count(*operations, SIGKILL) == 1);
    }

    SECTION("Timeout kills a target stopped in pre-exec setup")
    {
        Run        run;
        auto       operations                  = std::make_shared<ScriptedOperations>();
        auto const base                        = Clock::now();
        operations->now                        = base;
        operations->options.signal_before_exec = SIGSTOP;
        ProcessTestAccess::set_operations(run.process, operations);
        auto info              = helper();
        info.timeout           = 5s;
        info.termination_grace = Duration::zero();
        REQUIRE(run.process.start(std::move(info)));
        observe_stop(operations->observed_pid);
        run.expected_started_state = ProcessState::Stopping;
        operations->now            = base + 5s;
        EventLoopTestAccess::fire_timers(*run.loop, *operations->now);
        run.until([&] { return run.result.has_value(); });
        CAPTURE(run.result->exit_code.value_or(-1), run.result->signal_number.value_or(-1));
        CHECK(run.starts == 1); // Gate release makes clean EOF observable even though the target never execs.
        CHECK(run.result->kind == ProcessExitKind::TimedOut);
        // A stopped child may terminate from TERM before the immediate KILL attempt on macOS. The observed native
        // signal is diagnostic metadata; timeout classification and both group-signal attempts remain mandatory.
        CHECK((run.result->signal_number == SIGTERM || run.result->signal_number == SIGKILL));
        CHECK(group_signal_count(*operations, SIGTERM) == 1);
        CHECK(group_signal_count(*operations, SIGKILL) == 1);
    }

    SECTION("Expiry during parent setup accepts a gated timeout without target execution")
    {
        jb::test::TemporaryDirectory directory;
        auto const                   marker = directory.path() / "executed";
        Run                          run;
        auto                         operations = std::make_shared<ScriptedOperations>();
        auto const                   base       = TimePoint{Duration{1000}};
        operations->now_values                  = {base, base + 5s};
        ProcessTestAccess::set_operations(run.process, operations);
        auto info    = helper({"marker", marker.string()});
        info.timeout = 5s;
        REQUIRE(run.process.start(std::move(info)));
        CHECK(run.process.state() == ProcessState::Stopping);
        CHECK(operations->send_calls == 0);
        CHECK(run.starts == 0);
        CHECK(group_signal_count(*operations, SIGTERM) == 0);
        CHECK(group_signal_count(*operations, SIGKILL) == 1);
        run.until([&] { return run.result.has_value(); });
        CHECK(run.result->kind == ProcessExitKind::TimedOut);
        CHECK_FALSE(std::filesystem::exists(marker));
    }

    SECTION("Rejected gate release cancels its armed timeout")
    {
        Run  run;
        auto operations     = std::make_shared<ScriptedOperations>();
        operations->failure = ScriptedOperations::Failure::Send;
        operations->now     = Clock::now();
        ProcessTestAccess::set_operations(run.process, operations);
        auto info    = helper();
        info.timeout = 30s;
        auto result  = run.process.start(std::move(info));
        REQUIRE_FALSE(result);
        CHECK(result.error().code == "core.process.child_setup_failed");
        CHECK(EventLoopTestAccess::active_timer_count(*run.loop) == 0);
        CHECK(run.process.state() == ProcessState::NotRunning);
        check_reaped(operations->observed_pid);
    }
}

TEST_CASE("POSIX Process owns same-group descendants through every leader outcome", "[core][process][posix][lifecycle]")
{
    SECTION("Natural leader exit kills the group before reap")
    {
        PostExecChannel report;
        Run             run;
        auto            operations = std::make_shared<ScriptedOperations>();
        ProcessTestAccess::set_operations(run.process, operations);
        REQUIRE(run.process.start(helper({"group-exit", report.path()})));
        std::array<pid_t, 2> identities{};
        report.read(identities.data(), sizeof(identities));
        ProcessTerminationWatch descendant{identities[1]};
        run.until([&] { return run.result.has_value(); });
        CHECK(run.result->kind == ProcessExitKind::Exited);
        CHECK(run.result->exit_code == 37);
        descendant.check_terminated();
        auto const kill = std::ranges::find(operations->lifecycle_calls, 'K');
        auto const wait = std::ranges::find(operations->lifecycle_calls, 'W');
        REQUIRE(kill != operations->lifecycle_calls.end());
        REQUIRE(wait != operations->lifecycle_calls.end());
        CHECK(kill < wait);
    }

    SECTION("Zero-grace cancellation kills leader and descendant")
    {
        PostExecChannel report;
        Run             run;
        auto            operations = std::make_shared<ScriptedOperations>();
        ProcessTestAccess::set_operations(run.process, operations);
        auto info              = helper({"group-wait", report.path()});
        info.termination_grace = Duration::zero();
        REQUIRE(run.process.start(std::move(info)));
        std::array<pid_t, 2> identities{};
        report.read(identities.data(), sizeof(identities));
        ProcessTerminationWatch descendant{identities[1]};
        run.expected_started_state = ProcessState::Stopping;
        REQUIRE(run.process.stop());
        run.until([&] { return run.result.has_value(); });
        CHECK(run.result->kind == ProcessExitKind::Cancelled);
        CHECK(run.result->signal_number == SIGKILL);
        descendant.check_terminated();
    }

    SECTION("Zero-grace timeout kills leader and descendant")
    {
        PostExecChannel report;
        Run             run;
        auto            operations = std::make_shared<ScriptedOperations>();
        auto const      base       = Clock::now();
        operations->now            = base;
        ProcessTestAccess::set_operations(run.process, operations);
        auto info              = helper({"group-wait", report.path()});
        info.timeout           = 5s;
        info.termination_grace = Duration::zero();
        REQUIRE(run.process.start(std::move(info)));
        std::array<pid_t, 2> identities{};
        report.read(identities.data(), sizeof(identities));
        ProcessTerminationWatch descendant{identities[1]};
        run.expected_started_state = ProcessState::Stopping;
        operations->now            = base + 5s;
        EventLoopTestAccess::fire_timers(*run.loop, *operations->now);
        run.until([&] { return run.result.has_value(); });
        CHECK(run.result->kind == ProcessExitKind::TimedOut);
        descendant.check_terminated();
    }

    SECTION("Destructor kills and reaps without completion")
    {
        auto                   loop = std::make_unique<EventLoop>();
        ScopedCurrentEventLoop current{loop.get()};
        PostExecChannel        report;
        Object                 receiver;
        auto                   operations = std::make_shared<ScriptedOperations>();
        auto*                  process    = new Process;
        int                    finishes{0};
        auto                   finished = process->finished.connect(&receiver, [&](ProcessExit const&) { ++finishes; });
        ProcessTestAccess::set_operations(*process, operations);
        REQUIRE(process->start(helper({"group-wait", report.path()})));
        std::array<pid_t, 2> identities{};
        report.read(identities.data(), sizeof(identities));
        ProcessTerminationWatch descendant{identities[1]};
        delete process;
        CHECK(finishes == 0);
        check_reaped(identities[0]);
        descendant.check_terminated();
    }
}

TEST_CASE("POSIX Process retains an exited stopping leader until group grace expires",
          "[core][process][posix][lifecycle]")
{
    for (bool descendant_ignores_term : {false, true}) {
        CAPTURE(descendant_ignores_term);
        PostExecChannel report;
        Run             run;
        auto            operations = std::make_shared<ScriptedOperations>();
        auto const      base       = Clock::now();
        operations->now            = base;
        ProcessTestAccess::set_operations(run.process, operations);
        auto info              = helper({"group", report.path(), descendant_ignores_term ? "1" : "0", "0"});
        info.termination_grace = 10s;
        REQUIRE(run.process.start(std::move(info)));
        std::array<pid_t, 2> identities{};
        report.read(identities.data(), sizeof(identities));
        ProcessTerminationWatch descendant{identities[1]};
        run.expected_started_state = ProcessState::Stopping;
        REQUIRE(run.process.stop());

        run.until([&] { return EventLoopTestAccess::active_process_count(*run.loop) == 0; }, EventFlag::Watchers);
        CHECK(run.process.state() == ProcessState::Stopping);
        CHECK(run.process.process_id() == identities[0]);
        CHECK(EventLoopTestAccess::active_process_count(*run.loop) == 0);
        observe_exit(identities[0]);
        CHECK(run.finishes == 0);
        CHECK(group_signal_count(*operations, SIGKILL) == 0);
        if (!descendant_ignores_term) {
            char marker{};
            report.read(&marker, 1);
            CHECK(marker == 'C');
        }

        operations->now = base + 10s;
        EventLoopTestAccess::fire_timers(*run.loop, *operations->now);
        run.until([&] { return run.result.has_value(); });
        CHECK(run.result->kind == ProcessExitKind::Cancelled);
        CHECK(run.result->exit_code == 42);
        CHECK(group_signal_count(*operations, SIGKILL) == 1);
        CHECK(std::ranges::count(operations->wait_options, 0) == 1);
        descendant.check_terminated();
    }
}

TEST_CASE("POSIX Process bounds post-reap output retained by escaped descendants",
          "[core][process][posix][lifecycle][output]")
{
    constexpr std::size_t budget{std::size_t{256} * 1024};
    for (std::size_t channel = 0; channel < 2; ++channel) {
        for (bool continuous : {false, true}) {
            CAPTURE(channel, continuous);
            PostExecChannel report;
            auto            backend = std::make_unique<FakeEventLoopBackend>();
            auto*           fake    = backend.get();
            Run             run{std::move(backend)};
            auto            operations = std::make_shared<OutputOperations>();
            ProcessTestAccess::set_operations(run.process, operations);
            REQUIRE(
                run.process.start(helper({"escape", report.path(), std::to_string(channel), continuous ? "1" : "0"})));
            pid_t escaped{-1};
            report.read(&escaped, sizeof(escaped));
            REQUIRE(escaped > 0);
            ProcessTerminationWatch descendant{escaped};

            observe_exit(operations->observed_pid);
            fake->ready_events = {
                {.kind = ReadyEventKind::Process, .ident = operations->observed_pid}
            };
            REQUIRE(run.loop->process_events(EventFlag::Watchers, 0) != ProcessEventsResult::Failed);
            CHECK(run.process.state() == ProcessState::Finishing);
            CHECK_FALSE(run.process.process_id());
            CHECK(run.finishes == 0);
            auto const bytes_before = operations->read_bytes[channel];
            auto const stale        = EventLoopTestAccess::fd_callback(*run.loop, operations->output_fds[channel]);
            REQUIRE(stale);

            EventLoopTestAccess::fire_timers(*run.loop, TimePoint::max());
            REQUIRE(run.result);
            CHECK(run.result->kind == ProcessExitKind::Exited);
            CHECK(run.result->exit_code == 37);
            CHECK(run.result->stdout_lost == (channel == 0));
            CHECK(run.result->stderr_lost == (channel == 1));
            CHECK(operations->read_bytes[channel] - bytes_before <= budget);
            auto const reads = operations->read_calls;
            stale(operations->output_fds[channel], FdEvent::Read);
            REQUIRE(run.loop->process_events(EventFlag::Events, 0) != ProcessEventsResult::Failed);
            CHECK(operations->read_calls == reads);

            REQUIRE((::kill(escaped, SIGKILL) == 0 || errno == ESRCH));
            descendant.check_terminated();
        }
    }
}

TEST_CASE("POSIX Process keeps a reentrant post-reap deadline within the active output budget",
          "[core][process][posix][lifecycle][output]")
{
    constexpr std::size_t budget{std::size_t{256} * 1024};
    PostExecChannel       report;
    auto                  backend = std::make_unique<FakeEventLoopBackend>();
    auto*                 fake    = backend.get();
    Run                   run{std::move(backend)};
    auto                  operations = std::make_shared<OutputOperations>();
    ProcessTestAccess::set_operations(run.process, operations);
    REQUIRE(run.process.start(helper({"escape", report.path(), "0", "0"})));

    pid_t escaped{-1};
    report.read(&escaped, sizeof(escaped));
    REQUIRE(escaped > 0);
    ProcessTerminationWatch descendant{escaped};
    observe_exit(operations->observed_pid);
    fake->ready_events = {
        {.kind = ReadyEventKind::Process, .ident = operations->observed_pid}
    };
    REQUIRE(run.loop->process_events(EventFlag::Watchers, 0) != ProcessEventsResult::Failed);
    REQUIRE(run.process.state() == ProcessState::Finishing);

    operations->synthetic_remaining[0] = 2 * budget;
    auto const signals_before          = operations->signal_calls.size();
    bool       deadline_fired{false};
    auto       reenter = run.process.standard_output.connect(&run.receiver, [&](ByteBuffer const&) {
        if (deadline_fired) {
            return;
        }

        deadline_fired = true;
        CHECK(run.process.state() == ProcessState::Finishing);
        CHECK_FALSE(run.process.process_id());
        auto stopped = run.process.stop();
        CHECK_FALSE(stopped);
        if (!stopped) {
            CHECK(stopped.error().code == "core.process.invalid_state");
        }
        CHECK(operations->signal_calls.size() == signals_before);
        EventLoopTestAccess::fire_timers(*run.loop, TimePoint::max());
    });

    auto const fd             = operations->output_fds[0];
    auto const stale          = EventLoopTestAccess::fd_callback(*run.loop, fd);
    auto const wakeups_before = fake->wakeup_calls;
    REQUIRE(stale);
    fake->ready_events = {
        {.ident = fd, .events = FdEvent::Read}
    };
    REQUIRE(run.loop->process_events(EventFlag::Watchers, 0) != ProcessEventsResult::Failed);

    REQUIRE(deadline_fired);
    REQUIRE(run.result);
    CHECK(run.bytes[0].size() == budget);
    CHECK(operations->synthetic_remaining[0] == budget);
    CHECK(fake->wakeup_calls == wakeups_before);
    CHECK(operations->signal_calls.size() == signals_before);
    CHECK(run.result->stdout_lost);
    CHECK_FALSE(run.result->stderr_lost);
    auto const reads = operations->read_calls;
    stale(fd, FdEvent::Read);
    REQUIRE(run.loop->process_events(EventFlag::Events, 0) != ProcessEventsResult::Failed);
    CHECK(operations->read_calls == reads);

    REQUIRE((::kill(escaped, SIGKILL) == 0 || errno == ESRCH));
    descendant.check_terminated();
}

TEST_CASE("POSIX Process destruction in Finishing never signals the former process group",
          "[core][process][posix][lifecycle]")
{
    PostExecChannel        report;
    auto                   backend = std::make_unique<FakeEventLoopBackend>();
    auto*                  fake    = backend.get();
    auto                   loop    = EventLoopTestAccess::make_event_loop(std::move(backend));
    ScopedCurrentEventLoop current{loop.get()};
    Object                 receiver;
    auto                   operations = std::make_shared<OutputOperations>();
    auto*                  process    = new Process;
    int                    finishes{0};
    auto                   finished = process->finished.connect(&receiver, [&](ProcessExit const&) { ++finishes; });
    ProcessTestAccess::set_operations(*process, operations);
    REQUIRE(process->start(helper({"escape", report.path(), "1", "0"})));

    pid_t escaped{-1};
    report.read(&escaped, sizeof(escaped));
    REQUIRE(escaped > 0);
    ProcessTerminationWatch descendant{escaped};
    observe_exit(operations->observed_pid);
    fake->ready_events = {
        {.kind = ReadyEventKind::Process, .ident = operations->observed_pid}
    };
    REQUIRE(loop->process_events(EventFlag::Watchers, 0) != ProcessEventsResult::Failed);
    CHECK(process->state() == ProcessState::Finishing);
    CHECK_FALSE(process->process_id());
    CHECK(finishes == 0);

    auto const signals_before = operations->signal_calls.size();
    delete process;
    CHECK(operations->signal_calls.size() == signals_before);
    CHECK(finishes == 0);

    REQUIRE((::kill(escaped, SIGKILL) == 0 || errno == ESRCH));
    descendant.check_terminated();
}

TEST_CASE("POSIX Process streams binary channels beyond pipe capacity and preserves both tails",
          "[core][process][posix][output]")
{
    for (auto sizes : {
             std::array{0,       0      },
             std::array{1,       1      },
             std::array{1048593, 1049079},
             std::array{0,       1048607}
    }) {
        CAPTURE(sizes);
        Run  run;
        auto operations = std::make_shared<OutputOperations>();
        ProcessTestAccess::set_operations(run.process, operations);
        auto       finish = run.process.finished.connect(&run.receiver, [&](ProcessExit const&) {
            CHECK(operations->eof == std::array{true, true});
        });
        auto const result = run.execute(helper({"output", std::to_string(sizes[0]), std::to_string(sizes[1])}));
        CHECK(result.exit_code == 37);
        CHECK_FALSE(result.stdout_lost);
        CHECK_FALSE(result.stderr_lost);
        for (std::size_t i = 0; i < sizes.size(); ++i) {
            CHECK(run.bytes[i].size() == static_cast<std::size_t>(sizes[i]));
            CHECK(matches_pattern(run.bytes[i], i));
        }
        CHECK(operations->max_request <= std::size_t{64} * 1024);
    }
}

TEST_CASE("POSIX Process rolls back each partial output pipe and watch setup", "[core][process][posix][output]")
{
    for (int failure = 0; failure < 6; ++failure) {
        CAPTURE(failure);
        auto  backend = std::make_unique<FakeEventLoopBackend>();
        auto* fake    = backend.get();
        Run   run{std::move(backend)};
        auto  operations = std::make_shared<ScriptedOperations>();
        if (failure < 3) {
            operations->fail_pipe_call = failure + 1;
        }
        else {
            fake->add_fd_results = std::deque<bool>(static_cast<std::size_t>(failure - 3), true);
            fake->add_fd_results.push_back(false);
        }
        ProcessTestAccess::set_operations(run.process, operations);
        auto const descriptors = descriptor_count();
        auto       result      = run.process.start(helper());
        REQUIRE_FALSE(result);
        CHECK(result.error().code ==
              (failure < 3 ? "core.process.resource_setup_failed" : "core.process.watch_failed"));
        CHECK(operations->send_calls == 0);
        CHECK(run.process.state() == ProcessState::NotRunning);
        CHECK(EventLoopTestAccess::active_process_count(*run.loop) == 0);
        CHECK_FALSE(EventLoopTestAccess::fd_callback(*run.loop, operations->status_fd));
        for (auto fd : operations->output_fds) {
            CHECK_FALSE(EventLoopTestAccess::fd_callback(*run.loop, fd));
        }
        CHECK(descriptor_count() == descriptors);
        CHECK(run.loop->process_events(EventFlag::All, 0) != ProcessEventsResult::Failed);
        CHECK(run.starts == 0);
        CHECK(run.finishes == 0);
        if (failure < 3) {
            CHECK(operations->fork_calls == 0);
        }
        else {
            check_reaped(operations->observed_pid);
        }
    }
}

TEST_CASE("POSIX Process keeps the other channel alive after early EOF", "[core][process][posix][output]")
{
    for (std::size_t closed = 0; closed < 2; ++closed) {
        PostExecChannel gate;
        Run             run;
        auto            operations = std::make_shared<OutputOperations>();
        ProcessTestAccess::set_operations(run.process, operations);
        REQUIRE(run.process.start(helper({"early", std::to_string(closed), gate.path()})));
        auto const pid  = operations->observed_pid;
        auto const open = 1 - closed;
        run.until([&] { return operations->eof[closed] && run.bytes[open].size() == 1; });
        CHECK_FALSE(operations->eof[open]);
        CHECK(run.finishes == 0);
        CHECK(run.process.state() == ProcessState::Running);
        gate.permit();
        run.until([&] { return run.finishes == 1; });
        CHECK(run.result->exit_code == 37);
        CHECK(run.bytes[closed].empty());
        CHECK(run.bytes[open].size() == 4097);
        CHECK(matches_pattern(run.bytes[open], open));
        check_reaped(pid);
    }
}

TEST_CASE("POSIX Process reap waits for independently controlled output terminals", "[core][process][posix][output]")
{
    auto  backend = std::make_unique<FakeEventLoopBackend>();
    auto* fake    = backend.get();
    Run   run{std::move(backend)};
    auto  operations = std::make_shared<OutputOperations>();
    operations->hold = {true, true};
    ProcessTestAccess::set_operations(run.process, operations);
    REQUIRE(run.process.start(helper({"output", "11", "17"})));
    auto const pid = operations->observed_pid;
    observe_exit(pid);
    fake->ready_events = {
        {.ident = operations->output_fds[0], .events = FdEvent::Read},
        {.kind = ReadyEventKind::Process,    .ident = pid           },
        {.ident = operations->output_fds[1], .events = FdEvent::Read}
    };
    CHECK(run.loop->process_events(EventFlag::Watchers, 0) != ProcessEventsResult::Failed);
    CHECK(run.starts == 1);
    CHECK(run.finishes == 0);
    CHECK(run.process.state() == ProcessState::Finishing);
    CHECK_FALSE(run.process.process_id());
    check_reaped(pid);
    auto inspect = [&](ByteBuffer const&) {
        CHECK(run.process.state() == ProcessState::Finishing);
        CHECK_FALSE(run.process.process_id());
        CHECK_FALSE(run.process.start(helper()));
        CHECK_FALSE(run.process.stop());
    };
    auto output = run.process.standard_output.connect(&run.receiver, inspect);
    auto error  = run.process.standard_error.connect(&run.receiver, inspect);
    for (std::size_t i = 0; i < 2; ++i) {
        operations->hold[i] = false;
        fake->ready_events  = {
            {.ident = operations->output_fds[i], .events = FdEvent::Read}
        };
        CHECK(run.loop->process_events(EventFlag::Watchers, 0) != ProcessEventsResult::Failed);
        CHECK(operations->eof[i]);
        CHECK(run.finishes == static_cast<int>(i));
    }
    CHECK(run.bytes[0].size() == 11);
    CHECK(run.bytes[1].size() == 17);
    CHECK(matches_pattern(run.bytes[0], 0));
    CHECK(matches_pattern(run.bytes[1], 1));
    CHECK(run.result->exit_code == 37);
}

TEST_CASE("POSIX Process retries interrupted reads and coalesces budget continuations without another edge",
          "[core][process][posix][output]")
{
    constexpr std::size_t budget{std::size_t{256} * 1024};
    auto                  backend = std::make_unique<FakeEventLoopBackend>();
    auto*                 fake    = backend.get();
    Run                   run{std::move(backend)};
    auto                  operations   = std::make_shared<OutputOperations>();
    operations->read_errors[0]         = {EINTR, EAGAIN};
    operations->synthetic_remaining[0] = (3 * budget) + 31;
    ProcessTestAccess::set_operations(run.process, operations);
    REQUIRE(run.process.start(helper()));
    auto const pid = operations->observed_pid;
    observe_exit(pid);
    auto const fd      = operations->output_fds[0];
    fake->ready_events = {
        {.ident = fd, .events = FdEvent::Read}
    };
    CHECK(run.loop->process_events(EventFlag::Watchers, 0) != ProcessEventsResult::Failed);
    CHECK(run.starts == 1);
    CHECK(operations->read_calls[0] == 2);
    CHECK(run.bytes[0].empty());
    CHECK(run.loop->process_events(EventFlag::Events, 0) != ProcessEventsResult::Failed);
    CHECK(operations->read_calls[0] == 2); // EAGAIN waits for readiness rather than spinning a continuation.
    fake->ready_events = {
        {.ident = fd,                     .events = FdEvent::Read},
        {.ident = fd,                     .events = FdEvent::Read},
        {.kind = ReadyEventKind::Process, .ident = pid           }
    };
    CHECK(run.loop->process_events(EventFlag::Watchers, 0) != ProcessEventsResult::Failed);
    CHECK(run.bytes[0].size() == budget);
    CHECK(operations->read_calls[0] == 6);
    CHECK(run.process.state() == ProcessState::Finishing);
    CHECK(run.finishes == 0);
    for (std::size_t cycle = 2; cycle <= 3; ++cycle) {
        CHECK(run.loop->process_events(EventFlag::Events, 0) != ProcessEventsResult::Failed);
        CHECK(run.bytes[0].size() == cycle * budget);
        CHECK(run.finishes == 0);
    }
    CHECK(run.loop->process_events(EventFlag::Events, 0) != ProcessEventsResult::Failed);
    CHECK(run.bytes[0].size() == (3 * budget) + 31);
    CHECK(matches_pattern(run.bytes[0], 0));
    CHECK(operations->max_request == std::size_t{64} * 1024);
    CHECK(run.finishes == 1);
    CHECK(run.result->exit_code == 37);
    CHECK_FALSE(run.result->stdout_lost);
    CHECK_FALSE(run.result->stderr_lost);
    auto const calls = operations->read_calls;
    CHECK(run.loop->process_events(EventFlag::All, 0) != ProcessEventsResult::Failed);
    CHECK(operations->read_calls == calls);
}

TEST_CASE("POSIX Process endless native writers yield to timers descriptors and another child exit",
          "[core][process][posix][output]")
{
    constexpr std::size_t budget{std::size_t{256} * 1024};
    for (int mask : {1, 2, 3}) {
        CAPTURE(mask);
        auto  backend = std::make_unique<FakeEventLoopBackend>();
        auto* fake    = backend.get();
        Run   run{std::move(backend)};
        auto  operations = std::make_shared<OutputOperations>();
        ProcessTestAccess::set_operations(run.process, operations);
        REQUIRE(run.process.start(helper({"continuous", std::to_string(mask)})));
        for (std::size_t i = 0; i < 2; ++i) {
            operations->refill[i] = (mask & (1 << i)) != 0;
            if (operations->refill[i]) {
                pollfd item{.fd = operations->output_fds[i], .events = POLLIN, .revents = 0};
                REQUIRE(::poll(&item, 1, 3000) == 1);
                fake->ready_events.push_back({.ident = item.fd, .events = FdEvent::Read});
                fake->ready_events.push_back({.ident = item.fd, .events = FdEvent::Read});
            }
        }
        PostExecChannel unrelated;
        bool            descriptor_ready{false};
        bool            timer_ready{false};
        bool            child_finished{false};
        auto watch = run.loop->watch_fd(unrelated.descriptor(), FdEvent::Read, FdTriggerMode::Edge, [&](int, FdEvents) {
            descriptor_ready = true;
        });
        REQUIRE(watch);
        unrelated.permit();
        Process other;
        auto    finish = other.finished.connect(&run.receiver, [&](ProcessExit const& exit) {
            CHECK(exit.exit_code == 37);
            child_finished = true;
        });
        REQUIRE(other.start(helper()));
        auto const other_pid = static_cast<pid_t>(*other.process_id());
        observe_exit(other_pid);
        fake->ready_events.push_back({.ident = unrelated.descriptor(), .events = FdEvent::Read});
        fake->ready_events.push_back({.kind = ReadyEventKind::Process, .ident = other_pid});
        auto timer = run.loop->post_at(Clock::now(), [&] { timer_ready = true; });
        REQUIRE(timer);
        CHECK(run.loop->process_events(EventFlags{EventFlag::Watchers, EventFlag::Timers}, 0) !=
              ProcessEventsResult::Failed);
        CHECK(descriptor_ready);
        CHECK(timer_ready);
        CHECK(child_finished);
        check_reaped(other_pid);
        for (std::size_t cycle = 1; cycle <= 4; ++cycle) {
            if (cycle != 1) {
                CHECK(run.loop->process_events(EventFlag::Events, 0) != ProcessEventsResult::Failed);
            }
            for (std::size_t i = 0; i < 2; ++i) {
                CHECK(run.bytes[i].size() == (operations->refill[i] ? cycle * budget : 0));
                CHECK(matches_pattern(run.bytes[i], i));
            }
        }
        CHECK_FALSE(operations->refill_failed);
        CHECK(operations->max_request == std::size_t{64} * 1024);
        CHECK(run.finishes == 0);
        CHECK(run.loop->unwatch_fd(watch));
    }
}

TEST_CASE("POSIX Process failed continuation enqueue retires only its channel without stranding completion",
          "[core][process][posix][output]")
{
    constexpr std::size_t budget{std::size_t{256} * 1024};
    for (std::size_t failed = 0; failed < 2; ++failed) {
        for (bool repost : {false, true}) {
            CAPTURE(failed, repost);
            auto  backend          = std::make_unique<FakeEventLoopBackend>();
            auto* fake             = backend.get();
            fake->remove_fd_result = false;
            Run  run{std::move(backend)};
            auto operations                             = std::make_shared<OutputOperations>();
            operations->hold                            = {true, true};
            operations->synthetic_remaining[failed]     = (3 * budget) + 31;
            operations->synthetic_remaining[1 - failed] = 17;
            ProcessTestAccess::set_operations(run.process, operations);
            REQUIRE(run.process.start(helper()));
            auto const pid = operations->observed_pid;
            observe_exit(pid);
            fake->ready_events = {
                {.kind = ReadyEventKind::Process, .ident = pid}
            };
            REQUIRE(run.loop->process_events(EventFlag::Watchers, 0) != ProcessEventsResult::Failed);
            REQUIRE(run.process.state() == ProcessState::Finishing);
            check_reaped(pid);

            // Keep the other channel open so enqueue failure must retire exactly one channel before completion.
            auto const fd    = operations->output_fds[failed];
            auto const stale = EventLoopTestAccess::fd_callback(*run.loop, fd);
            REQUIRE(stale);
            operations->hold[failed] = false;
            fake->wakeup_result      = repost;
            auto const wakeups       = fake->wakeup_calls;
            fake->ready_events       = {
                {.ident = fd, .events = FdEvent::Read},
                {.ident = fd, .events = FdEvent::Read}
            };
            REQUIRE(run.loop->process_events(EventFlag::Watchers, 0) != ProcessEventsResult::Failed);
            CHECK(run.bytes[failed].size() == budget);
            if (repost) {
                fake->wakeup_result = false;
                REQUIRE(run.loop->process_events(EventFlag::Events, 0) != ProcessEventsResult::Failed);
            }
            CHECK(fake->wakeup_calls == wakeups + (repost ? 2 : 1));
            CHECK(run.bytes[failed].size() == (repost ? 2 * budget : budget));
            CHECK(matches_pattern(run.bytes[failed], failed));
            CHECK(run.finishes == 0);
            CHECK(::fcntl(fd, F_GETFD) == -1);
            CHECK(errno == EBADF);
            REQUIRE(EventLoopTestAccess::fd_callback(*run.loop, fd)); // Failed unwatch retained the inert callback.

            auto const reads = operations->read_calls;
            stale(fd, FdEvent::Read);
            fake->ready_events = {
                {.ident = fd, .events = FdEvent::Read}
            };
            REQUIRE(run.loop->process_events(EventFlag::All, 0) != ProcessEventsResult::Failed);
            CHECK(operations->read_calls == reads);
            CHECK(fake->wakeup_calls == wakeups + (repost ? 2 : 1)); // No retry or repost loop on persistent failure.

            operations->hold[1 - failed] = false;
            fake->ready_events           = {
                {.ident = operations->output_fds[1 - failed], .events = FdEvent::Read}
            };
            REQUIRE(run.loop->process_events(EventFlag::All, 0) != ProcessEventsResult::Failed);
            REQUIRE(run.result);
            CHECK(run.finishes == 1);
            CHECK(run.result->exit_code == 37);
            CHECK(run.result->stdout_lost == (failed == 0));
            CHECK(run.result->stderr_lost == (failed == 1));
            CHECK(run.bytes[1 - failed].size() == 17);
            CHECK(matches_pattern(run.bytes[1 - failed], 1 - failed));

            // Restoring wakeup and reusing the Process must not reactivate failed-generation work or loss state.
            fake->wakeup_result             = true;
            operations->synthetic_remaining = {};
            REQUIRE(run.process.start(helper()));
            auto const next_pid     = operations->observed_pid;
            auto const before_stale = operations->read_calls;
            stale(fd, FdEvent::Read);
            REQUIRE(run.loop->process_events(EventFlag::Events, 0) != ProcessEventsResult::Failed);
            CHECK(operations->read_calls == before_stale);
            CHECK(run.process.state() == ProcessState::Starting);
            observe_exit(next_pid);
            fake->ready_events = {
                {.kind = ReadyEventKind::Process, .ident = next_pid}
            };
            REQUIRE(run.loop->process_events(EventFlag::All, 0) != ProcessEventsResult::Failed);
            CHECK(run.finishes == 2);
            CHECK_FALSE(run.result->stdout_lost);
            CHECK_FALSE(run.result->stderr_lost);
            check_reaped(next_pid);
        }
    }
}

TEST_CASE("POSIX Process read failure loses only its channel and preserves leader classification",
          "[core][process][posix][output]")
{
    for (std::size_t failed = 0; failed < 2; ++failed) {
        auto  backend = std::make_unique<FakeEventLoopBackend>();
        auto* fake    = backend.get();
        Run   run{std::move(backend)};
        auto  operations                = std::make_shared<OutputOperations>();
        operations->read_errors[failed] = {EIO};
        ProcessTestAccess::set_operations(run.process, operations);
        REQUIRE(run.process.start(helper({"output", "11", "17"})));
        auto const pid = operations->observed_pid;
        observe_exit(pid);
        fake->ready_events = {
            {.kind = ReadyEventKind::Process, .ident = pid}
        };
        CHECK(run.loop->process_events(EventFlag::All, 0) != ProcessEventsResult::Failed);
        REQUIRE(run.result);
        CHECK(run.result->kind == ProcessExitKind::Exited);
        CHECK(run.result->exit_code == 37);
        CHECK(run.result->stdout_lost == (failed == 0));
        CHECK(run.result->stderr_lost == (failed == 1));
        CHECK(run.bytes[failed].empty());
        CHECK(run.bytes[1 - failed].size() == (failed == 0 ? 17 : 11));
        CHECK(matches_pattern(run.bytes[1 - failed], 1 - failed));
        check_reaped(pid);
    }
}

TEST_CASE("POSIX Process output slots defer deletion across a bounded drain and pending continuation",
          "[core][process][posix][output]")
{
    constexpr std::size_t budget{std::size_t{256} * 1024};
    for (std::size_t index = 0; index < 2; ++index) {
        auto  backend               = std::make_unique<FakeEventLoopBackend>();
        auto* fake                  = backend.get();
        fake->remove_fd_result      = false;
        auto                   loop = EventLoopTestAccess::make_event_loop(std::move(backend));
        ScopedCurrentEventLoop current{loop.get()};
        PostExecChannel        gate;
        Object                 receiver;
        auto const             descriptors = descriptor_count();
        auto*                  process     = new Process;
        bool                   destroyed{false};
        bool                   slot_returned{false};
        int                    finishes{0};
        std::size_t            bytes{0};
        auto                   operations      = std::make_shared<OutputOperations>();
        operations->synthetic_remaining[index] = 2 * budget;
        ProcessTestAccess::set_operations(*process, operations);
        auto destroy = process->destroyed.connect(&receiver, [&] { destroyed = true; });
        auto output  = (index == 0 ? process->standard_output : process->standard_error)
                           .connect(
                               &receiver,
                               [&](ByteBuffer const& chunk) {
                                  CHECK_FALSE(destroyed);
                                  CHECK(matches_pattern(chunk, index, bytes));
                                  bytes += chunk.size();
                                  process->delete_later();
                                  slot_returned = true;
                               },
                               ConnectionType::Direct);
        auto finish  = process->finished.connect(&receiver, [&](ProcessExit const&) { ++finishes; });
        REQUIRE(process->start(helper({"wait", gate.path()})));
        auto const pid = operations->observed_pid;
        pollfd     status{.fd = operations->status_fd, .events = POLLIN, .revents = 0};
        REQUIRE(::poll(&status, 1, 3000) == 1);
        auto const fd      = operations->output_fds[index];
        auto       stale   = EventLoopTestAccess::fd_callback(*loop, fd);
        fake->ready_events = {
            {.ident = fd, .events = FdEvent::Read}
        };
        CHECK(loop->process_events(EventFlag::Watchers, 0) != ProcessEventsResult::Failed);
        CHECK(slot_returned);
        CHECK_FALSE(destroyed);
        CHECK(bytes == budget); // The direct slot returns into the remaining reads before deferred deletion.
        auto const calls = operations->read_calls;
        CHECK(loop->process_events(EventFlag::Tasks, 0) != ProcessEventsResult::Failed);
        CHECK(destroyed);
        if (!destroyed) {
            delete process;
        }
        check_reaped(pid);
        stale(fd, FdEvent::Read);
        fake->ready_events = {
            {.ident = fd, .events = FdEvent::Read}
        };
        CHECK(loop->process_events(EventFlag::All, 0) != ProcessEventsResult::Failed);
        CHECK(loop->process_events(EventFlag::All, 0) != ProcessEventsResult::Failed);
        CHECK(operations->read_calls == calls);
        CHECK(finishes == 0);
        CHECK(descriptor_count() == descriptors);
    }
}
