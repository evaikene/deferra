#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)

#include "event_loop_backend.hpp"
#include "event_loop_types.hpp"

#include <algorithm>
#include <cerrno>
#include <memory>
#include <stdexcept>
#include <unordered_map>

#include <fcntl.h>
#include <sys/event.h>
#include <sys/time.h>
#include <unistd.h>

namespace jb::core::priv {

class KqueueBackend final : public Backend {
public:

    KqueueBackend()
    {
        _kq = ::kqueue();
        if (_kq < 0) {
            throw std::runtime_error{"kqueue failed"};
        }
        if (::fcntl(_kq, F_SETFD, FD_CLOEXEC) < 0) {
            ::close(_kq);
            throw std::runtime_error{"fcntl F_SETFD FD_CLOEXEC failed"};
        }

        // register the user-event wakeup channel once
        struct kevent ev;
        EV_SET(&ev, kWakeIdent, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, nullptr);
        if (::kevent(_kq, &ev, 1, nullptr, 0, nullptr) < 0) {
            ::close(_kq);
            throw std::runtime_error{"kevent EVFILT_USER register failed"};
        }
    }

    ~KqueueBackend() override { ::close(_kq); }

    void add_fd(int fd, FdEvents events) override
    {
        FdEvents old;
        if (auto it = _registered.find(fd); it != _registered.end()) {
            old = it->second;
        }

        struct kevent changes[2];
        int           n = 0;

        // reconcile READ filter
        if (events.test(FdEvent::Read) && !old.test(FdEvent::Read)) {
            EV_SET(&changes[n++], fd, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, nullptr);
        }
        else if (!events.test(FdEvent::Read) && old.test(FdEvent::Read)) {
            EV_SET(&changes[n++], fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
        }

        // reconcile WRITE filter
        if (events.test(FdEvent::Write) && !old.test(FdEvent::Write)) {
            EV_SET(&changes[n++], fd, EVFILT_WRITE, EV_ADD | EV_CLEAR, 0, 0, nullptr);
        }
        else if (!events.test(FdEvent::Write) && old.test(FdEvent::Write)) {
            EV_SET(&changes[n++], fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
        }

        if (n > 0 && ::kevent(_kq, changes, n, nullptr, 0, nullptr) < 0) {
            throw std::runtime_error{"kevent register failed"};
        }

        if (events.none()) {
            _registered.erase(fd);
        }
        else {
            _registered[fd] = events;
        }
    }

    void remove_fd(int fd) override
    {
        auto it = _registered.find(fd);
        if (it == _registered.end()) {
            return;
        }

        struct kevent changes[2];
        int           n = 0;
        if (it->second.test(FdEvent::Read)) {
            EV_SET(&changes[n++], fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
        }
        if (it->second.test(FdEvent::Write)) {
            EV_SET(&changes[n++], fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
        }

        ::kevent(_kq, changes, n, nullptr, 0, nullptr);
        _registered.erase(it);
    }

    auto poll(ReadyEvent* out, int max_events, int timeout_ms) -> int override
    {
        static constexpr int  kMaxEvents{64};
        static constexpr long kMSecInSec{1'000};
        static constexpr long kNSecInMSec{1'000'000};

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
            if (errno == EINTR) {
                return 0;
            }
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

    void wakeup() override
    {
        struct kevent ev;
        EV_SET(&ev, kWakeIdent, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
        while (::kevent(_kq, &ev, 1, nullptr, 0, nullptr) < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::runtime_error{"kevent wakeup failed"};
        }
    }

    private:

        // A fixed ident for the user-event wakeup channel
        static constexpr std::uintptr_t kWakeIdent{0};

        int                               _kq{-1};
        std::unordered_map<int, FdEvents> _registered;
    };

    auto make_backend() -> std::unique_ptr<Backend> { return std::make_unique<KqueueBackend>(); }

} // namespace jb::core::priv

#endif // __APPLE__ || __FreeBSD__ || __OpenBSD__ || __NetBSD__
