#include "event_loop.hpp"

#include "error.hpp"
#include "event_loop_backend.hpp"
#include "event_loop_types.hpp"
#include "result.hpp"
#include "support/catch_utils.hpp" // IWYU pragma: keep for Catch::StringMaker specializations
#include "support/fake_event_loop_backend.hpp"
#include "thread_context.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>
#include <vector>

#if defined(__linux__)
#  include "event_loop_backend_epoll_priv.hpp"
#  include <sys/epoll.h>
#endif
#if defined(__APPLE__)
#  include "event_loop_backend_kqueue_priv.hpp"
#  include <sys/event.h>
#endif

#if defined(__linux__) || defined(__APPLE__)

#  include <cerrno>
#  include <chrono>
#  include <csignal> // IWYU pragma: keep Provides POSIX kill() and SIGKILL through <signal.h>.
#  include <fcntl.h>
#  include <limits>
#  include <poll.h>
#  include <sys/socket.h>
#  include <sys/types.h>
#  include <sys/wait.h>
#  include <unistd.h>
#  include <utility>
#endif

using namespace jb::core;
using namespace jb::core::priv;

TEST_CASE("Process watches reject invalid registrations without native work", "[core][event-loop][process-watch]")
{
    auto fake = make_fake_event_loop();
    for (auto const pid : {-1, 0}) {
        auto result = EventLoopTestAccess::watch_process(*fake.loop, pid, [] {});
        REQUIRE_FALSE(result);
        CHECK(result.error().code == "core.process.invalid_request");
        CHECK(result.error().category == ErrorCategory::InvalidArgument);
    }
    CHECK_FALSE(EventLoopTestAccess::watch_process(*fake.loop, 42, {}));
    CHECK(fake.backend->add_process_calls == 0);
    CHECK(EventLoopTestAccess::active_process_count(*fake.loop) == 0);

    auto invalid = EventLoopTestAccess::make_event_loop(nullptr);
    auto result  = EventLoopTestAccess::watch_process(*invalid, 42, [] {});
    REQUIRE_FALSE(result);
    CHECK(result.error().code == "core.process.event_loop_unavailable");
    CHECK(result.error().category == ErrorCategory::Unavailable);
    CHECK(EventLoopTestAccess::unwatch_process(*invalid, 42));
}

TEST_CASE("Process registration distinguishes unsupported monitoring and operational failure",
          "[core][event-loop][process-watch]")
{
    auto fake                         = make_fake_event_loop();
    fake.backend->add_process_results = {ProcessRegistrationResult::Unsupported,
                                         ProcessRegistrationResult::Failed,
                                         ProcessRegistrationResult::Added};
    auto unsupported                  = EventLoopTestAccess::watch_process(*fake.loop, 42, [] {});
    REQUIRE_FALSE(unsupported);
    CHECK(unsupported.error().code == "core.process.monitor_unsupported");
    CHECK(unsupported.error().category == ErrorCategory::Unsupported);
    CHECK(EventLoopTestAccess::active_process_count(*fake.loop) == 0);

    auto failed = EventLoopTestAccess::watch_process(*fake.loop, 42, [] {});
    REQUIRE_FALSE(failed);
    CHECK(failed.error().code == "core.process.watch_failed");
    CHECK(failed.error().category == ErrorCategory::Unavailable);
    CHECK(EventLoopTestAccess::active_process_count(*fake.loop) == 0);
    REQUIRE(EventLoopTestAccess::watch_process(*fake.loop, 42, [] {}));
    CHECK(fake.backend->add_process_calls == 3);
    CHECK(fake.backend->last_added_process == 42);
    CHECK(EventLoopTestAccess::active_process_count(*fake.loop) == 1);
}

TEST_CASE("Process callback replacement neither rearms nor accepts an empty replacement",
          "[core][event-loop][process-watch]")
{
    auto fake = make_fake_event_loop();
    int  original_calls{0};
    int  replacement_calls{0};
    REQUIRE(EventLoopTestAccess::watch_process(*fake.loop, 42, [&] { ++original_calls; }));
    fake.backend->add_process_result = ProcessRegistrationResult::Failed;
    REQUIRE(EventLoopTestAccess::watch_process(*fake.loop, 42, [&] { ++replacement_calls; }));
    CHECK_FALSE(EventLoopTestAccess::watch_process(*fake.loop, 42, {}));
    CHECK(fake.backend->add_process_calls == 1);
    fake.backend->ready_events.push_back({.kind = ReadyEventKind::Process, .ident = 42, .events = {}});
    CHECK(fake.loop->process_events(EventFlag::Watchers, 0) == ProcessEventsResult::Stopped);
    CHECK(original_calls == 0);
    CHECK(replacement_calls == 1);
}

