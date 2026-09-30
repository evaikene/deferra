/// @file schedule_input.hpp
/// @brief Defines schedule requests before daemon defaults are resolved.
///
#pragma once

#include "job.hpp"

#include <optional>
#include <string>
#include <variant>

namespace jb::jobu {

/// Recurring schedule input, distinct from a stored schedule with an explicit timezone.
struct CronScheduleInput {
    /// Five-field cron expression or supported alias.
    std::string                expression;
    /// Explicit nonempty timezone, or no value to use the receiving daemon's default.
    /// Explicit UTC remains UTC regardless of that default.
    std::optional<std::string> timezone;
};

/// Replacement schedule input. An omitted replacement keeps the stored schedule unchanged.
using JobScheduleInput = std::variant<OnceSchedule, CronScheduleInput>;

} // namespace jb::jobu
