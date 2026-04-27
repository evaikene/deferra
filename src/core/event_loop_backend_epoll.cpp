#if defined(__linux__)

#include "event_loop_backend.hpp"

#include <algorithm>
#include <cerrno>
#include <stdexcept>

#include <sys/epoll.h>
#include <sys/eventfd.h>

#include <unistd.h>

namespace jb::core::priv {

class EpollBackend final : public Backend {
public:

    EpollBackend()
    {
        _epoll_fd = ::epoll_create1(EPOLL_CLOEXEC);
        if (_epoll_fd < 0) {
            throw std::runtime_error{"epoll_create1 failed"};
        }

        _wake_fd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        if (_wake_fd < 0) {
            ::close(_epoll_fd);
            throw std::runtime_error{"eventfd failed"};
        }

        // register wakeup fd once
        epoll_event ev{};
        ev.events  = EPOLLIN;
        ev.data.fd = _wake_fd;
        if (::epoll_ctl(_epoll_fd, EPOLL_CTL_ADD, _wake_fd, &ev) < 0) {
            ::close(_wake_fd);
            ::close(_epoll_fd);
            throw std::runtime_error{"epoll_ctl ADD failed"};
        }
    }

    ~EpollBackend() override
    {
        ::close(_wake_fd);
        ::close(_epoll_fd);
    }

    void add_fd(int fd, FdEvents events) override
    {
        epoll_event ev{};
        ev.events  = to_epoll(events) | EPOLLET;
        ev.data.fd = fd;

        if (::epoll_ctl(_epoll_fd, EPOLL_CTL_MOD, fd, &ev) < 0) {
            if (errno != ENOENT) {
                throw std::runtime_error{"epoll_ctl MOD failed"};
            }
            if (::epoll_ctl(_epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) {
                throw std::runtime_error{"epoll_ctl ADD failed"};
            }
        }
    }

    void remove_fd(int fd) override { ::epoll_ctl(_epoll_fd, EPOLL_CTL_DEL, fd, nullptr); }

    auto poll(ReadyEvent* out, int max_events, int timeout_ms) -> int override
    {
        static constexpr int kMaxEvents{64};

        epoll_event events[kMaxEvents];
        auto max = std::min(max_events, kMaxEvents);

        auto n = ::epoll_wait(_epoll_fd, events, max, timeout_ms);
        if (n < 0) {
            if (errno == EINTR) {
                return 0;
            }
            return -1;
        }

        int written{0};
        for (int i = 0; i < n; ++i) {
            auto fd = events[i].data.fd;
            if (fd == _wake_fd) {
                drain_wake_fd();
                continue; // not a user event
            }

            out[written++] = { .fd=fd, .events=from_epoll(events[i].events) };
        }

        return written;
    }

    void wakeup() override
    {
        for (;;) {
            std::uint64_t v{1};
            auto const n = ::write(_wake_fd, &v, sizeof(v)); // atomic, lock-free
            if (n == static_cast<ssize_t>(sizeof(v))) {
                return;
            }
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    return;
                }
                throw std::runtime_error{"eventfd write failed"};
            }
            throw std::runtime_error{"eventfd write returned unexpected size"};
        }
    }

private:
    int _epoll_fd{-1};
    int _wake_fd{-1};

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
            auto const n = ::read(_wake_fd, &v, sizeof(v));
            if (n == static_cast<ssize_t>(sizeof(v))) {
                continue;
            }
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                }
                return false;
            }
            return false;
        }

        return true;
    }
};

auto make_backend() -> std::unique_ptr<Backend>
{
    return std::make_unique<EpollBackend>();
}

} // namespace jb::core::priv

#endif // __linux__