TEST_CASE("Process removal failure retains its callback until successful retry", "[core][event-loop][process-watch]")
{
    auto fake = make_fake_event_loop();
    int  calls{0};
    REQUIRE(EventLoopTestAccess::watch_process(*fake.loop, 42, [&] { ++calls; }));
    fake.backend->remove_process_results = {false, false, true};
    for (int i = 0; i < 2; ++i) {
        CHECK_FALSE(EventLoopTestAccess::unwatch_process(*fake.loop, 42));
        auto retained = EventLoopTestAccess::process_callback(*fake.loop, 42);
        REQUIRE(retained);
        retained();
        fake.backend->ready_events.push_back({.kind = ReadyEventKind::Process, .ident = 42, .events = {}});
        CHECK(fake.loop->process_events(EventFlag::Watchers, 0) == ProcessEventsResult::Stopped);
    }
    CHECK(calls == 4);
    REQUIRE(EventLoopTestAccess::unwatch_process(*fake.loop, 42));
    CHECK_FALSE(EventLoopTestAccess::process_callback(*fake.loop, 42));
    CHECK(EventLoopTestAccess::unwatch_process(*fake.loop, 42));
    CHECK(fake.backend->remove_process_calls == 3);
    CHECK(fake.backend->last_removed_process == 42);
    fake.backend->ready_events.push_back({.kind = ReadyEventKind::Process, .ident = 42, .events = {}});
    CHECK(fake.loop->process_events(EventFlag::Watchers, 0) == ProcessEventsResult::Stopped);
    CHECK(calls == 4);
}

TEST_CASE("Process replacement rejects a retained watch when native removal still fails",
          "[core][event-loop][process-watch]")
{
    auto fake = make_fake_event_loop();
    int  original_calls{0};
    int  replacement_calls{0};
    REQUIRE(EventLoopTestAccess::watch_process(*fake.loop, 42, [&] { ++original_calls; }));
    fake.backend->remove_process_result = false;
    REQUIRE_FALSE(EventLoopTestAccess::unwatch_process(*fake.loop, 42));

    auto result = EventLoopTestAccess::watch_process(*fake.loop, 42, [&] { ++replacement_calls; });
    REQUIRE_FALSE(result);
    CHECK(result.error().category == ErrorCategory::Unavailable);
    CHECK(result.error().code == "core.process.watch_failed");
    CHECK(result.error().detail == "watch.retained_registration");
    CHECK(fake.backend->remove_process_calls == 2);
    CHECK(fake.backend->add_process_calls == 1);
    CHECK(EventLoopTestAccess::active_process_count(*fake.loop) == 1);
    fake.backend->ready_events.push_back({.kind = ReadyEventKind::Process, .ident = 42, .events = {}});
    CHECK(fake.loop->process_events(EventFlag::Watchers, 0) == ProcessEventsResult::Stopped);
    CHECK(original_calls == 1);
    CHECK(replacement_calls == 0);

    // Another PID remains independent of the retained registration.
    REQUIRE(EventLoopTestAccess::watch_process(*fake.loop, 43, [] {}));
    CHECK(fake.backend->add_process_calls == 2);
}

