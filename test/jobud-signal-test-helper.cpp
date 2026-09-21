#include "shutdown_signal_posix_priv.hpp"
#include "shutdown_signal_priv.hpp"
#include "shutdown_signal_test_priv.hpp"

#include "event_loop.hpp"
#include "event_loop_types.hpp"
#include "support/fake_event_loop_backend.hpp"

#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>
#if defined(__APPLE__)
#  include <catch2/generators/catch_generators.hpp>
#endif

#include <array>
#include <atomic>
#include <cerrno>
#include <csignal> // IWYU pragma: keep POSIX signal sets and dispositions.
#include <cstdint>
#include <cstdlib>
#include <latch>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <fcntl.h>
#include <pthread.h>
#include <spawn.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace jb::jobud::detail {

struct ShutdownSignalTestAccess {
    static auto install(ShutdownSignalOperations const& operations) { return ShutdownSignalRelay::install(operations); }
};

namespace {

// Configured before starting workers and kept alive for the process. The actual signal handler
// calls these checkpoints, so neither a latch nor an atomic wait is allowed in the checkpoint.
static_assert(std::atomic<unsigned>::is_always_lock_free);

struct SignalProbe {
    SignalTestCheckpoint  pause_at{SignalTestCheckpoint::HandlerEntered};
    std::atomic<unsigned> entered{0};
    std::atomic<unsigned> released{1};
    std::atomic<unsigned> retirement_waiting{0};
    std::atomic<unsigned> writes_finished{0};
    std::atomic<unsigned> release_borrows{1};
};

SignalProbe signal_probe;

void pause_handlers_at(SignalTestCheckpoint checkpoint)
{
    signal_probe.pause_at = checkpoint;
    signal_probe.entered.store(0);
    signal_probe.retirement_waiting.store(0);
    signal_probe.released.store(0);
    signal_probe.writes_finished.store(0);
    signal_probe.release_borrows.store(checkpoint == SignalTestCheckpoint::DescriptorAcquired ? 0U : 1U);
}

} // namespace

void signal_test_checkpoint(SignalTestCheckpoint checkpoint) noexcept
{
    if (checkpoint == SignalTestCheckpoint::RetirementWaiting) {
        signal_probe.retirement_waiting.store(1, std::memory_order_release);
        return;
    }
    if (checkpoint == SignalTestCheckpoint::DescriptorWritten) {
        signal_probe.writes_finished.fetch_add(1, std::memory_order_release);
        while (signal_probe.release_borrows.load(std::memory_order_acquire) == 0) {
        }
        return;
    }
    if (checkpoint == signal_probe.pause_at) {
        signal_probe.entered.fetch_add(1, std::memory_order_release);
        while (signal_probe.released.load(std::memory_order_acquire) == 0) {
        }
    }
}

} // namespace jb::jobud::detail

