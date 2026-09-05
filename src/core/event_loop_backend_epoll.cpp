#if defined(__linux__)

#  include "event_loop_backend.hpp"
#  include "event_loop_backend_epoll_priv.hpp"
#  include "logging.hpp"

#  include <algorithm>
#  include <cerrno>
#  include <cstring>
#  include <limits>
#  include <memory>
#  include <unordered_map>
#  include <utility>

#  include <sys/epoll.h>
#  include <sys/eventfd.h>
#  include <sys/syscall.h>
#  include <sys/types.h>

#  include <unistd.h>

namespace jb::core::priv {

auto EpollProcessOperations::open_pidfd(int process_id) -> int
{
#  if defined(SYS_pidfd_open)
    return static_cast<int>(::syscall(SYS_pidfd_open, process_id, 0));
#  else
    (void)process_id;
    errno = ENOSYS;
    return -1;
#  endif
}

auto EpollProcessOperations::control(int poller, int operation, int fd, epoll_event* event) -> int
{
    return ::epoll_ctl(poller, operation, fd, event);
}

class EpollBackend final : public Backend {
public:

    explicit EpollBackend(std::shared_ptr<EpollProcessOperations> operations)
        : _process_operations(std::move(operations))
    {}

    ~EpollBackend() override
    {
        // Backend destruction releases monitoring resources, never child-reaping ownership.
        for (auto const& [pid, fd] : _process_fds) {
            ::close(fd);
        }
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
        ev.events   = EPOLLIN;
        ev.data.u64 = static_cast<std::uint64_t>(_wake_fd);
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
        if (_fd_processes.contains(fd)) {
            return false;
        }
        epoll_event ev{};
        ev.events = to_epoll(events);
        if (trigger_mode == FdTriggerMode::Edge) {
            ev.events |= EPOLLET;
        }
        ev.data.u64 = static_cast<std::uint64_t>(fd);

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
        if (_fd_processes.contains(fd)) {
            return false;
        }
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

    auto add_process(std::int64_t process_id) -> ProcessRegistrationResult override
    {
        if (process_id <= 0 || process_id > std::numeric_limits<pid_t>::max()) {
            return ProcessRegistrationResult::Failed;
        }
        if (_process_fds.contains(process_id)) {
            return ProcessRegistrationResult::Added;
        }
        auto const fd = _process_operations->open_pidfd(static_cast<int>(process_id));
        if (fd < 0) {
            // ENODEV means the kernel lacks the anonymous-inode support required by pidfds.
            return errno == ENOSYS || errno == ENODEV ? ProcessRegistrationResult::Unsupported
                                                      : ProcessRegistrationResult::Failed;
        }

        // Prepare both lookups before native registration so syscall failure can roll back the complete entry.
        _process_fds.emplace(process_id, fd);
        _fd_processes.emplace(fd, process_id);
        epoll_event event{};
        // pidfds stay readable after exit. One-shot delivery prevents spinning when removal fails;
        // the tag prevents a numeric fd/PID collision from becoming an ordinary fd notification.
        event.events   = EPOLLIN | EPOLLONESHOT;
        event.data.u64 = kProcessTag | static_cast<std::uint64_t>(fd);
        if (_process_operations->control(_epoll_fd, EPOLL_CTL_ADD, fd, &event) != 0) {
            _process_fds.erase(process_id);
            _fd_processes.erase(fd);
            ::close(fd);
            return ProcessRegistrationResult::Failed;
        }
        return ProcessRegistrationResult::Added;
    }

    auto remove_process(std::int64_t process_id) -> bool override
    {
        auto const entry = _process_fds.find(process_id);
        if (entry == _process_fds.end()) {
            return true;
        }
        auto const fd = entry->second;
        // Retain both maps and the descriptor on failure so EventLoop can safely retry removal.
        // A previously delivered watch is deliberately never rearmed, even along this failure path.
        if (_process_operations->control(_epoll_fd, EPOLL_CTL_DEL, fd, nullptr) != 0) {
            return false;
        }
        ::close(fd);
        _fd_processes.erase(fd);
        _process_fds.erase(entry);
        return true;
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
            auto const tag = events[i].data.u64;
            auto const fd  = static_cast<int>(tag & ~kProcessTag);
            if ((tag & kProcessTag) != 0) {
                if (auto const entry = _fd_processes.find(fd); entry != _fd_processes.end()) {
                    out[written++] = {.kind = ReadyEventKind::Process, .ident = entry->second, .events = {}};
                }
                continue;
            }
            if (fd == _wake_fd) {
                if (!drain_wake_fd()) {
                    return -1;
                }
                continue; // not a user event
            }

            out[written++] = {.kind   = ReadyEventKind::FileDescriptor,
                              .ident  = fd,
                              .events = from_epoll(events[i].events)};
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
    static constexpr std::uint64_t          kProcessTag = std::uint64_t{1} << 63U;
    std::shared_ptr<EpollProcessOperations> _process_operations;
    std::unordered_map<std::int64_t, int>   _process_fds;
    std::unordered_map<int, std::int64_t>   _fd_processes;
    int                                     _epoll_fd{-1};
    int                                     _wake_fd{-1};

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
    return make_epoll_backend(std::make_shared<EpollProcessOperations>());
}

auto make_epoll_backend(std::shared_ptr<EpollProcessOperations> operations) -> std::unique_ptr<Backend>
{
    if (!operations) {
        return nullptr;
    }
    auto backend = std::make_unique<EpollBackend>(std::move(operations));
    if (!backend->initialize()) {
        return nullptr;
    }
    return backend;
}

} // namespace jb::core::priv

#endif // __linux__