TEST_CASE("Process replacement retires a retained watch before attempting fresh registration",
          "[core][event-loop][process-watch]")
{
    for (auto const added : {ProcessRegistrationResult::Added,
                             ProcessRegistrationResult::Failed,
                             ProcessRegistrationResult::Unsupported}) {
        auto fake = make_fake_event_loop();
        int  original_calls{0};
        int  replacement_calls{0};
        REQUIRE(EventLoopTestAccess::watch_process(*fake.loop, 42, [&] { ++original_calls; }));
        fake.backend->remove_process_results = {false, true};
        REQUIRE_FALSE(EventLoopTestAccess::unwatch_process(*fake.loop, 42));
        fake.backend->add_process_result = added;

        auto result = EventLoopTestAccess::watch_process(*fake.loop, 42, [&] { ++replacement_calls; });
        CHECK(fake.backend->remove_process_calls == 2);
        CHECK(fake.backend->last_removed_process == 42);
        CHECK(fake.backend->add_process_calls == 2);
        CHECK(fake.backend->last_added_process == 42);
        if (added == ProcessRegistrationResult::Added) {
            REQUIRE(result);
            CHECK(EventLoopTestAccess::active_process_count(*fake.loop) == 1);
            // A successfully refreshed entry once again supports ordinary callback-only replacement.
            REQUIRE(EventLoopTestAccess::watch_process(*fake.loop, 42, [&] { ++replacement_calls; }));
            CHECK(fake.backend->remove_process_calls == 2);
            CHECK(fake.backend->add_process_calls == 2);
        }
        else {
            REQUIRE_FALSE(result);
            CHECK(result.error().code == (added == ProcessRegistrationResult::Unsupported
                                              ? "core.process.monitor_unsupported"
                                              : "core.process.watch_failed"));
            CHECK(EventLoopTestAccess::active_process_count(*fake.loop) == 0);
            CHECK_FALSE(EventLoopTestAccess::process_callback(*fake.loop, 42));
        }
        fake.backend->ready_events.push_back({.kind = ReadyEventKind::Process, .ident = 42, .events = {}});
        CHECK(fake.loop->process_events(EventFlag::Watchers, 0) == ProcessEventsResult::Stopped);
        CHECK(original_calls == 0);
        CHECK(replacement_calls == (added == ProcessRegistrationResult::Added ? 1 : 0));
    }
}

TEST_CASE("Typed readiness isolates equal fd and process identifiers and preserves wide PIDs",
          "[core][event-loop][process-watch]")
{
    auto             fake = make_fake_event_loop();
    std::vector<int> order;
    REQUIRE(fake.loop->watch_fd(42, FdEvent::Read, FdTriggerMode::Edge, [&](int fd, FdEvents events) {
        CHECK(fd == 42);
        CHECK(events.bits() == FdEvents{FdEvent::Read}.bits());
        order.push_back(1);
    }));
    REQUIRE(EventLoopTestAccess::watch_process(*fake.loop, 42, [&] { order.push_back(2); }));
    auto const wide_pid = std::int64_t{1} << 40U;
    REQUIRE(EventLoopTestAccess::watch_process(*fake.loop, wide_pid, [&] { order.push_back(3); }));
    fake.backend->ready_events = {
        {.kind = ReadyEventKind::Process,        .ident = 42,            .events = {}           },
        {.kind = ReadyEventKind::FileDescriptor, .ident = 42,            .events = FdEvent::Read},
        {.kind = ReadyEventKind::Process,        .ident = wide_pid,      .events = {}           },
        {.kind = ReadyEventKind::FileDescriptor, .ident = wide_pid + 42, .events = FdEvent::Read},
        {.kind = ReadyEventKind::Process,        .ident = 99,            .events = {}           },
    };
    CHECK(fake.loop->process_events(EventFlag::Watchers, 0) == ProcessEventsResult::Stopped);
    CHECK(order == std::vector<int>{2, 1, 3});
}

TEST_CASE("Process callbacks can retire themselves and another already-polled registration",
          "[core][event-loop][process-watch]")
{
    auto               fake     = make_fake_event_loop();
    auto               state    = std::make_shared<int>(7);
    std::weak_ptr<int> lifetime = state;
    bool               called{false};
    REQUIRE(EventLoopTestAccess::watch_process(*fake.loop, 42, [&, state] {
        REQUIRE(EventLoopTestAccess::unwatch_process(*fake.loop, 42));
        REQUIRE(EventLoopTestAccess::unwatch_process(*fake.loop, 43));
        CHECK_FALSE(lifetime.expired());
        CHECK(*state == 7);
        called = true;
    }));
    state.reset();
    REQUIRE(EventLoopTestAccess::watch_process(*fake.loop, 43, [] { FAIL("Removed polled callback was dispatched"); }));
    fake.backend->ready_events = {
        {.kind = ReadyEventKind::Process, .ident = 42, .events = {}},
        {.kind = ReadyEventKind::Process, .ident = 43, .events = {}},
    };
    CHECK(fake.loop->process_events(EventFlag::Watchers, 0) == ProcessEventsResult::Stopped);
    CHECK(called);
    CHECK(lifetime.expired());
    CHECK(EventLoopTestAccess::active_process_count(*fake.loop) == 0);
}

TEST_CASE("Backend poll failure does not dispatch or discard a process callback", "[core][event-loop][process-watch]")
{
    auto fake = make_fake_event_loop();
    REQUIRE(EventLoopTestAccess::watch_process(*fake.loop, 42, [] { FAIL("Failed poll dispatched readiness"); }));
    fake.backend->poll_result = -1;
    CHECK(fake.loop->process_events(EventFlag::All, 0) == ProcessEventsResult::Failed);
    CHECK(EventLoopTestAccess::process_callback(*fake.loop, 42));
    CHECK(EventLoopTestAccess::unwatch_process(*fake.loop, 42));
}