namespace {

using jb::jobud::detail::pause_handlers_at;
using jb::jobud::detail::ShutdownSignalOperations;
using jb::jobud::detail::ShutdownSignalRelay;
using jb::jobud::detail::ShutdownSignalTestAccess;
using jb::jobud::detail::signal_probe;
using jb::jobud::detail::SignalTestCheckpoint;

enum class Failure : std::uint8_t {
    None,
    Block,
    Pipe,
    Term,
    Int,
    Enable,
    RestoreTerm,
    RestoreInt,
    RestoreMask,
};

Failure               failure{Failure::None};
std::array<int, 2>    pipe_descriptors{-1, -1};
volatile sig_atomic_t previous_calls{0};

auto consume_failure(Failure operation) noexcept -> bool
{
    if (failure != operation) {
        return false;
    }
    failure = Failure::None;
    errno   = EIO;
    return true;
}

#if defined(__APPLE__)
auto create_pipe(int* descriptors) noexcept -> int
#else
auto create_pipe(int* descriptors, int flags) noexcept -> int
#endif
{
    if (consume_failure(Failure::Pipe)) {
        return -1;
    }
#if defined(__APPLE__)
    auto const result = ::pipe(descriptors);
#else
    auto const result = ::pipe2(descriptors, flags);
#endif
    if (result == 0) {
        pipe_descriptors = {descriptors[0], descriptors[1]};
    }
    return result;
}

#if defined(__APPLE__)
int failed_control_call{-1};
int control_calls{0};

auto control_pipe(int descriptor, int command, int value) -> int
{
    if (control_calls++ == failed_control_call) {
        failed_control_call = -1;
        errno               = EIO;
        return -1;
    }
    return ::fcntl(descriptor, command, value);
}
#endif

auto set_action(int signal, struct sigaction const* action, struct sigaction* previous) noexcept -> int
{
    auto operation = signal == SIGTERM ? Failure::RestoreTerm : Failure::RestoreInt;
    if (previous) {
        operation = signal == SIGTERM ? Failure::Term : Failure::Int;
    }
    if (consume_failure(operation)) {
        return -1;
    }
    return ::sigaction(signal, action, previous);
}

auto set_mask(int how, sigset_t const* mask, sigset_t* previous) noexcept -> int
{
    if ((how == SIG_BLOCK && previous && consume_failure(Failure::Block)) ||
        (how == SIG_SETMASK && consume_failure(Failure::Enable)) ||
        (how == SIG_SETMASK && consume_failure(Failure::RestoreMask))) {
        return EIO;
    }
    return ::pthread_sigmask(how, mask, previous);
}

auto relay_operations() -> ShutdownSignalOperations
{
    return {.pipe = &create_pipe,
#if defined(__APPLE__)
            .control = &control_pipe,
#endif
            .action = &set_action,
            .mask   = &set_mask};
}

void previous_handler(int /*signal*/) noexcept
{
    previous_calls = 1;
}

struct SignalSnapshot {
    sigset_t         mask{};
    struct sigaction term{};
    struct sigaction interrupt{};
    struct sigaction child{};
};

/// @throws Catch::TestFailureException when the isolated process cannot inspect its signal state.
auto snapshot() -> SignalSnapshot
{
    SignalSnapshot state;
    REQUIRE(::pthread_sigmask(SIG_SETMASK, nullptr, &state.mask) == 0);
    REQUIRE(::sigaction(SIGTERM, nullptr, &state.term) == 0);
    REQUIRE(::sigaction(SIGINT, nullptr, &state.interrupt) == 0);
    REQUIRE(::sigaction(SIGCHLD, nullptr, &state.child) == 0);
    return state;
}

/// Deliberately unusual inherited state proves both enabling and exact restoration.
/// @throws Catch::TestFailureException if the child cannot set its test signal state.
auto prepare_signals() -> SignalSnapshot
{
    struct sigaction action{};
    action.sa_handler = &previous_handler;
    REQUIRE(sigemptyset(&action.sa_mask) == 0);
    REQUIRE(sigaddset(&action.sa_mask, SIGUSR2) == 0);
    action.sa_flags = SA_RESTART;
    REQUIRE(::sigaction(SIGTERM, &action, nullptr) == 0);
    REQUIRE(::sigaction(SIGINT, &action, nullptr) == 0);
    action.sa_flags = SA_NOCLDSTOP;
    REQUIRE(::sigaction(SIGCHLD, &action, nullptr) == 0);

    sigset_t blocked;
    REQUIRE(sigemptyset(&blocked) == 0);
    REQUIRE(sigaddset(&blocked, SIGTERM) == 0);
    REQUIRE(sigaddset(&blocked, SIGINT) == 0);
    REQUIRE(sigaddset(&blocked, SIGUSR1) == 0);
    REQUIRE(::pthread_sigmask(SIG_SETMASK, &blocked, nullptr) == 0);
    previous_calls = 0;
    return snapshot();
}

/// @throws Catch::TestFailureException if any signal membership changed.
void require_same_mask(sigset_t const& actual, sigset_t const& expected)
{
    for (int signal = 1; signal < NSIG; ++signal) {
        REQUIRE(sigismember(&actual, signal) == sigismember(&expected, signal));
    }
}

/// @throws Catch::TestFailureException if a saved disposition was not restored.
void require_same_action(struct sigaction const& actual, struct sigaction const& expected)
{
    REQUIRE(actual.sa_handler == expected.sa_handler);
    REQUIRE(actual.sa_flags == expected.sa_flags);
    require_same_mask(actual.sa_mask, expected.sa_mask);
}

/// @throws Catch::TestFailureException if cleanup did not restore the inherited state.
void require_restored(SignalSnapshot const& original)
{
    auto const actual = snapshot();
    require_same_mask(actual.mask, original.mask);
    require_same_action(actual.term, original.term);
    require_same_action(actual.interrupt, original.interrupt);
    require_same_action(actual.child, original.child);
    for (auto const fd : pipe_descriptors) {
        if (fd >= 0) {
            REQUIRE(::fcntl(fd, F_GETFD) == -1);
            REQUIRE(errno == EBADF);
        }
    }
}

/// @throws Catch::TestFailureException if relay setup or descriptor flags are incorrect.
auto install_relay() -> std::unique_ptr<ShutdownSignalRelay>
{
    pipe_descriptors = {-1, -1};
    auto installed   = ShutdownSignalTestAccess::install(relay_operations());
    REQUIRE(installed);
    for (auto const fd : pipe_descriptors) {
        REQUIRE((::fcntl(fd, F_GETFL) & O_NONBLOCK) != 0);
        REQUIRE((::fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0);
    }
    auto const active = snapshot();
    REQUIRE(sigismember(&active.mask, SIGTERM) == 0);
    REQUIRE(sigismember(&active.mask, SIGINT) == 0);
    REQUIRE(sigismember(&active.mask, SIGUSR1) == 1);
    REQUIRE(sigismember(&active.term.sa_mask, SIGTERM) == 1);
    REQUIRE(sigismember(&active.term.sa_mask, SIGINT) == 1);
    return std::move(installed).value();
}

/// @throws Catch::TestFailureException if the parent handshake fails.
void report_ready()
{
    std::string const marker{"!relay-ready!\n"};
    REQUIRE(::write(STDOUT_FILENO, marker.data(), marker.size()) == static_cast<ssize_t>(marker.size()));
}

/// @throws Catch::TestFailureException if the parent does not release startup after signalling.
void await_release()
{
    char    command{};
    ssize_t count;
    do {
        count = ::read(STDIN_FILENO, &command, 1);
    } while (count < 0 && errno == EINTR);
    REQUIRE(count == 1);
    REQUIRE(command == 'G');
}

} // namespace

TEST_CASE("startup")
{
    auto const original = prepare_signals();
    auto       relay    = install_relay();
    REQUIRE_FALSE(relay->requested());
    require_same_action(snapshot().child, original.child);

    // No EventLoop exists. The parent signals only after this readiness handshake.
    report_ready();
    await_release();
    REQUIRE(relay->requested());
    REQUIRE(previous_calls == 0);
    REQUIRE(relay->close());
    REQUIRE(relay->close());
    REQUIRE(relay->requested());
    require_restored(original);
}

TEST_CASE("loop")
{
    auto const original = prepare_signals();
    auto       relay    = install_relay();
    {
        jb::core::EventLoop loop;
        int                 notifications{0};
        auto                watch = relay->attach(loop, [&] {
            ++notifications;
            loop.request_quit();
        });
        REQUIRE(watch);
        // The parent cannot signal until a task has actually run inside the loop.
        REQUIRE(loop.post(&report_ready));
        REQUIRE(loop.run());
        REQUIRE(relay->requested());
        REQUIRE(notifications == 1);

        REQUIRE(::raise(SIGTERM) == 0);
        REQUIRE(::raise(SIGINT) == 0);
        REQUIRE(loop.process_events(jb::core::EventFlag::Watchers, 0) != jb::core::ProcessEventsResult::Failed);
        REQUIRE(notifications == 1);
        REQUIRE(previous_calls == 0);
    }
    REQUIRE(relay->close());
    require_restored(original);
}

TEST_CASE("coalescing")
{
    auto const             original = prepare_signals();
    auto                   relay    = install_relay();
    std::array<char, 4096> bytes{};
    while (::write(pipe_descriptors[1], bytes.data(), bytes.size()) > 0) {
    }
    REQUIRE(errno == EAGAIN);
    REQUIRE_FALSE(relay->requested());

    // The parent sends repeated TERM/INT while the full pipe cannot accept even one more byte.
    report_ready();
    await_release();
    REQUIRE(relay->requested());
    errno = EDOM;
    REQUIRE(::raise(SIGTERM) == 0);
    REQUIRE(errno == EDOM);

    int before{0};
    REQUIRE(::ioctl(pipe_descriptors[0], FIONREAD, &before) == 0);
    REQUIRE(before > 256);
    {
        jb::core::EventLoop loop;
        int                 notifications{0};
        auto                watch = relay->attach(loop, [&] {
            ++notifications;
            loop.request_quit();
        });
        REQUIRE(watch);
        REQUIRE(loop.run());
        int after{0};
        REQUIRE(::ioctl(pipe_descriptors[0], FIONREAD, &after) == 0);
        REQUIRE(after == before - 256);
        REQUIRE(loop.process_events(jb::core::EventFlag::Watchers, 0) != jb::core::ProcessEventsResult::Failed);
        REQUIRE(notifications == 1);
    }
    REQUIRE(relay->close());
    require_restored(original);
}

TEST_CASE("setup")
{
    auto const original = prepare_signals();
    pipe_descriptors    = {-1, -1};
    SECTION("initial signal block")
    {
        failure = Failure::Block;
    }
    SECTION("pipe creation")
    {
        failure = Failure::Pipe;
    }
#if defined(__APPLE__)
    SECTION("partial pipe descriptor configuration")
    {
        // Fail every get/set boundary on each end, including after the first end is fully configured.
        failed_control_call = GENERATE(0, 1, 2, 3, 4, 5, 6, 7);
        control_calls       = 0;
    }
#endif
    SECTION("first disposition")
    {
        failure = Failure::Term;
    }
    SECTION("second disposition")
    {
        failure = Failure::Int;
    }
    SECTION("enable delivery")
    {
        failure = Failure::Enable;
    }

    auto rejected = ShutdownSignalTestAccess::install(relay_operations());
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().code == "jobud.signal.setup");
    REQUIRE(failure == Failure::None);
#if defined(__APPLE__)
    REQUIRE(failed_control_call == -1);
#endif
    require_restored(original);

