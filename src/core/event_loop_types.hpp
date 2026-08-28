#pragma once

#include "enum_bitmask.hpp"

#include <chrono>
#include <cstdint>
#include <functional>

namespace jb::core {

/// Result of one EventLoop::process_events() invocation.
enum class ProcessEventsResult : std::uint8_t {
    Running, ///< Processing succeeded and the loop remains running
    Stopped, ///< Processing succeeded and the loop is not running
    Failed,  ///< Processing could not complete because the event-loop backend failed
};

/// Event flags
enum class EventFlag : std::uint8_t {
    None     = 0x00, ///< No events
    Tasks    = 0x01, ///< Task events (posted tasks)
    Events   = 0x02, ///< Object events (posted events and queued signals)
    Timers   = 0x04, ///< Timer events (expired timers)
    Watchers = 0x08, ///< Watcher events (ready file descriptors)
    All      = 0x0f, ///< All events
};
using EventFlags = enum_bitmask<EventFlag>;

/// fd events to watch for
enum class FdEvent : std::uint32_t { // NOLINT(performance-enum-size)
    Read  = 0x01,
    Write = 0x02,
};
using FdEvents = enum_bitmask<FdEvent>;

/// Readiness notification mode for an EventLoop file-descriptor watch.
enum class FdTriggerMode : std::uint8_t {
    /// Report readiness transitions. The descriptor must be nonblocking, and
    /// the consumer must drain the indicated operation until it would block
    /// before relying on a later readiness callback. Reads and accepts continue
    /// through `EAGAIN` or `EWOULDBLOCK`; writes continue until the output is
    /// drained or the operation would block. Otherwise, the descriptor may
    /// remain ready without another callback.
    Edge,

    /// Report the current readiness state. The callback may run again on a
    /// later poll while the requested condition remains true, so the consumer
    /// need not own or fully drain the native operation but must make progress,
    /// change or remove the watch, or tolerate repetition. One callback must not
    /// be treated as one readiness transition.
    Level,
};

using Clock      = std::chrono::steady_clock;
using TimePoint  = Clock::time_point;
using Duration   = Clock::duration;
using Task       = std::function<void()>;
using FdCallback = std::function<void(int fd, FdEvents events)>;

/// fd watch handle
struct FdWatch {
    static constexpr int kInvalid{-1};

    int fd{kInvalid}; // file descriptor being watched

    explicit operator bool() const noexcept { return fd > kInvalid; }
};

/// Timer handle
struct TimerHandle {
    using id_t = std::uint64_t;
    static constexpr id_t kInvalid{0};

    id_t id{kInvalid}; // unique timer ID (zero means invalid timer)

    explicit operator bool() const noexcept { return id != kInvalid; }
};

} // namespace jb::core