#if defined(__linux__) || defined(__APPLE__)
namespace {

/// Test-only child creation: the child cannot exit normally until the parent releases its gate.
/// Cleanup always kills/reaps an unreaped direct child, including after a failed REQUIRE.
class GatedChild final {
public:
    GatedChild() = default;

    ~GatedChild()
    {
        if (_gate >= 0) {
            ::close(_gate);
        }
        if (_pid > 0) {
            ::kill(_pid, SIGKILL);
            while (::waitpid(_pid, nullptr, 0) < 0 && errno == EINTR) {
            }
        }
    }

    GatedChild(GatedChild const&)                    = delete;
    auto operator=(GatedChild const&) -> GatedChild& = delete;

    auto start() -> bool
    {
        int sockets[2];
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
            return false;
        }
        // Configure the test gate before creation on both platforms; failed setup owns no child.
        bool configured =
            ::fcntl(sockets[0], F_SETFD, FD_CLOEXEC) == 0 && ::fcntl(sockets[1], F_SETFD, FD_CLOEXEC) == 0;
#  if defined(__APPLE__)
        int const enabled{1};
        configured = configured && ::setsockopt(sockets[0], SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) == 0;
#  endif
        if (!configured) {
            ::close(sockets[0]);
            ::close(sockets[1]);
            return false;
        }
        _pid = ::fork();
        if (_pid == 0) {
            // No test framework, allocation, or user callback runs in the child branch.
            ::close(sockets[0]);
            char    byte{};
            ssize_t count;
            do {
                count = ::read(sockets[1], &byte, 1);
            } while (count < 0 && errno == EINTR);
            ::_exit(count == 1 && byte == 'x' ? 37 : 38);
        }
        ::close(sockets[1]);
        _gate = sockets[0];
        return _pid > 0;
    }

    auto release() -> bool
    {
        char const byte{'x'};
        ssize_t    count;
        do {
#  if defined(__APPLE__)
            count = ::send(_gate, &byte, 1, 0);
#  else
            count = ::send(_gate, &byte, 1, MSG_NOSIGNAL);
#  endif
        } while (count < 0 && errno == EINTR);
        ::close(_gate);
        _gate = -1;
        return count == 1;
    }

    auto reap(int& status, int options = WNOHANG) -> pid_t
    {
        pid_t result;
        do {
            result = ::waitpid(_pid, &status, options);
        } while (result < 0 && errno == EINTR);
        if (result == _pid) {
            _pid = -1;
        }
        return result;
    }

    auto pid() const -> pid_t { return _pid; }

private:
    pid_t _pid{-1};
    int   _gate{-1};
};

auto pump_until_exit(EventLoop& loop, int const& notifications) -> bool
{
    // Elapsed time bounds a broken test; only native exit readiness establishes success.
    auto const deadline = Clock::now() + std::chrono::seconds{5};
    while (notifications == 0 && Clock::now() < deadline) {
        if (loop.process_events(EventFlag::All, 100) == ProcessEventsResult::Failed) {
            return false;
        }
    }
    return notifications == 1;
}

} // namespace
#endif

#if defined(__linux__)
namespace {

/// Inject only process-watch syscalls; successful operations still use real pidfds and epoll.
class ScriptedProcessOperations final : public EpollProcessOperations {
public:
    int           open_error{0};
    int           add_error{0};
    bool          fail_remove{false};
    int           open_calls{0};
    int           add_calls{0};
    int           remove_calls{0};
    int           pidfd{-1};
    pid_t         target_pid{-1};
    std::uint32_t registered_events{0};

    auto open_pidfd(int process_id) -> int override
    {
        ++open_calls;
        if (open_error != 0) {
            errno = open_error;
            return -1;
        }
        // Map one logical watch key to successive real children without relying on kernel PID reuse timing.
        pidfd = EpollProcessOperations::open_pidfd(target_pid > 0 ? target_pid : process_id);
        return pidfd;
    }

    auto control(int poller, int operation, int fd, epoll_event* event) -> int override
    {
        if (operation == EPOLL_CTL_ADD) {
            ++add_calls;
            registered_events = event->events;
            if (add_error != 0) {
                errno = add_error;
                return -1;
            }
        }
        if (operation == EPOLL_CTL_DEL) {
            ++remove_calls;
            if (fail_remove) {
                errno = EIO;
                return -1;
            }
        }
        return EpollProcessOperations::control(poller, operation, fd, event);
    }
};

void check_closed(int fd)
{
    CHECK(::fcntl(fd, F_GETFD) == -1);
    CHECK(errno == EBADF);
}

} // namespace

