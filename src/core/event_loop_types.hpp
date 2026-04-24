#pragma once

#include "enum_bitmask.hpp"

#include <chrono>
#include <cstdint>
#include <functional>

namespace jb::core {

using Clock     = std::chrono::steady_clock;
using TimePoint = Clock::time_point;
using Duration  = Clock::duration;
using Task      = std::function<void()>;

/// fd events to watch for
enum class FdEvent : std::uint32_t { // NOLINT(performance-enum-size)
    Read  = 0x01,
    Write = 0x02,
};
using FdEvents = enum_bitmask<FdEvent>;

/// fd watch handle
struct FdWatch {
    int           fd;
    std::uint32_t id;
};

/// Timer handle
struct TimerHandle {
    using id_t = std::uint64_t;
    id_t id;
};

} // namespace jb::core
