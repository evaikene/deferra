#if defined(__linux__)

#  include "event_loop_backend.hpp"
#  include "logging.hpp"

#  include <algorithm>
#  include <cerrno>
#  include <cstring>
#  include <memory>

#  include <sys/epoll.h>
#  include <sys/eventfd.h>

#  include <unistd.h>

namespace jb::core::priv {

class EpollBackend final : public Backend {
public:

    ~EpollBackend() override
    {
        if (_wake_fd >= 0) {
            ::close(_wake_fd);
        }
        if (_epoll_fd >= 0) {
            ::close(_epoll_fd);
        }
    }

    auto initialize() -> bool
    {
        _epoll_fd = ::epoll_create1(EPOLL_CLOEXEC);
        if (_epoll_fd < 0) {
            auto const error = errno;
            log_error("epoll_create1 failed: {}", std::strerror(error));
            return false;
        }

        _wake_fd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        if (_wake_fd < 0) {
            auto const error = errno;
            log_error("eventfd failed: {}", std::strerror(error));
            close_descriptors();
            return false;
        }

        // register wakeup fd once
        epoll_event ev{};
        ev.events  = EPOLLIN;
        ev.data.fd = _wake_fd;
        if (::epoll_ctl(_epoll_fd, EPOLL_CTL_ADD, _wake_fd, &ev) < 0) {
            auto const error = errno;
            log_error("epoll_ctl ADD wake fd failed: {}", std::strerror(error));
            close_descriptors();
            return false;
        }

        return true;
    }

    auto add_fd(int fd, FdEvents events, FdTriggerMode trigger_mode) -> bool override
    {
        epoll_event ev{};
        ev.events = to_epoll(events);
        if (trigger_mode == FdTriggerMode::Edge) {
            ev.events |= EPOLLET;
        }
        ev.data.fd = fd;

        if (::epoll_ctl(_epoll_fd, EPOLL_CTL_MOD, fd, &ev) < 0) {
            auto const modify_error = errno;
            if (modify_error != ENOENT) {
                log_error("epoll_ctl MOD failed for fd {}: {}", fd, std::strerror(modify_error));
                return false;
            }
            if (::epoll_ctl(_epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) {
                auto const add_error = errno;
                log_error("epoll_ctl ADD failed for fd {}: {}", fd, std::strerror(add_error));
                return false;
            }
        }

        return true;
    }

    auto remove_fd(int fd) -> bool override
    {
        if (::epoll_ctl(_epoll_fd, EPOLL_CTL_DEL, fd, nullptr) == 0) {
            return true;
        }

        auto const error = errno;
        if (error == ENOENT || error == EBADF) {
            return true;
        }

        log_error("epoll_ctl DEL failed for fd {}: {}", fd, std::strerror(error));
        return false;
    }

    auto poll(ReadyEvent* out, int max_events, int timeout_ms) -> int override
    {
        static constexpr int kMaxEvents{64};

        epoll_event events[kMaxEvents];
        auto        max = std::min(max_events, kMaxEvents);

        auto n = ::epoll_wait(_epoll_fd, events, max, timeout_ms);
        if (n < 0) {
            auto const error = errno;
            if (error == EINTR) {
                return 0;
            }
            log_error("epoll_wait failed: {}", std::strerror(error));
            return -1;
        }

        int written{0};
        for (int i = 0; i < n; ++i) {
            auto fd = events[i].data.fd;
            if (fd == _wake_fd) {
                if (!drain_wake_fd()) {
                    return -1;
                }
                continue; // not a user event
            }

            out[written++] = {.fd = fd, .events = from_epoll(events[i].events)};
        }

        return written;
    }

    auto wakeup() -> bool override
    {
        for (;;) {
            std::uint64_t v{1};
            auto const    n = ::write(_wake_fd, &v, sizeof(v)); // atomic, lock-free
            if (n == static_cast<ssize_t>(sizeof(v))) {
                return true;
            }
            if (n < 0) {
                auto const error = errno;
                if (error == EINTR) {
                    continue;
                }
                if (error == EAGAIN || error == EWOULDBLOCK) {
                    return true;
                }
                log_error("eventfd write failed: {}", std::strerror(error));
                return false;
            }
            log_error("eventfd write returned unexpected size: {}", n);
            return false;
        }
    }

private:
    int _epoll_fd{-1};
    int _wake_fd{-1};

    void close_descriptors()
    {
        if (_wake_fd >= 0) {
            ::close(_wake_fd);
            _wake_fd = -1;
        }
        if (_epoll_fd >= 0) {
            ::close(_epoll_fd);
            _epoll_fd = -1;
        }
    }

    static auto to_epoll(FdEvents e) -> std::uint32_t
    {
        std::uint32_t out{0};
        if (e.test(FdEvent::Read)) {
            out |= EPOLLIN;
        }
        if (e.test(FdEvent::Write)) {
            out |= EPOLLOUT;
        }

        return out;
    }

    static auto from_epoll(std::uint32_t e) -> FdEvents
    {
        FdEvents out;
        if ((e & (EPOLLIN | EPOLLHUP | EPOLLERR)) != 0) {
            out.set(FdEvent::Read);
        }
        if ((e & EPOLLOUT) != 0) {
            out.set(FdEvent::Write);
        }
        return out;
    }

    auto drain_wake_fd() const -> bool
    {
        for (;;) {
            std::uint64_t v;
            auto const    n = ::read(_wake_fd, &v, sizeof(v));
            if (n == static_cast<ssize_t>(sizeof(v))) {
                continue;
            }
            if (n < 0) {
                auto const error = errno;
                if (error == EINTR) {
                    continue;
                }
                if (error == EAGAIN || error == EWOULDBLOCK) {
                    break;
                }
                log_error("eventfd read failed: {}", std::strerror(error));
                return false;
            }
            log_error("eventfd read returned unexpected size: {}", n);
            return false;
        }

        return true;
    }
};

auto make_backend() -> std::unique_ptr<Backend>
{
    auto backend = std::make_unique<EpollBackend>();
    if (!backend->initialize()) {
        return nullptr;
    }
    return backend;
}

} // namespace jb::core::priv

#endif // __linux__
