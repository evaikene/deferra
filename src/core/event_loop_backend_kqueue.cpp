#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)

#  include "event_loop_backend.hpp"
#  include "event_loop_types.hpp"
#  include "logging.hpp"

#  include <algorithm>
#  include <cerrno>
#  include <cstring>
#  include <memory>
#  include <unordered_map>

#  include <fcntl.h>
#  include <sys/event.h>
#  include <sys/time.h>
#  include <unistd.h>

namespace jb::core::priv {

class KqueueBackend final : public Backend {
public:

    ~KqueueBackend() override
    {
        if (_kq >= 0) {
            ::close(_kq);
        }
    }

    auto initialize() -> bool
    {
        _kq = ::kqueue();
        if (_kq < 0) {
            auto const error = errno;
            log_error("kqueue failed: {}", std::strerror(error));
            return false;
        }

        for (;;) {
            if (::fcntl(_kq, F_SETFD, FD_CLOEXEC) == 0) {
                break;
            }

            auto const error = errno;
            if (error == EINTR) {
                continue;
            }

            log_error("fcntl F_SETFD FD_CLOEXEC failed: {}", std::strerror(error));
            close_kqueue();
            return false;
        }

        // register the user-event wakeup channel once
        struct kevent ev;
        EV_SET(&ev, kWakeIdent, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, nullptr);
        for (;;) {
            if (::kevent(_kq, &ev, 1, nullptr, 0, nullptr) == 0) {
                return true;
            }

            auto const error = errno;
            if (error == EINTR) {
                continue;
            }

            log_error("kevent EVFILT_USER register failed: {}", std::strerror(error));
            close_kqueue();
            return false;
        }
    }

    auto add_fd(int fd, FdEvents events) -> bool override
    {
        if (!reconcile_filter(fd, FdEvent::Read, EVFILT_READ, events.test(FdEvent::Read))) {
            return false;
        }
        return reconcile_filter(fd, FdEvent::Write, EVFILT_WRITE, events.test(FdEvent::Write));
    }

    auto remove_fd(int fd) -> bool override
    {
        auto it = _registered.find(fd);
        if (it == _registered.end()) {
            return true;
        }

        auto success = true;
        if (it->second.test(FdEvent::Read) && !reconcile_filter(fd, FdEvent::Read, EVFILT_READ, false)) {
            success = false;
        }
        if (auto current = _registered.find(fd); current != _registered.end() && current->second.test(FdEvent::Write) &&
                                                 !reconcile_filter(fd, FdEvent::Write, EVFILT_WRITE, false)) {
            success = false;
        }

        return success;
    }

    auto poll(ReadyEvent* out, int max_events, int timeout_ms) -> int override
    {
        static constexpr int  kMaxEvents{64};
        static constexpr long kMSecInSec{1000};
        static constexpr long kNSecInMSec{1000000};

        auto max = std::min(max_events, kMaxEvents);

        struct timespec  ts;
        struct timespec* ts_ptr = nullptr;
        if (timeout_ms >= 0) {
            ts.tv_sec  = timeout_ms / kMSecInSec;
            ts.tv_nsec = (timeout_ms % kMSecInSec) * kNSecInMSec;
            ts_ptr     = &ts;
        }

        struct kevent events[kMaxEvents];
        auto          n = ::kevent(_kq, nullptr, 0, events, max, ts_ptr);
        if (n < 0) {
            auto const error = errno;
            if (error == EINTR) {
                return 0;
            }
            log_error("kevent poll failed: {}", std::strerror(error));
            return -1;
        }

        // coalesce per-filter events for the same fd into one ReadyEvent
        int written = 0;
        for (int i = 0; i < n; ++i) {
            auto const& ev = events[i];

            if (ev.filter == EVFILT_USER && ev.ident == kWakeIdent) {
                continue; // wakeup, not a user event
            }

            auto     fd = static_cast<int>(ev.ident);
            FdEvents mask;
            if (ev.filter == EVFILT_READ) {
                mask.set(FdEvent::Read);
            }
            if (ev.filter == EVFILT_WRITE) {
                mask.set(FdEvent::Write);
            }
            if ((ev.flags & EV_EOF) != 0) {
                mask.set(FdEvent::Read);
            }

            bool merged = false;
            for (int j = 0; j < written; ++j) {
                if (out[j].fd == fd) {
                    out[j].events.set(mask);
                    merged = true;
                    break;
                }
            }
            if (!merged) {
                out[written++] = {.fd = fd, .events = mask};
            }
        }

        return written;
    }

    auto wakeup() -> bool override
    {
        struct kevent ev;
        EV_SET(&ev, kWakeIdent, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
        while (::kevent(_kq, &ev, 1, nullptr, 0, nullptr) < 0) {
            auto const error = errno;
            if (error == EINTR) {
                continue;
            }
            log_error("kevent wakeup failed: {}", std::strerror(error));
            return false;
        }
        return true;
    }

private:

    // A fixed ident for the user-event wakeup channel
    static constexpr std::uintptr_t kWakeIdent{0};

    int                               _kq{-1};
    std::unordered_map<int, FdEvents> _registered;

    void close_kqueue()
    {
        if (_kq >= 0) {
            ::close(_kq);
            _kq = -1;
        }
    }

    auto apply_filter_change(int fd, std::int16_t filter, std::uint16_t flags) -> bool
    {
        struct kevent change;
        EV_SET(&change, fd, filter, flags, 0, 0, nullptr);

        for (;;) {
            if (::kevent(_kq, &change, 1, nullptr, 0, nullptr) == 0) {
                return true;
            }

            auto const error = errno;
            if (error == EINTR) {
                continue;
            }
            if ((flags & EV_DELETE) != 0U && (error == ENOENT || error == EBADF)) {
                return true;
            }

            log_error("kevent filter change failed for fd {}: {}", fd, std::strerror(error));
            return false;
        }
    }

    auto reconcile_filter(int fd, FdEvent event, std::int16_t filter, bool requested) -> bool
    {
        FdEvents current;
        if (auto const it = _registered.find(fd); it != _registered.end()) {
            current = it->second;
        }

        if (current.test(event) == requested) {
            return true;
        }

        auto const flags = static_cast<std::uint16_t>(requested ? EV_ADD | EV_CLEAR : EV_DELETE);
        if (!apply_filter_change(fd, filter, flags)) {
            return false;
        }

        if (requested) {
            current.set(event);
        }
        else {
            current.clear(event);
        }

        if (current.none()) {
            _registered.erase(fd);
        }
        else {
            _registered[fd] = current;
        }

        return true;
    }
};

auto make_backend() -> std::unique_ptr<Backend>
{
    auto backend = std::make_unique<KqueueBackend>();
    if (!backend->initialize()) {
        return nullptr;
    }
    return backend;
}

} // namespace jb::core::priv

#endif // __APPLE__ || __FreeBSD__ || __OpenBSD__ || __NetBSD__
