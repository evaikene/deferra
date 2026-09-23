/// @file control_json.hpp
/// @brief Typed JSON conversion for run controls and cron previews.
///
#pragma once

#include "job.hpp"
#include "management.hpp"
#include "scheduler.hpp"

#include <cstddef>
#include <vector>

namespace jb::jobu {

/// Requests a bounded cron preview strictly after a UTC instant.
struct ScheduleNextRequest {
    CronSchedule           schedule;
    jb::core::UtcTimePoint after;
    /// Number of occurrences, from 1 through 200; omitted wire count defaults to five.
    std::size_t            count{5};
};

/// Encodes or strictly decodes job.run_now parameters. Key policy remains with ManagementService.
[[nodiscard]] auto run_now_request_to_json(RunNowRequest const& request)
    -> jb::core::Result<jb::core::JsonValue, jb::core::Error>;
[[nodiscard]] auto run_now_request_from_json(jb::core::JsonValue const& value)
    -> jb::core::Result<RunNowRequest, jb::core::Error>;

/// Encodes or strictly decodes run.cancel parameters containing one canonical run_id.
[[nodiscard]] auto cancel_run_request_to_json(jb::core::Uuid const& run_id)
    -> jb::core::Result<jb::core::JsonValue, jb::core::Error>;
[[nodiscard]] auto cancel_run_request_from_json(jb::core::JsonValue const& value)
    -> jb::core::Result<jb::core::Uuid, jb::core::Error>;

/// Encodes the owning cancellation reply using the shared full run view.
[[nodiscard]] auto cancel_run_result_to_json(CancelRunResult const& result, AttributeRegistry const& registry)
    -> jb::core::Result<jb::core::JsonValue, jb::core::Error>;
/// Decodes required cancellation fields while ignoring unknown response members.
[[nodiscard]] auto cancel_run_result_from_json(jb::core::JsonValue const& value, AttributeRegistry const& registry)
    -> jb::core::Result<CancelRunResult, jb::core::Error>;

/// Encodes or strictly decodes a complete cron-only schedule.validate request.
[[nodiscard]] auto schedule_validate_request_to_json(CronSchedule const& schedule)
    -> jb::core::Result<jb::core::JsonValue, jb::core::Error>;
[[nodiscard]] auto schedule_validate_request_from_json(jb::core::JsonValue const& value)
    -> jb::core::Result<CronSchedule, jb::core::Error>;

/// The only successful schedule.validate result is {"valid":true}.
[[nodiscard]] auto schedule_validate_result_to_json() -> jb::core::JsonValue;
[[nodiscard]] auto schedule_validate_result_from_json(jb::core::JsonValue const& value)
    -> jb::core::Result<void, jb::core::Error>;

/// Encodes or strictly decodes schedule.next parameters; the engine enforces the count range.
[[nodiscard]] auto schedule_next_request_to_json(ScheduleNextRequest const& request)
    -> jb::core::Result<jb::core::JsonValue, jb::core::Error>;
[[nodiscard]] auto schedule_next_request_from_json(jb::core::JsonValue const& value)
    -> jb::core::Result<ScheduleNextRequest, jb::core::Error>;

/// Encodes or decodes the ordered UTC occurrence array in a schedule.next result.
[[nodiscard]] auto schedule_next_result_to_json(std::vector<jb::core::UtcTimePoint> const& occurrences)
    -> jb::core::Result<jb::core::JsonValue, jb::core::Error>;
[[nodiscard]] auto schedule_next_result_from_json(jb::core::JsonValue const& value)
    -> jb::core::Result<std::vector<jb::core::UtcTimePoint>, jb::core::Error>;

} // namespace jb::jobu
