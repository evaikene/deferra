#pragma once

#include "error.hpp"
#include "result.hpp"
#include "time_source.hpp"

#include <bitset>
#include <chrono>
#include <string_view>

namespace jb::jobu {

struct CronExpression {
    std::bitset<60> minutes;
    std::bitset<24> hours;
    std::bitset<31> days_of_month;
    std::bitset<12> months;
    std::bitset<7>  days_of_week;

    auto operator==(CronExpression const&) const -> bool = default;
};

using LocalTimePoint = std::chrono::local_time<jb::core::UtcClock::duration>;
using LocalMinute    = std::chrono::local_time<std::chrono::minutes>;

[[nodiscard]] auto parse_cron_expression(std::string_view expression)
    -> jb::core::Result<CronExpression, jb::core::Error>;

[[nodiscard]] auto next_local_occurrence(CronExpression const& expression, LocalTimePoint exclusive_lower_bound)
    -> jb::core::Result<LocalMinute, jb::core::Error>;

} // namespace jb::jobu
