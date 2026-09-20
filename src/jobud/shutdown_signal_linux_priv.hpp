#pragma once

#include <csignal> // IWYU pragma: keep POSIX signal sets and sigaction declarations.

#include <pthread.h>
#include <unistd.h>

namespace jb::jobud::detail {

/// Private syscall boundary for deterministic setup failures; never called by the signal handler.
/// Copied into the relay so no injected operations object is borrowed through teardown.
struct ShutdownSignalOperations {
    decltype(&::pipe2)           pipe{&::pipe2};
    decltype(&::sigaction)       action{&::sigaction};
    decltype(&::pthread_sigmask) mask{&::pthread_sigmask};
};

} // namespace jb::jobud::detail
