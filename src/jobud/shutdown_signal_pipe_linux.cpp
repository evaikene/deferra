#include "shutdown_signal_posix_priv.hpp"

#include <fcntl.h>

namespace jb::jobud::detail {

auto prepare_shutdown_pipe(int* descriptors, ShutdownSignalOperations const& operations) -> int
{
    return operations.pipe(descriptors, O_NONBLOCK | O_CLOEXEC);
}

} // namespace jb::jobud::detail
