#pragma once

#include "enum_bitmask.hpp"

#include <chrono>
#include <cstdint>
#include <functional>

namespace jb::core {

/// Event flags
enum class EventFlag : std::uint8_t {
    None     = 0x00, ///< No events
    Tasks    = 0x01, ///< Task events (posted tasks)
    Timers   = 0x02, ///< Timer events (expired timers)
    Watchers = 0x04, ///< Watcher events (ready file descriptors)
    All      = 0x07, ///< All events
};
using EventFlags = enum_bitmask<EventFlag>;

/// fd events to watch for
enum class FdEvent : std::uint32_t { // NOLINT(performance-enum-size)
    Read  = 0x01,
    Write = 0x02,
};
using FdEvents = enum_bitmask<FdEvent>;

using Clock      = std::chrono::steady_clock;
using TimePoint  = Clock::time_point;
using Duration   = Clock::duration;
using Task       = std::function<void()>;
using FdCallback = std::function<void(int fd, FdEvents events)>;

/// fd watch handle
struct FdWatch {
    int fd{-1}; // file descriptor being watched

    explicit operator bool() const noexcept { return fd >= 0; }
};

/// Timer handle
struct TimerHandle {
    using id_t = std::uint64_t;
    id_t id{0}; // unique timer ID (zero means invalid timer)

    explicit operator bool() const noexcept { return id > 0; }
};

} // namespace jb::core
