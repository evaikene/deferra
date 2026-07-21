/** @file run.hpp
 * @brief Defines immutable execution snapshots and lifecycle state for JobU runs.
 */
#pragma once

#include "job.hpp"

#include <cstdint>
#include <optional>

namespace jb::jobu {

/// Source that created a run.
enum class RunOrigin : std::uint8_t {
    Scheduled, ///< Created from a job definition schedule.
    Manual,    ///< Created by a future Run Now operation.
    Submitted, ///< Created by a future application-submission operation.
};

/// Durable lifecycle state of a run.
enum class RunState : std::uint8_t {
    Scheduled,   ///< Waiting to become eligible for its first attempt.
    Running,     ///< An attempt is currently executing.
    RetryWait,   ///< A failed attempt is waiting for its retry time.
    Succeeded,   ///< Completed successfully.
    Failed,      ///< Completed with an observed failure.
    Interrupted, ///< Completed after an outcome could not be determined safely.
    Cancelled,   ///< Cancelled without further execution.
};

/** One durable occurrence of a job definition.
 *
 * The runner type, priority, attributes, and payload form an immutable execution snapshot. Phase 3 creates only
 * scheduled, schedule-owned runs and never starts an attempt.
 */
struct JobRun {
    /// Stable run identity.
    jb::core::Uuid                        id;
    /// Definition that produced this run.
    jb::core::Uuid                        job_id;
    /// Definition revision captured by this run.
    JobRevision                           job_revision{1};
    /// Queue in which this occurrence executes.
    jb::core::Uuid                        queue_id;
    /// Operation that created the run.
    RunOrigin                             origin{RunOrigin::Scheduled};
    /// Whether this is the definition schedule's current non-terminal run.
    bool                                  schedule_owned{true};
    /// Nominal occurrence time in UTC.
    jb::core::UtcTimePoint                planned_at;
    /// Earliest time at which the run may become eligible, in UTC.
    jb::core::UtcTimePoint                runnable_at;
    /// Time execution started, or no value before the first attempt starts.
    std::optional<jb::core::UtcTimePoint> started_at;
    /// Terminal completion time, or no value while non-terminal.
    std::optional<jb::core::UtcTimePoint> completed_at;
    /// Snapshotted runner family.
    JobType                               type{JobType::Cli};
    /// Snapshotted scheduling priority.
    std::int32_t                          priority{0};
    /// Complete snapshotted attributes.
    AttributeSet                          attributes;
    /// Snapshotted runner payload JSON object.
    jb::rpc::JsonValue                    payload;
    /// Current durable lifecycle state.
    RunState                              state{RunState::Scheduled};
    /// Terminal result summary, or no value while non-terminal.
    std::optional<jb::rpc::JsonValue>     result;
};

} // namespace jb::jobu
