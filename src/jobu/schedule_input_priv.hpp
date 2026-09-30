#pragma once

#include "error.hpp"
#include "json.hpp"
#include "result.hpp"
#include "schedule_input.hpp"

#include <string_view>

namespace jb::jobu::detail {

/// Strict cron request conversion shared by management, previews, and canonical creation records.
[[nodiscard]] auto cron_schedule_input_from_json(jb::core::JsonValue const& value)
    -> jb::core::Result<CronScheduleInput, jb::core::Error>;
[[nodiscard]] auto cron_schedule_input_to_json(CronScheduleInput const& schedule)
    -> jb::core::Result<jb::core::JsonValue, jb::core::Error>;

/// Resolves omission without changing the caller's symbolic input. CronEngine validates the resolved schedule.
[[nodiscard]] auto resolve_cron_schedule(CronScheduleInput const& input, std::string_view default_timezone)
    -> jb::core::Result<CronSchedule, jb::core::Error>;

} // namespace jb::jobu::detail
