/// @file execution_events.hpp
/// @brief Defines owning, payload-free scheduler execution diagnostics.
#pragma once

#include "attempt.hpp"
#include "job.hpp"
#include "retry_mode.hpp"
#include "run.hpp"
#include "time_source.hpp"
#include "uuid.hpp"

#include <chrono>
#include <cstdint>
#include <optional>

namespace jb::jobu {

/// Identifies one run and the queue, job and runner family in its execution snapshot.
/// Owns every field; retaining a copy does not borrow scheduler, database or executor state.
/// Contains no names, payload, output, result object or free-form diagnostic text.
struct ExecutionContext {
    jb::core::Uuid queue_id; ///< Queue identity from the run snapshot.
    jb::core::Uuid job_id;   ///< Definition identity from the run snapshot.
    jb::core::Uuid run_id;   ///< Stable run identity shared by its attempts and retries.
    JobType        type;     ///< Runner family selected for this run.
    RunOrigin      origin;   ///< How the run was created.
};

/// Diagnostic for an attempt accepted by its executor after the durable start committed.
/// Acceptance does not promise that a child has exec'd or an HTTP connection has opened.
struct AttemptStarted {
    ExecutionContext       execution;      ///< Owning run identity and classification.
    AttemptNumber          attempt_number; ///< Positive durable sequence number within the run.
    jb::core::UtcTimePoint started_at;     ///< Existing durable attempt-start timestamp in UTC.
};

/// Furthest execution-start boundary reached before an attempt completed.
enum class AttemptStartPhase : std::uint8_t {
    Accepted,          ///< The executor accepted the work.
    PreparationFailed, ///< Payload/secret preparation failed before executor invocation.
    ExecutorRejected,  ///< The executor refused the start operation.
};

/// Diagnostic for a committed attempt completion, using its effective cancellation-normalized outcome.
/// Optional diagnostics never redefine success, retry policy or the durable outcome.
struct AttemptFinished {
    ExecutionContext       execution;      ///< Owning run identity and classification.
    AttemptNumber          attempt_number; ///< Positive durable sequence number within the run.
    AttemptOutcome         outcome;        ///< Effective durable outcome, including accepted cancellation overrides.
    AttemptStartPhase      start_phase;    ///< Whether execution was accepted, unprepared or rejected.
    jb::core::UtcTimePoint completed_at;   ///< Durable completion timestamp in UTC.

    /// Local monotonic interval from successful executor-start return to entry into completion handling.
    /// Excludes the completion transaction; this is neither runnable wait nor total run/transport time.
    /// Absent for preparation/rejection, negative or unrepresentable intervals. UTC changes do not affect it.
    std::optional<std::chrono::milliseconds> elapsed;
    std::optional<int>                       exit_code;     ///< CLI exit code in 0..255, when available.
    std::optional<int>                       signal_number; ///< Positive platform signal number, when available.
    std::optional<unsigned>                  http_status;   ///< HTTP status in 100..599, when available.
};

/// Diagnostic for the retry chosen and persisted by a committed attempt completion.
/// Reports the actual due time and mode, including policy jitter and Retry-After decisions; does not recompute them.
struct RetryScheduled {
    ExecutionContext       execution;         ///< Owning run identity and classification.
    AttemptNumber          completed_attempt; ///< Positive sequence number of the completed attempt.
    AttemptNumber          next_attempt;      ///< Checked successor, at most maximum_attempt_number.
    jb::core::UtcTimePoint runnable_at;       ///< Persisted retry eligibility time in UTC.
    RetryMode              mode;              ///< Chosen queue-capacity policy during the wait.
};

/// Diagnostic for a committed terminal run outcome, with or without a completing attempt.
struct RunFinished {
    ExecutionContext       execution;    ///< Owning run identity and classification.
    RunState               state;        ///< Terminal run state; live execution does not produce Interrupted.
    jb::core::UtcTimePoint completed_at; ///< Durable terminal timestamp in UTC.

    /// Completing attempt, or absent for cancellation of pending Scheduled/RetryWait work.
    /// Absence does not imply that the run never had earlier attempts.
    std::optional<AttemptNumber> completing_attempt;
};

} // namespace jb::jobu
