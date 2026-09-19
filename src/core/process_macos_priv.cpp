#include "process_posix_priv.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <limits>
#include <string_view>

#include <dirent.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/wait.h>

namespace jb::core::priv {
namespace {
auto set_close_on_exec(int fd) noexcept -> bool
{
    int result;
    do {
        result = ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    } while (result < 0 && errno == EINTR);
    return result == 0;
}

// Creation has not transferred ownership to ProcessDescriptors yet. Roll back both endpoints on setup failure.
auto discard_pair(int* descriptors) noexcept -> int
{
    auto const error = errno;
    close_process_fd(descriptors[0]);
    close_process_fd(descriptors[1]);
    errno = error;
    return -1;
}
} // namespace

auto ProcessOperations::child_options() noexcept -> ProcessChildOptions
{
    // macOS has no strict no-new-privileges equivalent. The common preparation rejects a strict request.
    // A null close_range selects the bounded close loop; the supported SDK has no public close-from operation.
    return {.effective_uid = ::geteuid};
}

auto ProcessOperations::descriptor_close_limit(unsigned int& limit) noexcept -> int
{
    auto* directory = ::opendir("/dev/fd");
    if (!directory) {
        return -1;
    }

    // A lowered RLIMIT_NOFILE does not invalidate already-open higher descriptors. Include the live fd table before
    // combining it with the allocation ceiling that covers ordinary opens racing with this parent-side scan.
    auto highest_open = 3U;
    int  scan_error{0};
    while (true) {
        errno       = 0;
        auto* entry = ::readdir(directory);
        if (!entry) {
            scan_error = errno;
            break;
        }

        auto const   name = std::string_view{entry->d_name};
        unsigned int descriptor;
        auto const [end, error] = std::from_chars(name.begin(), name.end(), descriptor);
        if (error == std::errc{} && end == name.end()) {
            highest_open = std::max(highest_open, descriptor);
        }
    }

    if (::closedir(directory) != 0 && scan_error == 0) {
        scan_error = errno;
    }
    if (scan_error != 0) {
        errno = scan_error;
        return -1;
    }

    errno                       = 0;
    auto const allocation_limit = ::sysconf(_SC_OPEN_MAX);
    if (allocation_limit < 4) {
        if (allocation_limit >= 0 || errno == 0) {
            errno = EINVAL;
        }
        return -1;
    }

    constexpr auto descriptor_ceiling        = static_cast<unsigned int>(std::numeric_limits<int>::max()) + 1U;
    auto const     unsigned_allocation_limit = static_cast<unsigned long>(allocation_limit);
    auto const     allocation_bound          = unsigned_allocation_limit >= descriptor_ceiling
                                                 ? descriptor_ceiling
                                                 : static_cast<unsigned int>(unsigned_allocation_limit);
    auto const live_bound = highest_open == std::numeric_limits<int>::max() ? descriptor_ceiling : highest_open + 1U;
    limit                 = std::max(allocation_bound, live_bound);
    return 0;
}

auto ProcessOperations::open_null(int flags) noexcept -> int
{
    return ::open("/dev/null", flags | O_CLOEXEC);
}

auto ProcessOperations::make_pipe(int* descriptors) noexcept -> int
{
    if (::pipe(descriptors) != 0) {
        return -1;
    }
    if (!set_close_on_exec(descriptors[0]) || !set_close_on_exec(descriptors[1])) {
        return discard_pair(descriptors);
    }
    return 0;
}

auto ProcessOperations::make_gate(int* descriptors) noexcept -> int
{
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors) != 0) {
        return -1;
    }
    if (!set_close_on_exec(descriptors[0]) || !set_close_on_exec(descriptors[1])) {
        return discard_pair(descriptors);
    }

    // Suppress only this socket's sends; do not change dispositions, masks, or pending host signals.
    int const enabled{1};
    if (::setsockopt(descriptors[0], SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) != 0) {
        return discard_pair(descriptors);
    }
    return 0;
}

auto ProcessOperations::create_child() noexcept -> pid_t
{
    // Public fork() runs host/runtime pthread_atfork handlers internally. Our async-signal-safe guarantee begins
    // only after it returns in the child; no private libc entry point or deprecated vfork() bypasses that boundary.
    return ::fork();
}

auto ProcessOperations::establish_group(pid_t pid) noexcept -> int
{
    return ::setpgid(pid, pid);
}

auto ProcessOperations::release_gate(int fd, pid_t /*pid*/) -> ssize_t
{
    // Suppress only SIGPIPE caused by this send. Never inspect or consume host pending signals.
    char const permission{'x'};
    return ::send(fd, &permission, 1, 0);
}

auto ProcessOperations::read_output(int fd, void* buffer, std::size_t size) noexcept -> ssize_t
{
    return ::read(fd, buffer, size);
}

auto ProcessOperations::monotonic_now() noexcept -> TimePoint
{
    return Clock::now();
}

auto ProcessOperations::signal_group(pid_t group_id, int signal) noexcept -> int
{
    return ::kill(-group_id, signal);
}

auto ProcessOperations::signal_process(pid_t process_id, int signal) noexcept -> int
{
    return ::kill(process_id, signal);
}

auto ProcessOperations::wait_process(pid_t process_id, int* status, int options) noexcept -> pid_t
{
    return ::waitpid(process_id, status, options);
}
} // namespace jb::core::priv
