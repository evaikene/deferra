/** @file attempt.hpp
 * @brief Defines durable JobU attempt identity, timing, state, and outcome values.
 */
#pragma once

#include "json.hpp"
#include "time_source.hpp"
#include "uuid.hpp"

#include <cstdint>
#include <optional>

namespace jb::jobu {

/// Positive sequence number identifying an attempt within one run.
using AttemptNumber = std::uint64_t;

/// Durable execution state of an attempt.
enum class AttemptState : std::uint8_t {
    Pending,   ///< Persisted but not yet executing.
    Running,   ///< External execution is in progress.
    Completed, ///< Execution has a terminal outcome.
};

/// Stable terminal classification of a completed attempt.
enum class AttemptOutcome : std::uint8_t {
    Succeeded,   ///< The external operation met its success criteria.
    Failed,      ///< The external operation produced an observed failure.
    Interrupted, ///< The final external outcome could not be established safely.
    Cancelled,   ///< Execution was cancelled explicitly before or after it started.
};

/** One durable execution attempt within a run.
 *
 * The composite identity is run_id plus a positive attempt_number. Phase 3 provides this value for repository fixture
 * round trips; production management operations do not create attempts.
 */
struct JobAttempt {
    /// Parent run identity.
    jb::core::Uuid                        run_id;
    /// Positive sequence number within the parent run.
    AttemptNumber                         attempt_number{1};
    /// Earliest time the attempt may start, in UTC.
    jb::core::UtcTimePoint                due_at;
    /// Actual start time, or no value if execution did not start.
    std::optional<jb::core::UtcTimePoint> started_at;
    /// Terminal completion time, or no value while incomplete.
    std::optional<jb::core::UtcTimePoint> completed_at;
    /// Current durable execution state.
    AttemptState                          state{AttemptState::Pending};
    /// Terminal classification, or no value while incomplete.
    std::optional<AttemptOutcome>         outcome;
    /// Detailed terminal result object, or no value when unavailable.
    std::optional<jb::rpc::JsonValue>     result;
};

} // namespace jb::jobu
