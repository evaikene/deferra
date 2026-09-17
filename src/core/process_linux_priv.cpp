#include "process_posix_priv.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <limits>
#include <string_view>

#include <dirent.h>
#include <fcntl.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>

namespace jb::core::priv {
namespace {
auto close_descriptor_range(unsigned int first, unsigned int last) noexcept -> int
{
    // Use the kernel interface directly so the Process backend does not depend on glibc versus musl wrapper exposure.
    return static_cast<int>(::syscall(SYS_close_range, first, last, 0));
}

auto prevent_child_privilege_gain() noexcept -> int
{
    if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        return -1;
    }

    // A strict request cannot rely on a successful setter alone; confirm the irreversible bit before target exec.
    auto const enabled = ::prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0);
    if (enabled == 1) {
        return 0;
    }
    if (enabled >= 0) {
        errno = EIO;
    }
    return -1;
}
} // namespace

auto ProcessOperations::child_options() noexcept -> ProcessChildOptions
{
    return {.effective_uid              = ::geteuid,
            .close_range                = close_descriptor_range,
            .enable_privilege_hardening = prevent_child_privilege_gain};
}

auto ProcessOperations::descriptor_close_limit(unsigned int& limit) noexcept -> int
{
    auto* directory = ::opendir("/proc/self/fd");
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
    return ::pipe2(descriptors, O_CLOEXEC);
}

auto ProcessOperations::make_gate(int* descriptors) noexcept -> int
{
    return ::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, descriptors);
}

auto ProcessOperations::create_child() noexcept -> pid_t
{
    // Unlike fork(), _Fork() cannot invoke application at-fork handlers in the prepared child interval.
    return ::_Fork();
}

auto ProcessOperations::establish_group(pid_t pid) noexcept -> int
{
    return ::setpgid(pid, pid);
}

auto ProcessOperations::release_gate(int fd, pid_t /*pid*/) -> ssize_t
{
    // Suppress only SIGPIPE caused by this send. Never inspect or consume host pending signals.
    char const permission{'x'};
    return ::send(fd, &permission, 1, MSG_NOSIGNAL);
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
