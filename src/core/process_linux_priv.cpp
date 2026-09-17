#include "process_posix_priv.hpp"

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/wait.h>

namespace jb::core::priv {
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
