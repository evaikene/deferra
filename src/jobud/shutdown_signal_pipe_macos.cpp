#include "shutdown_signal_posix_priv.hpp"

#include <fcntl.h>

namespace jb::jobud::detail {

auto prepare_shutdown_pipe(int* descriptors, ShutdownSignalOperations const& operations) -> int
{
    if (operations.pipe(descriptors) != 0) {
        return -1;
    }

    // Darwin has no pipe2. Installation precedes worker creation and handler publication, so
    // neither end can escape before all flags are set. The relay unwinds both ends on failure.
    for (int index = 0; index < 2; ++index) {
        auto const descriptor = descriptors[index];
        auto const status     = operations.control(descriptor, F_GETFL, 0);
        if (status < 0 || operations.control(descriptor, F_SETFL, status | O_NONBLOCK) < 0) {
            return -1;
        }
        auto const flags = operations.control(descriptor, F_GETFD, 0);
        if (flags < 0 || operations.control(descriptor, F_SETFD, flags | FD_CLOEXEC) < 0) {
            return -1;
        }
    }
    return 0;
}

} // namespace jb::jobud::detail