    // Partial installation must release singleton ownership as well as native resources.
    auto relay     = install_relay();
    auto duplicate = ShutdownSignalRelay::install();
    REQUIRE_FALSE(duplicate);
    REQUIRE(duplicate.error().code == "jobud.signal.active");
    REQUIRE(relay->close());
    require_restored(original);
}

TEST_CASE("cleanup")
{
    auto const original = prepare_signals();
    auto       relay    = install_relay();
    SECTION("first disposition")
    {
        failure = Failure::RestoreTerm;
    }
    SECTION("second disposition")
    {
        failure = Failure::RestoreInt;
    }
    SECTION("original mask")
    {
        failure = Failure::RestoreMask;
    }
    auto failed = relay->close();
    REQUIRE_FALSE(failed);
    REQUIRE(failed.error().code == "jobud.signal.cleanup");
    REQUIRE(failure == Failure::None);
    REQUIRE(relay->close());
    require_restored(original);
}

TEST_CASE("watch lifetime")
{
    auto const original         = prepare_signals();
    auto       relay            = install_relay();
    auto       fake             = jb::core::priv::make_fake_event_loop();
    fake.backend->add_fd_result = false;
    auto failed                 = relay->attach(*fake.loop, [] {});
    REQUIRE_FALSE(failed);
    REQUIRE(failed.error().detail == "watch_pipe");
    REQUIRE_FALSE(relay->attach(*fake.loop, {}));

    fake.backend->add_fd_result = true;
    int  notifications{0};
    auto attached = relay->attach(*fake.loop, [&] { ++notifications; });
    REQUIRE(attached);
    auto watch = std::move(attached).value();
    REQUIRE(fake.backend->last_added_trigger_mode == jb::core::FdTriggerMode::Level);
    REQUIRE_FALSE(relay->attach(*fake.loop, [] {}));
    REQUIRE_FALSE(relay->close());
    auto retained = jb::core::priv::EventLoopTestAccess::fd_callback(*fake.loop, pipe_descriptors[0]);
    REQUIRE(retained);
    REQUIRE(::raise(SIGINT) == 0);

    // Force native removal failure, then invoke the retained callback after relay destruction.
    // It must not drain a recycled descriptor or touch the former notification target.
    fake.backend->remove_fd_result = false;
    watch.reset();
    REQUIRE(relay->close());
    relay.reset();
    require_restored(original);
    std::array<int, 2> recycled{};
    REQUIRE(jb::jobud::detail::prepare_shutdown_pipe(recycled.data(), ShutdownSignalOperations{}) == 0);
    REQUIRE(recycled[0] == pipe_descriptors[0]);
    REQUIRE(::write(recycled[1], "x", 1) == 1);
    retained(recycled[0], jb::core::FdEvent::Read);
    char byte{};
    REQUIRE(::read(recycled[0], &byte, 1) == 1);
    REQUIRE(byte == 'x');
    REQUIRE(notifications == 0);
    ::close(recycled[0]);
    ::close(recycled[1]);
}

