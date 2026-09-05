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

/// Identifier namespace of a native readiness notification.
enum class ReadyEventKind : std::uint8_t {
    FileDescriptor,
    Process,
};

/// Outcome of registering native child-exit monitoring.
enum class ProcessRegistrationResult : std::uint8_t {
    Added,
    Unsupported,
    Failed,
};

/// A single ready event as reported by poll(); process events carry no fd mask.
struct ReadyEvent {
    ReadyEventKind kind{ReadyEventKind::FileDescriptor};
    std::int64_t   ident{-1};
    FdEvents       events;
};

/// Abstract backend.
///
/// File-descriptor registration changes are transactional while the backend is
/// usable. On failure, implementations restore the prior registration. If that
/// restoration also fails, the backend must make subsequent polling fail rather
/// than continue with registration state that its EventLoop cannot represent.
class Backend {
public:

    virtual ~Backend() = default;

    /// Apply or replace the complete native registration for an fd.
    /// @param[in] fd the file descriptor to watch
    /// @param[in] events the events to watch for
    /// @param[in] trigger_mode the readiness notification mode
    /// @return true when the registration was applied, false when the previous
    ///         registration was retained or the backend became unusable
    ///
    /// A successful call means native state matches `{events, trigger_mode}`.
    /// Registration changes are transactional: on failure the previous native
    /// registration is retained or restored, unless the backend becomes
    /// unusable because restoration also failed.
    virtual auto add_fd(int fd, FdEvents events, FdTriggerMode trigger_mode) -> bool = 0;

    /// Remove an fd from the poller
    /// @param[in] fd the file descriptor to remove
    /// @return true when the registration is absent, false when the previous
    ///         registration was retained or the backend became unusable
    virtual auto remove_fd(int fd) -> bool = 0;

    /// Watch a positive native child PID for one exit notification, without reaping it.
    /// Duplicate registration succeeds without rearming. Failure leaves registrations unchanged.
    /// Unsupported denotes unavailable native monitoring, not an ordinary operational failure.
    virtual auto add_process(std::int64_t process_id) -> ProcessRegistrationResult = 0;

    /// Remove a child watch and release its native resources, without reaping the child.
    /// Returns true if absent or removed; false retains the complete registration for retry.
    /// A delivered registration remains one-shot even when removal fails.
    virtual auto remove_process(std::int64_t process_id) -> bool = 0;

    /// Block until native readiness or the timeout expires
    /// @param[in,out] out the output buffer to fill with ready events
    /// @param[in] max_events the maximum number of events to return
    /// @param[in] timeout_ms the maximum time to wait in milliseconds (negative for infinite)
    /// @return the number of events returned in the output buffer, or -1 on error
    virtual auto poll(ReadyEvent* out, int max_events, int timeout_ms) -> int = 0;

    /// Wake a blocked poll() from another thread
    ///
    /// Must be idempotent and safe to call from any thread at any time.
    /// @return true when the poller is or will be awake, false on backend failure
    virtual auto wakeup() -> bool = 0;
};

/// Backend factory
/// @return a unique pointer to a new backend instance, or nullptr on native initialization failure
auto make_backend() -> std::unique_ptr<Backend>;

} // namespace jb::core::priv