TEST_CASE("Linux pidfd readiness observes a gated child without reaping it", "[core][event-loop][process-watch][linux]")
{
    GatedChild child;
    REQUIRE(child.start());
    auto const pid        = child.pid();
    auto       operations = std::make_shared<ScriptedProcessOperations>();
    auto       loop       = EventLoopTestAccess::make_event_loop(make_epoll_backend(operations));
    REQUIRE(loop->is_valid());
    int         notifications{0};
    auto const* owner = loop->thread_ctx();
    REQUIRE(EventLoopTestAccess::watch_process(*loop, pid, [&] {
        CHECK(ThreadCtx::current() == owner);
        ++notifications;
        REQUIRE(EventLoopTestAccess::unwatch_process(*loop, pid));
    }));
    CHECK((::fcntl(operations->pidfd, F_GETFD) & FD_CLOEXEC) != 0);
    CHECK(operations->registered_events == (EPOLLIN | EPOLLONESHOT));
    CHECK(loop->process_events(EventFlag::All, 0) == ProcessEventsResult::Stopped);
    CHECK(notifications == 0);
    REQUIRE(child.release());
    REQUIRE(pump_until_exit(*loop, notifications));
    check_closed(operations->pidfd);
    int status{};
    REQUIRE(child.reap(status) == pid);
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 37);
    CHECK(::waitpid(pid, &status, WNOHANG) == -1);
    CHECK(errno == ECHILD);
    CHECK(EventLoopTestAccess::active_process_count(*loop) == 0);
}

TEST_CASE("Delivered Linux pidfds never rearm when removal fails", "[core][event-loop][process-watch][linux]")
{
    GatedChild child;
    REQUIRE(child.start());
    auto const pid        = child.pid();
    auto       operations = std::make_shared<ScriptedProcessOperations>();
    auto       backend    = make_epoll_backend(operations);
    REQUIRE(backend);
    auto* native            = backend.get();
    auto  loop              = EventLoopTestAccess::make_event_loop(std::move(backend));
    operations->fail_remove = true;
    int notifications{0};
    REQUIRE(EventLoopTestAccess::watch_process(*loop, pid, [&] {
        ++notifications;
        CHECK_FALSE(EventLoopTestAccess::unwatch_process(*loop, pid));
    }));
    REQUIRE(child.release());
    REQUIRE(pump_until_exit(*loop, notifications));
    REQUIRE(EventLoopTestAccess::process_callback(*loop, pid));

    // The actual descriptor remains readable while epoll's delivered one-shot watch stays disabled.
    pollfd descriptor{.fd = operations->pidfd, .events = POLLIN, .revents = 0};
    REQUIRE(::poll(&descriptor, 1, 0) == 1);
    CHECK((descriptor.revents & POLLIN) != 0);
    CHECK(native->add_process(pid) == ProcessRegistrationResult::Added);
    auto replacement = EventLoopTestAccess::watch_process(*loop, pid, [&] { ++notifications; });
    REQUIRE_FALSE(replacement);
    CHECK(replacement.error().code == "core.process.watch_failed");
    CHECK(operations->open_calls == 1);
    CHECK(operations->add_calls == 1);

    int tasks{0};
    int timers{0};
    for (int i = 0; i < 10; ++i) {
        CHECK_FALSE(EventLoopTestAccess::unwatch_process(*loop, pid));
        ReadyEvent event;
        CHECK(native->poll(&event, 1, 0) == 0);
        REQUIRE(loop->post([&] { ++tasks; }));
        REQUIRE(loop->post_at(Clock::now(), [&] { ++timers; }));
        CHECK(loop->process_events(EventFlag::All, 0) == ProcessEventsResult::Stopped);
    }
    CHECK(notifications == 1);
    CHECK(tasks == 10);
    CHECK(timers == 10);
    operations->fail_remove = false;
    REQUIRE(EventLoopTestAccess::unwatch_process(*loop, pid));
    check_closed(operations->pidfd);
    int status{};
    REQUIRE(child.reap(status) == pid);
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 37);
}