TEST_CASE("worker lifetime")
{
    auto const  original = prepare_signals();
    auto        relay    = install_relay();
    std::latch  release_worker{1};
    bool        inherited_delivery{false};
    bool        delivered{false};
    std::thread worker{[&] {
        sigset_t mask;
        inherited_delivery = ::pthread_sigmask(SIG_SETMASK, nullptr, &mask) == 0 && sigismember(&mask, SIGTERM) == 0 &&
                             sigismember(&mask, SIGINT) == 0;
        release_worker.wait();
        // Thread-directed delivery proves a worker may still access the handler's descriptor.
        delivered = ::pthread_kill(::pthread_self(), SIGTERM) == 0;
    }};
    {
        jb::core::EventLoop loop;
        auto                watch = relay->attach(loop, [] {});
        // Join even when attachment fails, so a failed test cannot leave a joinable thread.
        CHECK(watch);
    }
    release_worker.count_down();
    worker.join();
    REQUIRE(inherited_delivery);
    REQUIRE(delivered);
    REQUIRE(relay->requested());
    REQUIRE(previous_calls == 0);
    REQUIRE(relay->close());
    require_restored(original);
}

TEST_CASE("retirement waits for admitted handlers")
{
    auto const original = prepare_signals();
    auto       relay    = install_relay();
    pause_handlers_at(SignalTestCheckpoint::DescriptorAcquired);

    // Real TERM and INT handlers both hold the pipe descriptor. Keep their worker threads alive
    // after handler return as well: close must wait for the writes, but not for worker exit.
    std::latch                  finish_workers{1};
    std::atomic<unsigned>       workers_exited{0};
    std::array<bool, 2>         delivered{};
    std::array<bool, 2>         preserved_errno{};
    std::array<std::jthread, 2> workers;
    for (std::size_t index = 0; index < workers.size(); ++index) {
        workers[index] = std::jthread{[&, index] {
            errno                  = EDOM;
            delivered[index]       = ::pthread_kill(::pthread_self(), index == 0 ? SIGTERM : SIGINT) == 0;
            preserved_errno[index] = errno == EDOM;
            finish_workers.wait();
            workers_exited.fetch_add(1);
        }};
    }
    while (signal_probe.entered.load(std::memory_order_acquire) != workers.size()) {
        std::this_thread::yield();
    }

    bool failed_close_retained_resources{true};
    SECTION("ordinary retirement")
    {}
    SECTION("retry after partial disposition restoration")
    {
        failure                         = Failure::RestoreInt;
        auto rejected                   = relay->close();
        failed_close_retained_resources = !rejected && rejected.error().code == "jobud.signal.cleanup" &&
                                          ::fcntl(pipe_descriptors[0], F_GETFD) >= 0 &&
                                          ::fcntl(pipe_descriptors[1], F_GETFD) >= 0;
    }

    bool         descriptors_open_while_waiting{false};
    bool         admitted_writes_completed{false};
    std::jthread release_handlers{[&] {
        // Wait until retirement has closed admission and observed outstanding borrowers. This
        // handshake proves the pipe remains open during the wait without a timing assertion.
        while (signal_probe.retirement_waiting.load(std::memory_order_acquire) == 0) {
            std::this_thread::yield();
        }
        descriptors_open_while_waiting =
            ::fcntl(pipe_descriptors[0], F_GETFD) >= 0 && ::fcntl(pipe_descriptors[1], F_GETFD) >= 0;
        signal_probe.released.store(1, std::memory_order_release);

        // Keep both descriptor borrows until the writes are observable. Retirement cannot close
        // either pipe end while this second handshake is pending.
        while (signal_probe.writes_finished.load(std::memory_order_acquire) != workers.size()) {
            std::this_thread::yield();
        }
        std::array<char, 2> bytes{};
        admitted_writes_completed =
            ::read(pipe_descriptors[0], bytes.data(), bytes.size()) == 2 && bytes[0] == 's' && bytes[1] == 's';
        signal_probe.release_borrows.store(1, std::memory_order_release);
    }};

    auto       closed          = relay->close();
    auto const exited_at_close = workers_exited.load();
    relay.reset();
    finish_workers.count_down();
    for (auto& worker : workers) {
        worker.join();
    }
    release_handlers.join();

    REQUIRE(closed);
    REQUIRE(failed_close_retained_resources);
    REQUIRE(descriptors_open_while_waiting);
    REQUIRE(admitted_writes_completed);
    REQUIRE(exited_at_close == 0);
    for (std::size_t index = 0; index < workers.size(); ++index) {
        REQUIRE(delivered[index]);
        REQUIRE(preserved_errno[index]);
    }
    require_restored(original);
}

