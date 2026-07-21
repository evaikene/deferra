#pragma once

#include <cerrno>

#include <fcntl.h>
#include <sys/socket.h>

namespace jb::net::priv {

inline auto get_descriptor_flags(int fd, int command) -> int
{
    for (;;) {
        auto const flags = ::fcntl(fd, command, 0);
        if (flags >= 0 || errno != EINTR) {
            return flags;
        }
    }
}

inline auto set_descriptor_flags(int fd, int command, int flags) -> bool
{
    for (;;) {
        if (::fcntl(fd, command, flags) == 0) {
            return true;
        }
        if (errno != EINTR) {
            return false;
        }
    }
}

inline auto disable_sigpipe(int fd) -> bool
{
    int yes = 1;
    for (;;) {
        if (::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes)) == 0) {
            return true;
        }
        if (errno != EINTR) {
            return false;
        }
    }
}

inline auto configure_socket(int fd) -> bool
{
    auto const status_flags = get_descriptor_flags(fd, F_GETFL);
    if (status_flags < 0 || !set_descriptor_flags(fd, F_SETFL, status_flags | O_NONBLOCK)) {
        return false;
    }

    auto const descriptor_flags = get_descriptor_flags(fd, F_GETFD);
    return descriptor_flags >= 0 && set_descriptor_flags(fd, F_SETFD, descriptor_flags | FD_CLOEXEC) &&
           disable_sigpipe(fd);
}

} // namespace jb::net::priv
