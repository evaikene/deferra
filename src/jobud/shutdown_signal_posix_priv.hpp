#pragma once

#include <csignal> // IWYU pragma: keep POSIX signal sets and sigaction declarations.

#if defined(__APPLE__)
#  include <fcntl.h>
#endif
#include <pthread.h>
#include <unistd.h>

namespace jb::jobud::detail {

/// Private syscall boundary for deterministic setup failures; never called by the signal handler.
/// Copied into the relay so no injected operations object is borrowed through teardown.
struct ShutdownSignalOperations {
#if defined(__APPLE__)
    decltype(&::pipe) pipe{&::pipe};
    int (*control)(int, int, int){[](int fd, int command, int value) { return ::fcntl(fd, command, value); }};
#else
    decltype(&::pipe2) pipe{&::pipe2};
#endif
    decltype(&::sigaction)       action{&::sigaction};
    decltype(&::pthread_sigmask) mask{&::pthread_sigmask};
};

/// Creates nonblocking CLOEXEC ends before workers start. The caller owns any opened ends even on failure.
auto prepare_shutdown_pipe(int* descriptors, ShutdownSignalOperations const& operations) -> int;

} // namespace jb::jobud::detail