TEST_CASE("Linux retained process watches install a fresh pidfd for a reused watch key",
          "[core][event-loop][process-watch][linux]")
{
    GatedChild first;
    REQUIRE(first.start());
    auto const key        = first.pid();
    auto       operations = std::make_shared<ScriptedProcessOperations>();
    auto       loop       = EventLoopTestAccess::make_event_loop(make_epoll_backend(operations));
    REQUIRE(loop->is_valid());
    operations->fail_remove = true;
    int first_notifications{0};
    REQUIRE(EventLoopTestAccess::watch_process(*loop, key, [&] {
        ++first_notifications;
        CHECK_FALSE(EventLoopTestAccess::unwatch_process(*loop, key));
    }));
    REQUIRE(first.release());
    REQUIRE(pump_until_exit(*loop, first_notifications));
    int status{};
    REQUIRE(first.reap(status) == key);
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 37);

    // The old child is reaped, but failed removal still owns its delivered pidfd. Keep the logical key
    // and change only the syscall target so the next registration must observe a different real child.
    GatedChild second;
    REQUIRE(second.start());
    auto const second_pid  = second.pid();
    operations->target_pid = second_pid;
    int second_notifications{0};
    REQUIRE_FALSE(EventLoopTestAccess::watch_process(*loop, key, [&] { ++second_notifications; }));
    CHECK(operations->open_calls == 1);
    CHECK(operations->add_calls == 1);

    operations->fail_remove = false;
    REQUIRE(EventLoopTestAccess::watch_process(*loop, key, [&] {
        ++second_notifications;
        CHECK(EventLoopTestAccess::unwatch_process(*loop, key));
    }));
    CHECK(operations->remove_calls == 3);
    CHECK(operations->open_calls == 2);
    CHECK(operations->add_calls == 2);
    CHECK(loop->process_events(EventFlag::All, 0) == ProcessEventsResult::Stopped);
    CHECK(first_notifications == 1);
    CHECK(second_notifications == 0);
    REQUIRE(second.release());
    REQUIRE(pump_until_exit(*loop, second_notifications));
    CHECK(first_notifications == 1);
    CHECK(EventLoopTestAccess::active_process_count(*loop) == 0);
    check_closed(operations->pidfd);
    REQUIRE(second.reap(status) == second_pid);
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 37);
}

TEST_CASE("Linux process-watch failures retain no partial native registration",
          "[core][event-loop][process-watch][linux]")
{
    auto operations = std::make_shared<ScriptedProcessOperations>();
    auto backend    = make_epoll_backend(operations);
    REQUIRE(backend);
    CHECK_FALSE(make_epoll_backend(nullptr));
    for (auto const pid : {std::int64_t{-1}, std::int64_t{0}, std::numeric_limits<std::int64_t>::max()}) {
        CHECK(backend->add_process(pid) == ProcessRegistrationResult::Failed);
    }
    CHECK(operations->open_calls == 0);
    for (auto const error : {ENOSYS, ENODEV}) {
        operations->open_error = error;
        CHECK(backend->add_process(42) == ProcessRegistrationResult::Unsupported);
    }
    for (auto const error : {EPERM, ESRCH, EMFILE}) {
        operations->open_error = error;
        CHECK(backend->add_process(42) == ProcessRegistrationResult::Failed);
    }
    CHECK(operations->add_calls == 0);
    CHECK(backend->remove_process(42));
    CHECK(operations->remove_calls == 0);

    GatedChild child;
    REQUIRE(child.start());
    operations->open_error = 0;
    for (auto const error : {ENOSYS, ENODEV}) {
        operations->add_error = error;
        CHECK(backend->add_process(child.pid()) == ProcessRegistrationResult::Failed);
        check_closed(operations->pidfd);
        CHECK(backend->remove_process(child.pid()));
        CHECK(operations->remove_calls == 0);
    }
    operations->add_error = 0;
    REQUIRE(backend->add_process(child.pid()) == ProcessRegistrationResult::Added);
    CHECK_FALSE(backend->add_fd(operations->pidfd, FdEvent::Read, FdTriggerMode::Level));
    CHECK_FALSE(backend->remove_fd(operations->pidfd));
    operations->fail_remove = true;
    CHECK_FALSE(backend->remove_process(child.pid()));
    backend.reset();
    check_closed(operations->pidfd);
    // Backend destruction closes its retained pidfd; the fixture still exclusively owns the child.
    CHECK(::kill(child.pid(), 0) == 0);
}
#endif

#if defined(__APPLE__)
namespace {

/// Fail only process-filter changes; successful registration and readiness still use the native kqueue.
class ScriptedKqueueProcessOperations final : public KqueueProcessOperations {
public:
    int add_error{0};
    int remove_error{0};
    int interruptions{0};
    int add_calls{0};
    int remove_calls{0};
    int missing_removals{0};

