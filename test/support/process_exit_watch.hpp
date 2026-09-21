#pragma once

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#if defined(__linux__)
#  include <csignal> // IWYU pragma: keep POSIX kill signal constants.
#  include <poll.h>
#  include <sys/syscall.h>
#else
#  include <cstdint>
#  include <sys/event.h>
#endif
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>

namespace jb::test {

/// Observe a coordinated live helper before releasing it or triggering daemon shutdown.
/// Darwin watches do not grant identity-safe signalling; helper alarms bound failed-test lifetimes.
class ProcessExitWatch final {
public:
    /// @throws Catch::TestFailureException if the live helper cannot be watched.
    explicit ProcessExitWatch(pid_t pid)
        : _pid{pid}
    {
#if defined(__APPLE__)
        _fd = ::kqueue();
        REQUIRE(_fd >= 0);
        struct kevent change;
        EV_SET(&change, static_cast<std::uintptr_t>(pid), EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT, 0, nullptr);
        int registered{-1};
        if (::fcntl(_fd, F_SETFD, FD_CLOEXEC) == 0) {
            do {
                registered = ::kevent(_fd, &change, 1, nullptr, 0, nullptr);
            } while (registered < 0 && errno == EINTR);
        }
        if (registered < 0) {
            auto const error = errno;
            ::close(_fd);
            FAIL("process exit watch setup failed with errno " << error);
        }
#else
        _fd = static_cast<int>(::syscall(SYS_pidfd_open, _pid, 0));
        REQUIRE(_fd >= 0);
#endif
    }

    ~ProcessExitWatch()
    {
#if defined(__linux__)
        // Fallback only: a pidfd still names the original helper after its numeric PID is reused.
        static_cast<void>(::syscall(SYS_pidfd_send_signal, _fd, SIGKILL, nullptr, 0));
#endif
        ::close(_fd);
    }

    ProcessExitWatch(ProcessExitWatch const&)                    = delete;
    auto operator=(ProcessExitWatch const&) -> ProcessExitWatch& = delete;

#if defined(__linux__)
    /// @throws Catch::TestFailureException if identity-safe orphan cleanup fails.
    void kill() const
    {
        auto const result = ::syscall(SYS_pidfd_send_signal, _fd, SIGKILL, nullptr, 0);
        REQUIRE((result == 0 || errno == ESRCH));
    }
#endif

    /// Remembers consumed exit events. Timeout bounds observation only; it never terminates the helper.
    /// @throws Catch::TestFailureException if native observation fails.
    auto exited(std::chrono::milliseconds timeout = {}) -> bool
    {
        if (_exited) {
            return true;
        }
        auto const deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            auto const remaining =
                std::max(std::chrono::ceil<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()),
                         std::chrono::milliseconds::zero());
#if defined(__APPLE__)
            struct kevent   event;
            struct timespec wait{.tv_sec  = static_cast<time_t>(remaining.count() / 1000),
                                 .tv_nsec = static_cast<long>((remaining.count() % 1000) * 1000000)};
            auto const      ready = ::kevent(_fd, nullptr, 0, &event, 1, &wait);
#else
            pollfd     descriptor{.fd = _fd, .events = POLLIN, .revents = 0};
            auto const ready = ::poll(&descriptor, 1, static_cast<int>(remaining.count()));
#endif
            if (ready < 0 && errno == EINTR) {
                if (std::chrono::steady_clock::now() >= deadline) {
                    return false;
                }
                continue;
            }
            REQUIRE(ready >= 0);
            if (ready == 0) {
                return false;
            }
#if defined(__APPLE__)
            REQUIRE(event.filter == EVFILT_PROC);
            REQUIRE(event.ident == static_cast<std::uintptr_t>(_pid));
            REQUIRE((event.flags & EV_ERROR) == 0);
            REQUIRE((event.fflags & NOTE_EXIT) != 0);
#else
            REQUIRE((descriptor.revents & POLLIN) != 0);
#endif
            _exited = true;
            return true;
        }
    }

private:
    pid_t _pid;
    int   _fd{-1};
    bool  _exited{false};
};

} // namespace jb::test
