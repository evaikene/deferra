/// @file history.hpp
/// @brief Owning public views and query inputs for retained run and attempt history.
///
#pragma once

#include "attempt.hpp"
#include "run.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace jb::jobu {

/// Half-open UTC interval. Missing bounds leave that side unrestricted.
struct UtcRange {
    /// Inclusive lower bound.
    std::optional<jb::core::UtcTimePoint> from;
    /// Exclusive upper bound; must exceed from when both are present.
    std::optional<jb::core::UtcTimePoint> to;
};

/// Filters applied together to immutable run identity and current retained state.
/// Queue and job IDs refer to the run snapshot, including after an owner is moved or deleted.
struct RunFilters {
    /// Queue captured on the run, independent of the job's current queue.
    std::optional<jb::core::Uuid> queue_id;
    /// Definition identity retained after soft deletion.
    std::optional<jb::core::Uuid> job_id;
    /// Current retained lifecycle state.
    std::optional<RunState>       state;
    /// Scheduled or manual; Submitted is reserved and rejected.
    std::optional<RunOrigin>      origin;
    /// Runner family captured on the run.
    std::optional<JobType>        type;
    /// Immutable nominal occurrence-time range.
    UtcRange                      planned;
    /// Actual first-start range; null timestamps never match a nonempty range.
    UtcRange                      started;
    /// Terminal completion range; null timestamps never match a nonempty range.
    UtcRange                      completed;
};

/// Initial run query. Limit is the maximum item count, from 1 through 200.
struct RunQuery {
    /// All supplied filters are combined with AND.
    RunFilters  filters;
    /// Requested item upper bound, default 100 and range 1–200.
    std::size_t limit{100};
};

/// Opaque continuation token; a continuation request supplies no other fields.
struct CursorRequest {
    /// Unknown, malformed, expired, evicted, or wrong-method tokens return jobu.history.invalid_cursor.
    std::string cursor;
};

/// Initial filters or a cursor-only continuation of the same live query.
using RunListRequest = std::variant<RunQuery, CursorRequest>;

/// Lightweight run projection; attributes, payload, result, and output are excluded.
struct RunSummary {
    /// Stable run identity.
    jb::core::Uuid                        id;
    /// Definition that produced this occurrence.
    jb::core::Uuid                        job_id;
    /// Queue captured when the run was created.
    jb::core::Uuid                        queue_id;
    /// Definition revision captured at creation.
    JobRevision                           job_revision{1};
    /// Operation that created the run.
    RunOrigin                             origin{RunOrigin::Scheduled};
    /// Captured runner family.
    JobType                               type{JobType::Cli};
    /// Current durable lifecycle state.
    RunState                              state{RunState::Scheduled};
    /// Whether this is the definition schedule's current occurrence.
    bool                                  schedule_owned{true};
    /// Captured scheduling priority.
    std::int32_t                          priority{0};
    /// Immutable nominal occurrence time.
    jb::core::UtcTimePoint                planned_at;
    /// Earliest time the run may become eligible.
    jb::core::UtcTimePoint                runnable_at;
    /// First actual start, absent before any attempt starts.
    std::optional<jb::core::UtcTimePoint> started_at;
    /// Terminal time, absent while the run is nonterminal.
    std::optional<jb::core::UtcTimePoint> completed_at;
};

/// Full run view. Payload is the original durable template, never resolved execution bytes.
/// Attempts and output are queried separately.
struct RunDetails : RunSummary {
    /// Complete immutable materialized attributes.
    AttributeSet                       attributes;
    /// Original immutable CLI or HTTP template JSON.
    jb::core::JsonValue                payload;
    /// Safe terminal summary, absent while unavailable.
    std::optional<jb::core::JsonValue> result;
};

/// Run summaries in descending (planned_at, id) order with an optional live-view continuation.
struct RunPage {
    /// At most the requested item limit; the response byte budget may stop earlier.
    std::vector<RunSummary>    items;
    /// Server-owned continuation when more matching rows may remain.
    std::optional<std::string> next_cursor;
};

/// Lightweight attempt projection; result and output are excluded.
struct AttemptSummary {
    /// Parent run identity.
    jb::core::Uuid                        run_id;
    /// Positive sequence number within the run.
    AttemptNumber                         attempt_number{1};
    /// Earliest eligible start time.
    jb::core::UtcTimePoint                due_at;
    /// Actual start, absent before execution.
    std::optional<jb::core::UtcTimePoint> started_at;
    /// Terminal completion, absent while incomplete.
    std::optional<jb::core::UtcTimePoint> completed_at;
    /// Current durable attempt state.
    AttemptState                          state{AttemptState::Pending};
    /// Terminal classification, absent while incomplete.
    std::optional<AttemptOutcome>         outcome;
};

/// Full attempt view; output remains available only through the separate output API.
struct AttemptDetails : AttemptSummary {
    /// Safe terminal result object, absent while unavailable.
    std::optional<jb::core::JsonValue> result;
};

/// Initial request for attempts belonging to one run, newest attempt number first.
/// Limit is the maximum item count, from 1 through 200.
struct AttemptQuery {
    /// Run whose retained attempts are requested.
    jb::core::Uuid run_id;
    /// Requested item upper bound, default 100 and range 1–200.
    std::size_t    limit{100};
};

/// Initial owner/limit request or a cursor-only continuation.
using AttemptListRequest = std::variant<AttemptQuery, CursorRequest>;

/// Attempt summaries in descending attempt-number order with an optional live-view continuation.
struct AttemptPage {
    /// At most the requested item limit; the response byte budget may stop earlier.
    std::vector<AttemptSummary> items;
    /// Server-owned continuation when more attempts may remain.
    std::optional<std::string>  next_cursor;
};

} // namespace jb::jobu