    auto control(int poller, std::int64_t process_id, std::uint16_t flags) -> int override
    {
        auto const removing = (flags & EV_DELETE) != 0U;
        if (removing) {
            ++remove_calls;
        }
        else {
            ++add_calls;
            CHECK(flags == (EV_ADD | EV_ONESHOT));
        }
        if (interruptions > 0) {
            --interruptions;
            errno = EINTR;
            return -1;
        }
        auto const error = removing ? remove_error : add_error;
        if (error != 0) {
            errno = error;
            return -1;
        }
        auto const result = KqueueProcessOperations::control(poller, process_id, flags);
        if (result < 0 && removing && errno == ENOENT) {
            ++missing_removals;
        }
        return result;
    }
};

} // namespace

TEST_CASE("Kqueue observes gated child exit without reaping and never rearms a delivered watch",
          "[core][event-loop][process-watch][macos]")
{
    GatedChild child;
    REQUIRE(child.start());
    auto const pid        = child.pid();
    auto       operations = std::make_shared<ScriptedKqueueProcessOperations>();
    auto       backend    = make_kqueue_backend(operations);
    REQUIRE(backend);
    // Keep direct access to verify Backend's duplicate contract as well as EventLoop callback replacement.
    auto* native = backend.get();
    auto  loop   = EventLoopTestAccess::make_event_loop(std::move(backend));
    REQUIRE(loop->is_valid());
    int notifications{0};
    REQUIRE(EventLoopTestAccess::watch_process(*loop, pid, [&] {
        CHECK(ThreadCtx::current() == loop->thread_ctx());
        ++notifications;
    }));
    CHECK(loop->process_events(EventFlag::All, 0) == ProcessEventsResult::Stopped);
    CHECK(notifications == 0);
    REQUIRE(child.release());
    REQUIRE(pump_until_exit(*loop, notifications));

    REQUIRE(native->add_process(pid) == ProcessRegistrationResult::Added);
    CHECK(operations->add_calls == 1);
    operations->remove_error = EIO;
    CHECK_FALSE(EventLoopTestAccess::unwatch_process(*loop, pid));
    CHECK_FALSE(EventLoopTestAccess::watch_process(*loop, pid, [&] { FAIL("Retained watch was replaced"); }));
    CHECK(operations->add_calls == 1);
    CHECK(EventLoopTestAccess::active_process_count(*loop) == 1);
    bool task_ran{false};
    REQUIRE(loop->post([&] { task_ran = true; }));
    for (int poll = 0; poll < 4; ++poll) {
        CHECK(loop->process_events(EventFlag::All, 0) == ProcessEventsResult::Stopped);
    }
    CHECK(task_ran);
    CHECK(notifications == 1);

    // Native NOTE_EXIT observation leaves exclusive reaping ownership with the caller.
    int status{};
    REQUIRE(child.reap(status) == pid);
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 37);
    operations->remove_error = 0;
    REQUIRE(EventLoopTestAccess::unwatch_process(*loop, pid));
    CHECK(operations->missing_removals == 1);
    CHECK(EventLoopTestAccess::active_process_count(*loop) == 0);
    CHECK(::waitpid(pid, &status, WNOHANG) == -1);
    CHECK(errno == ECHILD);
}

TEST_CASE("Kqueue removes and reinstalls a live child watch after a retained removal failure",
          "[core][event-loop][process-watch][macos]")
{
    GatedChild child;
    REQUIRE(child.start());
    auto const pid        = child.pid();
    auto       operations = std::make_shared<ScriptedKqueueProcessOperations>();
    auto       loop       = EventLoopTestAccess::make_event_loop(make_kqueue_backend(operations));
    REQUIRE(loop->is_valid());
    REQUIRE(EventLoopTestAccess::watch_process(*loop, pid, [] { FAIL("Old callback survived replacement"); }));
    operations->remove_error = EIO;
    CHECK_FALSE(EventLoopTestAccess::unwatch_process(*loop, pid));
    operations->remove_error = 0;
    int notifications{0};
    REQUIRE(EventLoopTestAccess::watch_process(*loop, pid, [&] {
        ++notifications;
        CHECK(EventLoopTestAccess::unwatch_process(*loop, pid));
    }));
    CHECK(operations->add_calls == 2);
    CHECK(operations->missing_removals == 0);
    REQUIRE(child.release());
    REQUIRE(pump_until_exit(*loop, notifications));
    CHECK(operations->missing_removals == 1);
    int status{};
    REQUIRE(child.reap(status) == pid);
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 37);
}

