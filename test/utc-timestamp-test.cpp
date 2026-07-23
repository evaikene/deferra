#include "utc_timestamp.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <string_view>

using namespace jb::core;
using namespace jb::jobu;
using namespace std::chrono_literals;

TEST_CASE("UTC timestamps format canonical epoch and pre-epoch values", "[jobu][time]")
{
    REQUIRE(format_utc_timestamp(UtcTimePoint{}));
    CHECK(*format_utc_timestamp(UtcTimePoint{}) == "1970-01-01T00:00:00.000000Z");
    CHECK(*format_utc_timestamp(UtcTimePoint{-1us}) == "1969-12-31T23:59:59.999999Z");
    CHECK(*format_utc_timestamp(UtcTimePoint{951827696123456us}) == "2000-02-29T12:34:56.123456Z");
}

TEST_CASE("UTC timestamps parse accepted fractional precision", "[jobu][time]")
{
    auto const expected = UtcTimePoint{1704067200000000us};
    for (auto const text : {
             std::string_view{"2024-01-01T00:00:00Z"},
             std::string_view{"2024-01-01T00:00:00.0Z"},
             std::string_view{"2024-01-01T00:00:00.00Z"},
             std::string_view{"2024-01-01T00:00:00.000Z"},
             std::string_view{"2024-01-01T00:00:00.000000Z"},
         }) {
        auto const parsed = parse_utc_timestamp(text);
        REQUIRE(parsed);
        CHECK(*parsed == expected);
        CHECK(*format_utc_timestamp(*parsed) == "2024-01-01T00:00:00.000000Z");
    }

    auto const tenths = parse_utc_timestamp("2024-01-01T00:00:00.1Z");
    REQUIRE(tenths);
    CHECK(*format_utc_timestamp(*tenths) == "2024-01-01T00:00:00.100000Z");
}

TEST_CASE("UTC timestamps reject invalid syntax and civil values", "[jobu][time]")
{
    constexpr auto rejected = std::array{
        std::string_view{""},
        std::string_view{"2024-01-01 00:00:00Z"},
        std::string_view{"2024-01-01T00:00:00+00:00"},
        std::string_view{"2024-01-01T00:00:00z"},
        std::string_view{"2024-01-01T00:00:60Z"},
        std::string_view{"2023-02-29T00:00:00Z"},
        std::string_view{"2024-04-31T00:00:00Z"},
        std::string_view{"2024-13-01T00:00:00Z"},
        std::string_view{"2024-01-01T24:00:00Z"},
        std::string_view{"2024-01-01T00:00:00.Z"},
        std::string_view{"2024-01-01T00:00:00.1234567Z"},
        std::string_view{"2024-01-01T00:00:00Ztrailing"},
    };

    for (auto const text : rejected) {
        auto const parsed = parse_utc_timestamp(text);
        REQUIRE_FALSE(parsed);
        CHECK(parsed.error().code == "jobu.time.invalid_format");
    }
}

TEST_CASE("UTC timestamps honor the platform clock range", "[jobu][time]")
{
    using Microseconds = std::chrono::microseconds;

    struct Boundary {
        std::string_view text;
        std::int64_t     microseconds;
    };

    constexpr auto boundaries = std::array{
        Boundary{"0000-01-01T00:00:00Z",        -62167219200000000},
        Boundary{"9999-12-31T23:59:59.999999Z", 253402300799999999},
    };
    auto const minimum = std::chrono::ceil<Microseconds>(UtcTimePoint::min().time_since_epoch()).count();
    auto const maximum = std::chrono::floor<Microseconds>(UtcTimePoint::max().time_since_epoch()).count();

    for (auto const& boundary : boundaries) {
        auto const parsed = parse_utc_timestamp(boundary.text);
        if (boundary.microseconds < minimum || boundary.microseconds > maximum) {
            REQUIRE_FALSE(parsed);
            CHECK(parsed.error().code == "jobu.time.out_of_range");
        }
        else {
            REQUIRE(parsed);
            CHECK(std::chrono::duration_cast<Microseconds>(parsed->time_since_epoch()).count() ==
                  boundary.microseconds);
        }
    }
}