TEST_CASE("late handler cannot write a reused descriptor")
{
    auto const original = prepare_signals();
    auto       relay    = install_relay();
    pause_handlers_at(SignalTestCheckpoint::HandlerEntered);

    bool         delivered{false};
    bool         preserved_errno{false};
    bool         restored_delivery{false};
    std::jthread worker{[&] {
        errno             = EDOM;
        delivered         = ::pthread_kill(::pthread_self(), SIGTERM) == 0;
        preserved_errno   = errno == EDOM;
        // The worker survives relay retirement; subsequent signals use the restored disposition.
        restored_delivery = ::pthread_kill(::pthread_self(), SIGINT) == 0;
    }};
    while (signal_probe.entered.load(std::memory_order_acquire) == 0) {
        std::this_thread::yield();
    }

    // This handler has entered but has not borrowed the descriptor. Close and destroy the relay
    // while it is paused, then reuse BOTH pipe numbers before allowing its body to proceed.
    auto closed = relay->close();
    relay.reset();
    bool const         descriptors_closed = ::fcntl(pipe_descriptors[0], F_GETFD) == -1 && errno == EBADF &&
                                            ::fcntl(pipe_descriptors[1], F_GETFD) == -1 && errno == EBADF;
    std::array<int, 2> recycled{-1, -1};
    auto const         opened = jb::jobud::detail::prepare_shutdown_pipe(recycled.data(), ShutdownSignalOperations{});
    auto const         seeded = opened == 0 ? ::write(recycled[1], "x", 1) : -1;
    signal_probe.released.store(1, std::memory_order_release);
    worker.join();

    std::array<char, 2> bytes{};
    auto const          count = opened == 0 ? ::read(recycled[0], bytes.data(), bytes.size()) : -1;
    for (auto const descriptor : recycled) {
        if (descriptor >= 0) {
            ::close(descriptor);
        }
    }
    REQUIRE(closed);
    REQUIRE(descriptors_closed);
    REQUIRE(opened == 0);
    REQUIRE(recycled == pipe_descriptors);
    REQUIRE(seeded == 1);
    REQUIRE(count == 1);
    REQUIRE(bytes[0] == 'x');
    REQUIRE(delivered);
    REQUIRE(preserved_errno);
    REQUIRE(restored_delivery);
    REQUIRE(previous_calls == 1);
    require_restored(original);
}