TEST_CASE("Kqueue process registration failures are transactional and interrupted changes retry",
          "[core][event-loop][process-watch][macos]")
{
    auto operations = std::make_shared<ScriptedKqueueProcessOperations>();
    auto backend    = make_kqueue_backend(operations);
    REQUIRE(backend);
    CHECK_FALSE(make_kqueue_backend(nullptr));
    for (auto const pid : {std::int64_t{-1}, std::int64_t{0}, std::numeric_limits<std::int64_t>::max()}) {
        CHECK(backend->add_process(pid) == ProcessRegistrationResult::Failed);
    }
    CHECK(operations->add_calls == 0);
    GatedChild child;
    REQUIRE(child.start());
    for (auto const error : {ESRCH, EPERM, ENOMEM, EINVAL}) {
        operations->add_error = error;
        CHECK(backend->add_process(child.pid()) == ProcessRegistrationResult::Failed);
        CHECK(backend->remove_process(child.pid()));
        CHECK(operations->remove_calls == 0);
    }
    operations->add_error     = 0;
    operations->interruptions = 1;
    REQUIRE(backend->add_process(child.pid()) == ProcessRegistrationResult::Added);
    CHECK(operations->add_calls == 6);
    operations->interruptions = 1;
    REQUIRE(backend->remove_process(child.pid()));
    CHECK(operations->remove_calls == 2);
    REQUIRE(backend->add_process(child.pid()) == ProcessRegistrationResult::Added);
    REQUIRE(backend->remove_process(child.pid()));
    auto const pid = child.pid();
    REQUIRE(child.release());
    int status{};
    REQUIRE(child.reap(status, 0) == pid);
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 37);
    // Observe absence only after exit is established, so this cannot pass just because the child was still running.
    ReadyEvent event;
    CHECK(backend->poll(&event, 1, 0) == 0);

    GatedChild retained;
    REQUIRE(retained.start());
    REQUIRE(backend->add_process(retained.pid()) == ProcessRegistrationResult::Added);
    backend.reset();
    // Destroying the poller releases the watch without killing or reaping its still-gated child.
    CHECK(retained.reap(status) == 0);
    REQUIRE(retained.release());
    REQUIRE(retained.reap(status, 0) > 0);
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 37);
}

TEST_CASE("Kqueue process exits coexist with native descriptor readiness", "[core][event-loop][process-watch][macos]")
{
    GatedChild first;
    GatedChild second;
    REQUIRE(first.start());
    REQUIRE(second.start());
    auto loop = EventLoopTestAccess::make_event_loop(make_backend());
    REQUIRE(loop->is_valid());
    int first_notifications{0};
    int second_notifications{0};
    REQUIRE(EventLoopTestAccess::watch_process(*loop, first.pid(), [&] { ++first_notifications; }));
    REQUIRE(EventLoopTestAccess::watch_process(*loop, second.pid(), [&] { ++second_notifications; }));

    // A socket supplies real fd readiness alongside both process filters, without timing-dependent sleeps.
    struct SocketPair {
        int descriptors[2]{-1, -1};

        ~SocketPair()
        {
            for (auto fd : descriptors) {
                if (fd >= 0) {
                    ::close(fd);
                }
            }
        }
    } sockets;

    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets.descriptors) == 0);
    int     fd_notifications{0};
    FdWatch watch;
    watch = loop->watch_fd(sockets.descriptors[0], FdEvent::Read, FdTriggerMode::Edge, [&](int fd, FdEvents events) {
        CHECK(events.test(FdEvent::Read));
        char byte{};
        CHECK(::read(fd, &byte, 1) == 1);
        CHECK(byte == 'x');
        ++fd_notifications;
        CHECK(loop->unwatch_fd(watch));
    });
    REQUIRE(watch);
    REQUIRE(::write(sockets.descriptors[1], "x", 1) == 1);
    REQUIRE(first.release());
    REQUIRE(second.release());
    REQUIRE(pump_until_exit(*loop, first_notifications));
    REQUIRE(pump_until_exit(*loop, second_notifications));
    REQUIRE(pump_until_exit(*loop, fd_notifications));
    CHECK(EventLoopTestAccess::unwatch_process(*loop, first.pid()));
    CHECK(EventLoopTestAccess::unwatch_process(*loop, second.pid()));
    int status{};
    CHECK(first.reap(status) > 0);
    CHECK(second.reap(status) > 0);
}
#endif
