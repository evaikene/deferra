#include "process.hpp"

#include "event_loop.hpp"
#include "event_loop_backend.hpp"
#include "event_loop_backend_epoll_priv.hpp"
#include "process_posix_priv.hpp"
#include "support/fake_event_loop_backend.hpp"
#include "support/temporary_directory.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cerrno>
#include <csignal> // IWYU pragma: keep Provides POSIX signal sets and dispositions.
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <pthread.h>
#include <sys/epoll.h>
#include <sys/socket.h>
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
    return static_cast<std::size_t>(
        std::distance(std::filesystem::directory_iterator{"/proc/self/fd"}, std::filesystem::directory_iterator{}));
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
            CHECK(process.state() == ProcessState::Running);
            CHECK(EventLoop::current() == loop.get());
        });
        auto finished = process.finished.connect(&receiver, [this](ProcessExit const& exit) {
            ++finishes;
            result = exit;
            CHECK(process.state() == ProcessState::NotRunning);
            CHECK_FALSE(process.process_id());
            CHECK(EventLoop::current() == loop.get());
        });
        auto output   = process.standard_output.connect(&receiver, [](ByteBuffer const&) { FAIL("Stage 6.3 output"); });
        auto error    = process.standard_error.connect(&receiver, [](ByteBuffer const&) { FAIL("Stage 6.3 output"); });
    }

    /// @throws Catch::TestFailureException when readiness fails or the watchdog expires.
    void until(std::function<bool()> const& predicate) const
    {
        auto const deadline = Clock::now() + 3s;
        while (!predicate() && Clock::now() < deadline) {
            REQUIRE(loop->process_events(EventFlag::All, 10) != ProcessEventsResult::Failed);
        }
        REQUIRE(predicate());
    }

    /// @throws Catch::TestFailureException when launch, completion, or ownership assertions fail.
    auto execute(ProcessStartInfo info = helper()) -> ProcessExit
    {
        auto const previous_starts   = starts;
        auto const previous_finishes = finishes;
        result.reset();
        REQUIRE(process.start(std::move(info)));
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
    Failure               failure{Failure::None};
    ProcessChildOptions   options;
    pid_t                 observed_pid{-1};
    int                   fork_calls{0};
    int                   send_calls{0};
    int                   status_fd{-1};
    EventLoop*            loop{nullptr};
    bool                  watches_before_release{false};
    bool                  blocked_at_creation{false};
    bool                  restored_at_release{false};
    bool                  send_interrupted{false};
    std::function<void()> before_send;

    auto open_null(int flags) noexcept -> int override
    {
        if (failure == Failure::Open) {
            errno = EMFILE;
            return -1;
        }
        return ProcessOperations::open_null(flags);
    }

    auto make_pipe(int* pair) noexcept -> int override
    {
        if (failure == Failure::Pipe) {
            errno = EMFILE;
            return -1;
        }
        auto const result = ProcessOperations::make_pipe(pair);
        if (result == 0) {
            status_fd = pair[0];
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
        blocked_at_creation = ::sigismember(&mask, SIGHUP) == 1 && ::sigismember(&mask, SIGUSR1) == 1;
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
        restored_at_release = ::sigismember(&mask, SIGHUP) == 0;
        if (loop) {
            watches_before_release =
                EventLoopTestAccess::fd_callback(*loop, status_fd) && EventLoopTestAccess::process_callback(*loop, pid);
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
    REQUIRE(::sigemptyset(&action.sa_mask) == 0);
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
} // namespace

TEST_CASE("Linux Process executes absolute and ordered PATH candidates without missing immediate exits",
          "[core][process][linux]")
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

TEST_CASE("Linux Process reports signal and asynchronous exec or directory failures safely", "[core][process][linux]")
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

TEST_CASE("Linux Process PATH remembers permission denial but continues to a usable candidate",
          "[core][process][linux]")
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

TEST_CASE("Linux Process installs exact argv environment cwd and clean target signal state", "[core][process][linux]")
{
    SignalScope hup{SIGHUP};
    SignalScope usr{SIGUSR1};
    install_handler(SIGHUP, record_signal);
    install_handler(SIGUSR1, SIG_IGN);
    sigset_t blocked{};
    REQUIRE(::sigemptyset(&blocked) == 0);
    REQUIRE(::sigaddset(&blocked, SIGUSR1) == 0);
    REQUIRE(::pthread_sigmask(SIG_BLOCK, &blocked, nullptr) == 0);
    jb::test::TemporaryDirectory directory;
    Run                          run;
    auto                         info  = helper({"inspect", "", "-option", directory.path().string()});
    info.working_directory             = directory.path();
    info.environment["PROCESS_MARKER"] = "literal $x = value";
    CHECK(run.execute(std::move(info)).exit_code == 0);
    sigset_t after{};
    REQUIRE(::pthread_sigmask(SIG_SETMASK, nullptr, &after) == 0);
    CHECK(::sigismember(&after, SIGUSR1) == 1);
}

TEST_CASE("Linux Process normalizes closed conventional descriptors and bypasses atfork handlers",
          "[core][process][linux]")
{
    Run run;
    for (int mask : {1, 2, 4, 8, 15}) {
        for (bool missing : {false, true}) {
            CHECK(run.execute(helper({"closed", std::to_string(mask), missing ? "1" : "0"})).exit_code == 0);
        }
    }
    CHECK(run.execute(helper({"atfork"})).exit_code == 0);
}

TEST_CASE("Linux Process gates execution on watch acceptance and restores parent masks", "[core][process][linux]")
{
    SignalScope mask{SIGHUP};
    sigset_t    empty{};
    REQUIRE(::sigemptyset(&empty) == 0);
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

TEST_CASE("Linux Process rejects parent setup faults without signals descriptors or zombies", "[core][process][linux]")
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
            CHECK(::sigismember(&before, signal) == ::sigismember(&after, signal));
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

TEST_CASE("Linux Process unwinds parent setup without releasing the target or retaining ownership",
          "[core][process][linux]")
{
    Run  run;
    auto operations         = std::make_shared<ScriptedOperations>();
    operations->before_send = [] { throw std::runtime_error{"parent setup probe"}; };
    ProcessTestAccess::set_operations(run.process, operations);
    jb::test::TemporaryDirectory directory;
    auto const                   marker      = directory.path() / "executed";
    auto const                   descriptors = descriptor_count();

    // Inject a non-allocation exception after both watches exist; never catch or simulate std::bad_alloc recovery.
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

TEST_CASE("Linux Process child failures map safe setup and authoritative identity stages", "[core][process][linux]")
{
    Run  run;
    auto operations = std::make_shared<ScriptedOperations>();
    ProcessTestAccess::set_operations(run.process, operations);
    for (auto stage : {ProcessChildStage::Group, ProcessChildStage::Descriptors, ProcessChildStage::Signals}) {
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

TEST_CASE("Linux Process inherited handlers cannot run before reset and pre-exec signal death is observable",
          "[core][process][linux]")
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

TEST_CASE("Linux Process dead gate neither generates SIGPIPE nor consumes host pending signals",
          "[core][process][linux]")
{
    SignalScope scope{SIGPIPE};
    install_handler(SIGPIPE, record_signal);
    sigset_t pipe_signal{};
    REQUIRE(::sigemptyset(&pipe_signal) == 0);
    REQUIRE(::sigaddset(&pipe_signal, SIGPIPE) == 0);
    for (int pending_mode : {0, 1, 2}) {
        CAPTURE(pending_mode);
        signal_hits = 0;
        REQUIRE(::pthread_sigmask(pending_mode == 0 ? SIG_UNBLOCK : SIG_BLOCK, &pipe_signal, nullptr) == 0);
        Run  run;
        auto operations     = std::make_shared<ScriptedOperations>();
        operations->failure = ScriptedOperations::Failure::DeadChild;
        auto const owner    = ::pthread_self();
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
        CHECK(::sigismember(&pending, SIGPIPE) == (pending_mode == 0 ? 0 : 1));
        if (pending_mode != 0) {
            int signal{};
            // The test consumes only the host signal it deliberately queued, before restoring the original mask.
            REQUIRE(::sigwait(&pipe_signal, &signal) == 0);
            CHECK(signal == SIGPIPE);
        }
        check_reaped(operations->observed_pid);
    }
}

TEST_CASE("Linux Process rejects watch failure before releasing the child", "[core][process][linux]")
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

TEST_CASE("Linux Process old callbacks stay inert after failed removal restart and destruction",
          "[core][process][linux]")
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
    auto                   operations = std::make_shared<ScriptedOperations>();
    ProcessTestAccess::set_operations(*process, operations);
    std::vector<FdCallback> stale_fds;
    std::vector<Task>       stale_processes;
    for (int run = 0; run < 3; ++run) {
        REQUIRE(process->start(helper()));
        auto const pid = operations->observed_pid;
        auto const fd  = operations->status_fd;
        stale_fds.push_back(EventLoopTestAccess::fd_callback(*loop, fd));
        stale_processes.push_back(EventLoopTestAccess::process_callback(*loop, pid));
        siginfo_t info{};
        int       waited;
        do {
            waited = ::waitid(P_PID, static_cast<id_t>(pid), &info, WEXITED | WNOWAIT);
        } while (waited < 0 && errno == EINTR);
        REQUIRE(waited == 0);
        // Process exit retires both callbacks; subsequent events were already returned in the same poll batch.
        fake->ready_events = {
            {.kind = ReadyEventKind::Process, .ident = pid           },
            {.ident = fd,                     .events = FdEvent::Read},
            {.kind = ReadyEventKind::Process, .ident = pid           }
        };
        CHECK(loop->process_events(EventFlag::All, 0) == ProcessEventsResult::Stopped);
        CHECK(finished == run + 1);
        check_reaped(pid);
        for (auto const& callback : stale_fds) {
            callback(fd, FdEvent::Read);
        }
        for (auto const& callback : stale_processes) {
            callback();
        }
        CHECK(finished == run + 1);
    }
    REQUIRE(process->start(helper()));
    auto const active_pid = operations->observed_pid;
    auto const active_fd  = operations->status_fd;
    // Old callbacks must not resolve or reap a newly accepted run.
    for (auto const& callback : stale_fds) {
        callback(active_fd, FdEvent::Read);
    }
    for (auto const& callback : stale_processes) {
        callback();
    }
    CHECK(process->state() == ProcessState::Starting);
    CHECK(finished == 3);
    stale_fds.push_back(EventLoopTestAccess::fd_callback(*loop, active_fd));
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
        {.ident = active_fd,              .events = FdEvent::Read},
        {.kind = ReadyEventKind::Process, .ident = active_pid    }
    };
    CHECK(loop->process_events(EventFlag::All, 0) == ProcessEventsResult::Stopped);
    CHECK(finished == 3);
}

namespace {
class RetainedPidfd final : public EpollProcessOperations {
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
} // namespace

TEST_CASE("Linux Process retained delivered pidfd does not redispatch or block unrelated work",
          "[core][process][linux]")
{
    auto native = std::make_shared<RetainedPidfd>();
    Run  run{make_epoll_backend(native)};
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

TEST_CASE("Linux Process direct finished slots can restart safely", "[core][process][linux]")
{
    Run  run;
    int  count{0};
    auto connection = run.process.finished.connect(&run.receiver, [&](ProcessExit const&) {
        if (++count < 3) {
            CHECK(run.process.start(helper()));
        }
    });
    REQUIRE(run.process.start(helper()));
    run.until([&] { return count == 3; });
    CHECK(run.starts == 3);
    CHECK(run.finishes == 3);
}

TEST_CASE("Linux Process direct lifecycle slots defer destruction without leaking the child", "[core][process][linux]")
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
        auto start  = process->started.connect(&receiver, [&] {
            if (on_started) {
                schedule();
            }
        });
        auto finish = process->finished.connect(&receiver, [&](ProcessExit const&) {
            if (!on_started) {
                schedule();
            }
        });
        // A blocked helper keeps direct-child ownership active when started schedules deletion.
        int  gate[2];
        REQUIRE(::pipe(gate) == 0);
        auto accepted = process->start(on_started ? helper({"wait", std::to_string(gate[0])}) : helper());
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
        ::close(gate[0]);
        ::close(gate[1]);
        check_reaped(pid);
    }
}

TEST_CASE("Linux Process rejects unenforced later-stage policies before native setup", "[core][process][linux]")
{
    Run  run;
    auto operations = std::make_shared<ScriptedOperations>();
    ProcessTestAccess::set_operations(run.process, operations);
    for (bool hardening : {false, true}) {
        auto info                   = helper();
        info.prevent_privilege_gain = hardening;
        if (!hardening) {
            info.timeout = 1s;
        }
        auto result = run.process.start(std::move(info));
        REQUIRE_FALSE(result);
        CHECK(result.error().category == ErrorCategory::Unsupported);
    }
    CHECK(operations->fork_calls == 0);
    CHECK(run.starts == 0);
    CHECK(run.finishes == 0);
}
