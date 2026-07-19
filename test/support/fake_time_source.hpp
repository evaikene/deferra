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

    [[nodiscard]] auto monotonic_now() const noexcept -> core::TimePoint override { return _monotonic; }

    void set_utc(core::UtcTimePoint value) { _utc = value; }

    void set_monotonic(core::TimePoint value) { _monotonic = value; }

    void advance(core::Duration duration)
    {
        _utc       += std::chrono::duration_cast<core::UtcClock::duration>(duration);
        _monotonic += duration;
    }

private:
    core::UtcTimePoint _utc{};
    core::TimePoint    _monotonic{};
};

} // namespace jb::test
  /// Returns the configured monotonic time.
  /// Replaces UTC time without changing monotonic time.
  /// Replaces monotonic time without changing UTC time.
  /// Advances both clocks by the same duration.