TEST_CASE("concurrent workers")
{
    auto const original = prepare_signals();
    auto       relay    = install_relay();
    {
        jb::core::EventLoop loop;
        int                 notifications{0};
        auto                watch = relay->attach(loop, [&] {
            CHECK(relay->requested());
            ++notifications;
            loop.request_quit();
        });
        REQUIRE(watch);

        // Each worker targets itself; several handlers may publish while the owner dispatches.
        // The latch starts delivery only after loop entry, without using a scheduling delay.
        std::latch                 release_workers{1};
        std::array<bool, 4>        delivered{};
        std::array<std::thread, 4> workers;
        for (std::size_t index = 0; index < workers.size(); ++index) {
            workers[index] = std::thread{[&, index] {
                release_workers.wait();
                delivered[index] = ::pthread_kill(::pthread_self(), index % 2 == 0 ? SIGTERM : SIGINT) == 0;
            }};
        }
        auto const posted = loop.post([&] { release_workers.count_down(); });
        if (!posted) {
            release_workers.count_down();
        }
        auto const ran = posted && loop.run();
        for (auto& worker : workers) {
            worker.join();
        }
        REQUIRE(posted);
        REQUIRE(ran);
        REQUIRE(notifications == 1);
        for (auto const success : delivered) {
            REQUIRE(success);
        }
        REQUIRE(relay->requested());
    }
    REQUIRE(relay->close());
    require_restored(original);
}

