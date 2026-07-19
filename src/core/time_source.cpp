#include "time_source.hpp"

namespace jb::core {

auto SystemTimeSource::utc_now() const noexcept -> UtcTimePoint
{
    return UtcClock::now();
}

auto SystemTimeSource::monotonic_now() const noexcept -> TimePoint
{
    return Clock::now();
}

} // namespace jb::core
