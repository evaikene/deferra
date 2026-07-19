/** @file time_source.hpp
 * @brief Defines injectable wall-clock and monotonic time sources.
 */
#pragma once

#include "event_loop_types.hpp"

#include <chrono>

namespace jb::core {

/// Wall clock used for timestamps persisted in UTC.
using UtcClock     = std::chrono::system_clock;
/// Point in UTC wall-clock time.
using UtcTimePoint = UtcClock::time_point;

/// Provides wall and monotonic time to production components.
/// Components receive this reference instead of calling clocks directly, allowing deterministic tests.
class TimeSource {
public:
    /// Destroys a time source through its interface.
    virtual ~TimeSource() = default;

    /// Returns current UTC wall-clock time for persisted data.
    [[nodiscard]] virtual auto utc_now() const noexcept -> UtcTimePoint    = 0;
    /// Returns current monotonic time for in-process deadlines.
    [[nodiscard]] virtual auto monotonic_now() const noexcept -> TimePoint = 0;
};

/// Production TimeSource backed by the system and steady clocks.
/// Use a deterministic implementation in tests instead.
class SystemTimeSource final : public TimeSource {
public:
    /// Returns current system UTC time.
    [[nodiscard]] auto utc_now() const noexcept -> UtcTimePoint override;
    /// Returns current steady-clock time.
    [[nodiscard]] auto monotonic_now() const noexcept -> TimePoint override;
};

} // namespace jb::core
