/** @file fake_time_source.hpp
 * @brief Defines deterministic wall and monotonic time support for tests.
 */
#pragma once

#include "time_source.hpp"

#include <chrono>

namespace jb::test {

/// Single-threaded TimeSource implementation controlled directly by a test.
/// Set clocks independently for wall-clock jumps or call advance() to avoid real sleeps.
class FakeTimeSource final : public core::TimeSource {
public:
    /// Returns the configured UTC time.
    [[nodiscard]] auto utc_now() const noexcept -> core::UtcTimePoint override { return _utc; }

    /// Returns the configured monotonic time.
    [[nodiscard]] auto monotonic_now() const noexcept -> core::TimePoint override { return _monotonic; }

    /// Replaces UTC time without changing monotonic time.
    void set_utc(core::UtcTimePoint value) { _utc = value; }

    /// Replaces monotonic time without changing UTC time.
    void set_monotonic(core::TimePoint value) { _monotonic = value; }

    /// Advances both clocks by the same duration.
    void advance(core::Duration duration)
    {
        _utc       += std::chrono::duration_cast<core::UtcClock::duration>(duration);
        _monotonic += duration;
    }

private:
    core::UtcTimePoint _utc;
    core::TimePoint    _monotonic;
};

} // namespace jb::test
