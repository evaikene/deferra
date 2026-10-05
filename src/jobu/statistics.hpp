/// @file statistics.hpp
/// @brief Bounded retained-history statistics contracts.
///
#pragma once

#include "history.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace jb::jobu {

/// One grouping dimension; None always produces one aggregate, including for an empty cohort.
enum class StatisticsGroupBy : std::uint8_t {
    None,
    Queue,
    Job,
    Type,
    Origin,
    State
};

/// The method family that owns a statistics cursor.
enum class StatisticsScope : std::uint8_t {
    System,
    Queue
};

/// Initial retained-run query. The planned window is half-open and may omit either bound.
/// The service resolves omitted bounds once: upper to server UTC now, lower to 24 hours before upper.
/// The resolved window must be increasing and no longer than 31 days. Queue scope requires queue_id;
/// a future queue.stats adapter resolves its selector to that stable ID before calling the service.
struct StatisticsRequest {
    std::optional<jb::core::Uuid> queue_id;
    std::optional<jb::core::Uuid> job_id;
    std::optional<JobType>        type;
    std::optional<RunOrigin>      origin;
    UtcRange                      planned;
    StatisticsGroupBy             group_by{StatisticsGroupBy::None};
    std::size_t                   limit{100};
};

/// A continuation contains only the opaque cursor, as with retained-history lists.
using StatisticsListRequest = std::variant<StatisticsRequest, CursorRequest>;

/// Count of every implemented retained run state, plus the fixed type and origin breakdowns.
struct StatisticsRunCounts {
    std::uint64_t total{0};
    std::uint64_t scheduled{0};
    std::uint64_t running{0};
    std::uint64_t retry_wait{0};
    std::uint64_t succeeded{0};
    std::uint64_t failed{0};
    std::uint64_t interrupted{0};
    std::uint64_t cancelled{0};
    std::uint64_t cli{0};
    std::uint64_t http{0};
    std::uint64_t scheduled_origin{0};
    std::uint64_t manual_origin{0};
};

/// Counts all retained attempts of the selected runs, including retries outside the planned window.
struct StatisticsAttemptCounts {
    std::uint64_t total{0};
    std::uint64_t pending{0};
    std::uint64_t running{0};
    std::uint64_t completed{0};
    std::uint64_t succeeded{0};
    std::uint64_t failed{0};
    std::uint64_t interrupted{0};
    std::uint64_t cancelled{0};
    std::uint64_t retries{0};
};

/// Explicit events recorded by retained output rows. An absent row adds neither event.
struct StatisticsCaptureCounts {
    std::uint64_t truncated_attempts{0};
    std::uint64_t lost_attempts{0};
};

/// Duration in milliseconds. Empty populations have null average and maximum.
/// Fractional milliseconds are retained; page provenance distinguishes wall-clock and monotonic inputs.
struct StatisticsDuration {
    std::uint64_t         samples{0};
    std::optional<double> average;
    std::optional<double> maximum;
};

/// Mutually exclusive retained-run coverage, summing to the group's run total.
/// Nonterminal runs are unfinished regardless of quality. Terminal runs use their persisted quality;
/// Partial counters are lower bounds and Unmeasured runs have no observed wait. Neither supplies duration samples.
struct StatisticsRunnableWaitCoverage {
    std::uint64_t complete{0};
    std::uint64_t partial{0};
    std::uint64_t unmeasured{0};
    std::uint64_t unfinished{0};
};

/// The key is null for None, a UUID for Queue/Job, or the corresponding enum otherwise.
using StatisticsGroupKey = std::variant<std::monostate, jb::core::Uuid, JobType, RunOrigin, RunState>;

/// One group's separately counted runs and attempts.
struct StatisticsGroup {
    StatisticsGroupKey                key;
    StatisticsRunCounts               runs;
    StatisticsAttemptCounts           attempts;
    StatisticsCaptureCounts           capture;
    StatisticsDuration                schedule_lateness_ms;
    StatisticsDuration                execution_wall_duration_ms;
    /// Complete terminal runs only, once per run across all attempts. Null means measurement is unavailable;
    /// an available measurement with no finalized samples has zero samples and null average/maximum.
    std::optional<StatisticsDuration> runnable_wait_ms;
    StatisticsRunnableWaitCoverage    runnable_wait_coverage;
};

/// Provenance attached to every page, including an empty page.
/// Capture events come only from persisted output flags; absence is not evidence of successful capture.
struct StatisticsMeasurement {
    /// First run start and attempt start/end are wall-clock timestamps, not monotonic measurements.
    std::string timing{"wall_clock_derived"};
    /// "monotonic_observed" for persisted eligible-wait accounting, or "unavailable" for uninstrumented callers.
    /// Provenance describes the measurement method; coverage describes whether this cohort was measured.
    std::string runnable_wait{"unavailable"};
    /// Truncation and loss are counted from persisted output rows only.
    std::string capture{"persisted_output_flags"};
};

/// Bounded group page over a live retained-history view. Both window bounds are always resolved.
struct StatisticsPage {
    UtcRange                     window;
    StatisticsGroupBy            group_by{StatisticsGroupBy::None};
    std::vector<StatisticsGroup> groups;
    std::optional<std::string>   next_cursor;
    StatisticsMeasurement        measurement;
};

} // namespace jb::jobu
