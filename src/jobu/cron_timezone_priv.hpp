#pragma once

#include "error.hpp"
#include "result.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace jb::jobu {

enum class PosixDateRuleKind : std::uint8_t {
    MonthWeekday,
    JulianWithoutLeapDay,
    JulianWithLeapDay,
};

struct PosixDateRule {
    PosixDateRuleKind kind{PosixDateRuleKind::MonthWeekday};
    std::uint16_t     day{};
    std::uint8_t      month{};
    std::uint8_t      week{};
    std::uint8_t      weekday{};
    std::int32_t      transition_time_seconds{7200};

    auto operator==(PosixDateRule const&) const -> bool = default;
};

struct PosixDaylightRule {
    std::string   abbreviation;
    std::int32_t  utc_offset_seconds{};
    PosixDateRule start;
    PosixDateRule end;

    auto operator==(PosixDaylightRule const&) const -> bool = default;
};

struct PosixFutureRule {
    std::string                      standard_abbreviation;
    std::int32_t                     standard_utc_offset_seconds{};
    std::optional<PosixDaylightRule> daylight;

    auto operator==(PosixFutureRule const&) const -> bool = default;
};

struct TimezoneLocalTimeType {
    std::int32_t utc_offset_seconds{};
    bool         daylight{};

    auto operator==(TimezoneLocalTimeType const&) const -> bool = default;
};

struct TimezoneTransition {
    std::int64_t unix_seconds{};
    std::uint8_t local_time_type{};

    auto operator==(TimezoneTransition const&) const -> bool = default;
};

struct TimezoneData {
    std::vector<TimezoneTransition>    transitions;
    std::vector<TimezoneLocalTimeType> local_time_types;
    std::uint8_t                       default_local_time_type{};
    std::optional<PosixFutureRule>     future_rule;

    auto operator==(TimezoneData const&) const -> bool = default;
};

[[nodiscard]] auto parse_timezone_data(std::string_view bytes) -> jb::core::Result<TimezoneData, jb::core::Error>;

[[nodiscard]] auto load_timezone_data(std::string_view timezone) -> jb::core::Result<TimezoneData, jb::core::Error>;

[[nodiscard]] auto load_timezone_data(std::string_view timezone, std::span<std::filesystem::path const> roots)
    -> jb::core::Result<TimezoneData, jb::core::Error>;

} // namespace jb::jobu
