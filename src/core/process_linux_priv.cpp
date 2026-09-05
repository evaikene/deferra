#include "process_posix_priv.hpp"

#include <fcntl.h>
#include <sys/socket.h>

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
} // namespace jb::core::priv
