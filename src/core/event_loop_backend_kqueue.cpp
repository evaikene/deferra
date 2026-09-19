#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)

#  include "event_loop_backend.hpp"
#  include "event_loop_backend_kqueue_priv.hpp"
#  include "event_loop_types.hpp"
#  include "logging.hpp"

#  include <algorithm>
#  include <atomic>
#  include <cerrno>
#  include <cstring>
#  include <limits>
#  include <memory>
#  include <unordered_map>
#  include <unordered_set>
#  include <utility>

#  include <fcntl.h>
#  include <sys/event.h>
#  include <sys/time.h>
#  include <unistd.h>

namespace jb::core::priv {

class KqueueBackend final : public Backend {
public:

#  if defined(__APPLE__)
    explicit KqueueBackend(std::shared_ptr<KqueueProcessOperations> operations)
        : _process_operations{std::move(operations)}
    {}
#  endif

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

    auto add_fd(int fd, FdEvents events, FdTriggerMode trigger_mode) -> bool override
    {
        if (!_healthy.load(std::memory_order_relaxed)) {
            return false;
        }

        auto current = KqueueFdRegistration{
            .events       = {},
            .trigger_mode = trigger_mode,
        };
        if (auto const it = _registered.find(fd); it != _registered.end()) {
            current = it->second;
        }
        auto const requested = KqueueFdRegistration{
            .events       = events,
            .trigger_mode = trigger_mode,
        };
        // EventLoop skips ordinary callback-only replacements. Reaching the backend must apply
        // every requested filter, including after failed removal followed by descriptor reuse.
        if (transition_fd(fd, current, requested, true) != KqueueTransitionStatus::Applied) {
            return false;
        }

        _registered.insert_or_assign(fd, requested);
        return true;
    }

    auto remove_fd(int fd) -> bool override
    {
        if (!_healthy.load(std::memory_order_relaxed)) {
            return false;
        }

        auto const current = _registered.find(fd);
        if (current == _registered.end()) {
            return true;
        }
        auto const requested = KqueueFdRegistration{
            .events       = {},
            .trigger_mode = current->second.trigger_mode,
        };
        if (transition_fd(fd, current->second, requested) != KqueueTransitionStatus::Applied) {
            return false;
        }

        _registered.erase(current);
        return true;
    }

    auto add_process(std::int64_t process_id) -> ProcessRegistrationResult override
    {
#  if defined(__APPLE__)
        if (!_healthy.load(std::memory_order_relaxed) || process_id <= 0 ||
            process_id > std::numeric_limits<pid_t>::max()) {
            return ProcessRegistrationResult::Failed;
        }

        // Keep logical ownership after EV_ONESHOT delivery: a duplicate must never rearm a delivered watch.
        auto const [entry, inserted] = _processes.insert(process_id);
        if (!inserted) {
            return ProcessRegistrationResult::Added;
        }
        if (change_process_filter(process_id, EV_ADD | EV_ONESHOT)) {
            return ProcessRegistrationResult::Added;
        }

        _processes.erase(entry);
        return ProcessRegistrationResult::Failed;
#  else
        (void)process_id;
        return ProcessRegistrationResult::Unsupported;
#  endif
    }

    auto remove_process(std::int64_t process_id) -> bool override
    {
#  if defined(__APPLE__)
        if (!_healthy.load(std::memory_order_relaxed)) {
            return false;
        }
        auto const entry = _processes.find(process_id);
        if (entry == _processes.end()) {
            return true;
        }
        if (!change_process_filter(process_id, EV_DELETE)) {
            return false;
        }
        _processes.erase(entry);
#  else
        (void)process_id;
#  endif
        return true;
    }

    auto poll(ReadyEvent* out, int max_events, int timeout_ms) -> int override
    {
        if (!_healthy.load(std::memory_order_relaxed)) {
            return -1;
        }

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

#  if defined(__APPLE__)
            // PID and fd values occupy independent namespaces. Never coalesce a process exit into fd readiness.
            if (ev.filter == EVFILT_PROC) {
                out[written++] = {.kind   = ReadyEventKind::Process,
                                  .ident  = static_cast<std::int64_t>(ev.ident),
                                  .events = {}};
                continue;
            }
#  endif

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
                if (out[j].kind == ReadyEventKind::FileDescriptor && out[j].ident == fd) {
                    out[j].events.set(mask);
                    merged = true;
                    break;
                }
            }
            if (!merged) {
                out[written++] = {.kind = ReadyEventKind::FileDescriptor, .ident = fd, .events = mask};
            }
        }