namespace {

// Run before Catch2 installs its own fatal-signal handlers, so this observes the state after exec.
auto inspect_exec_state() -> int
{
    for (auto const* name : {"JOBUD_SIGNAL_READ_FD", "JOBUD_SIGNAL_WRITE_FD"}) {
        auto const* value = std::getenv(name);
        if (!value) {
            return 71;
        }
        auto const descriptor = std::atoi(value);
        if (::fcntl(descriptor, F_GETFD) != -1 || errno != EBADF) {
            return 72;
        }
    }
    struct sigaction action{};
    for (auto const signal : {SIGTERM, SIGINT}) {
        if (::sigaction(signal, nullptr, &action) != 0 || action.sa_handler != SIG_DFL) {
            return 73;
        }
    }
    return 0;
}

} // namespace

TEST_CASE("descriptor inheritance")
{
    auto const original = prepare_signals();
    auto       relay    = install_relay();
    REQUIRE(::setenv("JOBUD_SIGNAL_READ_FD", std::to_string(pipe_descriptors[0]).c_str(), 1) == 0);
    REQUIRE(::setenv("JOBUD_SIGNAL_WRITE_FD", std::to_string(pipe_descriptors[1]).c_str(), 1) == 0);
    char                 executable[]{JOBUD_SIGNAL_TEST_HELPER};
    char                 filter[]{"--exec-probe"};
    std::array<char*, 3> arguments{executable, filter, nullptr};
    pid_t                child{-1};
    REQUIRE(::posix_spawn(&child, executable, nullptr, nullptr, arguments.data(), ::environ) == 0);
    int   status{0};
    pid_t waited;
    do {
        waited = ::waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    REQUIRE(waited == child);
    REQUIRE(WIFEXITED(status));
    REQUIRE(WEXITSTATUS(status) == 0);
    REQUIRE_FALSE(relay->requested());
    require_same_action(snapshot().child, original.child);
    REQUIRE(relay->close());
    require_restored(original);
}

auto main(int argc, char* argv[]) -> int
{
    if (argc == 2 && std::string_view{argv[1]} == "--exec-probe") {
        return inspect_exec_state();
    }

    // The parent registers native exit observation before allowing even a fast-failing helper to run.
    char    command{};
    ssize_t received;
    do {
        received = ::read(STDIN_FILENO, &command, 1);
    } while (received < 0 && errno == EINTR);
    if (received != 1 || command != 'G') {
        return 70;
    }
    if (argc > 1 && std::string_view{argv[1]} == "--release-exit") {
        // A deliberate peer exit gives the parent a deterministic EPIPE and buffered diagnostic.
        std::string const message{"helper exited before release\n"};
        static_cast<void>(::write(STDOUT_FILENO, message.data(), message.size()));
        return 23;
    }
    return Catch::Session().run(argc, argv);
}
