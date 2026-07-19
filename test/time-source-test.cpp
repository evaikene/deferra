#include "time_source.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace jb::core;

TEST_CASE("SystemTimeSource provides wall and monotonic timestamps", "[core][time]")
{
    SystemTimeSource time_source;

    CHECK(time_source.utc_now() > UtcTimePoint{});
    CHECK(time_source.monotonic_now() > TimePoint{});
}
