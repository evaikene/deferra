#pragma once

#include "event_loop_types.hpp"

#include <cstdint>
#include <memory>

namespace jb::core::priv {

/// Opaque per-fd registration handle
struct FdRegistration {
    int           fd;
    std::uint32_t events;
};

/// A single ready event as reported by poll()
struct ReadyEvent {
    int      fd;
    FdEvents events;
};

/// Abstract backend
class Backend {
public:

    virtual ~Backend() = default;

    /// Register an fd for the given events
    /// @param[in] fd the file descriptor to watch
    /// @param[in] events the events to watch for
    virtual void add_fd(int fd, FdEvents events) = 0;

    /// Remove an fd from the poller
    /// @param[in] fd the file descriptor to remove
    virtual void remove_fd(int fd) = 0;

    /// Block until at least one fd is ready or the timeout expires
    /// @param[in,out] out the output buffer to fill with ready events
    /// @param[in] max_events the maximum number of events to return
    /// @param[in] timeout_ms the maximum time to wait in milliseconds (negative for infinite)
    /// @return the number of events returned in the output buffer, or -1 on error
    virtual auto poll(ReadyEvent* out, int max_events, int timeout_ms) -> int = 0;

    /// Wake a blocked poll() from another thread
    ///
    /// Must be idempotent and safe to call from any thread at any time.
    virtual void wakeup() = 0;
};

/// Backend factory
/// @return a unique pointer to a new backend instance
auto make_backend() -> std::unique_ptr<Backend>;

} // namespace jb::core::priv
