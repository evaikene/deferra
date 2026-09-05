#pragma once

#include "event_loop_backend.hpp"

#include <memory>

#include <sys/epoll.h>

namespace jb::core::priv {

/// Linux-only native process-watch seam. Defaults invoke the real system calls.
/// Overrides must preserve syscall errno and failure-without-mutation semantics.
class EpollProcessOperations {
public:
    virtual ~EpollProcessOperations() = default;
    virtual auto open_pidfd(int process_id) -> int;
    virtual auto control(int poller, int operation, int fd, epoll_event* event) -> int;
};

/// Construct the real epoll backend with owned process-operation injection.
/// Returns nullptr for null operations or failed native poller initialization.
auto make_epoll_backend(std::shared_ptr<EpollProcessOperations> operations) -> std::unique_ptr<Backend>;

} // namespace jb::core::priv