        return written;
    }

    auto wakeup() -> bool override
    {
        if (!_healthy.load(std::memory_order_relaxed)) {
            return false;
        }

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

    int                                           _kq{-1};
    std::atomic_bool                              _healthy{true};
    std::unordered_map<int, KqueueFdRegistration> _registered;

#  if defined(__APPLE__)
    std::shared_ptr<KqueueProcessOperations> _process_operations;
    std::unordered_set<std::int64_t>         _processes;

    auto change_process_filter(std::int64_t process_id, std::uint16_t flags) -> bool
    {
        for (;;) {
            if (_process_operations->control(_kq, process_id, flags) == 0) {
                return true;
            }
            auto const error = errno;
            if (error == EINTR) {
                continue;
            }
            // Delivery deletes a one-shot native filter before EventLoop asks to remove its logical registration.
            if ((flags & EV_DELETE) != 0U && error == ENOENT) {
                return true;
            }
            log_error("kevent process filter change failed: {}", error);
            return false;
        }
    }
#  endif

    void close_kqueue()
    {
        if (_kq >= 0) {
            ::close(_kq);
            _kq = -1;
        }
    }

    auto apply_filter_change(int fd, std::int16_t filter, std::uint16_t flags) const -> bool
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

    static auto filter_flags(KqueueFilterMode mode) noexcept -> std::uint16_t
    {
        auto flags = static_cast<std::uint16_t>(mode == KqueueFilterMode::Disabled ? EV_DELETE : EV_ADD);
        if (mode == KqueueFilterMode::Edge) {
            flags = static_cast<std::uint16_t>(flags | EV_CLEAR);
        }
        return flags;
    }

    auto transition_fd(int                         fd,
                       KqueueFdRegistration const& current,
                       KqueueFdRegistration const& requested,
                       bool                        refresh_enabled = false) -> KqueueTransitionStatus
    {
        auto const status = transition_kqueue_filters(
            current,
            requested,
            [this, fd](FdEvent event, KqueueFilterMode mode) {
                auto const filter = static_cast<std::int16_t>(event == FdEvent::Read ? EVFILT_READ : EVFILT_WRITE);
                // macOS does not persist EV_CLEAR changes from an in-place
                // EV_ADD, so the transition helper deletes an enabled filter
                // before adding it in a different trigger mode.
                return apply_filter_change(fd, filter, filter_flags(mode));
            },
            refresh_enabled);

        if (status == KqueueTransitionStatus::RollbackFailed) {
            log_error("kevent filter rollback failed for fd {}; event-loop backend is unusable", fd);
            _healthy.store(false, std::memory_order_relaxed);
        }

        return status;
    }
};

#  if defined(__APPLE__)
auto KqueueProcessOperations::control(int poller, std::int64_t process_id, std::uint16_t flags) -> int
{
    struct kevent change;
    EV_SET(&change, process_id, EVFILT_PROC, flags, NOTE_EXIT, 0, nullptr);
    return ::kevent(poller, &change, 1, nullptr, 0, nullptr);
}

auto make_kqueue_backend(std::shared_ptr<KqueueProcessOperations> operations) -> std::unique_ptr<Backend>
{
    if (!operations) {
        return nullptr;
    }
    auto backend = std::make_unique<KqueueBackend>(std::move(operations));
    if (!backend->initialize()) {
        return nullptr;
    }
    return backend;
}
#  endif

auto make_backend() -> std::unique_ptr<Backend>
{
#  if defined(__APPLE__)
    return make_kqueue_backend(std::make_shared<KqueueProcessOperations>());
#  else
    auto backend = std::make_unique<KqueueBackend>();
    if (!backend->initialize()) {
        return nullptr;
    }
    return backend;
#  endif
}

} // namespace jb::core::priv

#endif // __APPLE__ || __FreeBSD__ || __OpenBSD__ || __NetBSD__
