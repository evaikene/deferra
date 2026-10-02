#pragma once

#include "error.hpp"
#include "result.hpp"
#include "run.hpp"
#include "storage_failure_priv.hpp"
#include "time_source.hpp"
#include "uuid.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace jb::db {
class Database;
}

namespace jb::jobu::detail {

/// One shared logical boundary. Ticks are elapsed microseconds within epoch, never UTC timestamps.
struct WaitSample {
    jb::core::UtcTimePoint utc_now;
    jb::core::Uuid         epoch;
    std::int64_t           tick_us{0};
    jb::core::TimePoint    monotonic_now;
};

/// Known wait includes the open tail without checkpointing it. Only a successful durable
/// claimant sets claimed; until_warning identifies the first microsecond strictly above policy.
struct WaitWarning {
    std::chrono::microseconds                known_wait{0};
    std::optional<std::chrono::microseconds> until_warning;
    bool                                     claimed{false};
};

enum class WaitQuality : std::uint8_t {
    Unmeasured,
    Complete,
    Partial,
};

/// Owning lightweight projection; no payload, output or attribute document is loaded.
struct WaitTiming {
    RunState                      state{RunState::Scheduled};
    std::int64_t                  runnable_wait_us{0};
    WaitQuality                   quality{WaitQuality::Unmeasured};
    std::optional<jb::core::Uuid> open_epoch;
    std::optional<std::int64_t>   open_tick_us;
    bool                          delay_warned{false};
};

/// Keeps durable-data origin explicit instead of reconstructing it from diagnostic text.
struct TelemetryFailure {
    jb::core::Error      error;
    StorageFailureOrigin origin{StorageFailureOrigin::Operation};
};

template <typename T>
using TelemetryResult = jb::core::Result<T, TelemetryFailure>;

/// Row arithmetic only. Accounting methods require the caller's top-level transaction;
/// on failure the caller rolls that whole transaction back. No method begins/commits a
/// transaction, retains a query or emits a signal. Calls are serialized on the DB owner thread.
/// Eligibility beyond pending run state is supplied by the enclosing management/scheduler policy.
class WaitRepository final {
public:
    explicit WaitRepository(jb::db::Database& database) noexcept;

    /// Reads and validates one required timing row and its run state; usable outside a transaction.
    [[nodiscard]] auto read(jb::core::Uuid const& run_id) -> TelemetryResult<WaitTiming>;

    /// Exclusive UUID-keyset page of open rows, with limits 1..4096. Includes either open
    /// field so malformed pairs cannot evade validation. Owns IDs and releases its query.
    [[nodiscard]] auto list_open(std::size_t limit, std::optional<jb::core::Uuid> after = {})
        -> TelemetryResult<std::vector<jb::core::Uuid>>;

    /// Pre-activation recovery only: discard an abandoned tail without interpreting its tick.
    /// Retains the checkpointed lower bound and warning flag, marks Partial and clears the pair.
    /// Requires the caller's repair transaction; returns false for an already closed row.
    [[nodiscard]] auto repair_abandoned(jb::core::Uuid const& run_id) -> TelemetryResult<bool>;

    /// Opens pending work if closed; an already-open current-epoch interval keeps its original tick.
    /// First observation changes Unmeasured to Partial; Complete and Partial retain their quality.
    [[nodiscard]] auto open_interval(jb::core::Uuid const& run_id, WaitSample const& sample)
        -> TelemetryResult<WaitTiming>;

    /// Adds the current-epoch open delta and clears both open fields. Closed rows are unchanged.
    [[nodiscard]] auto settle(jb::core::Uuid const& run_id, WaitSample const& sample) -> TelemetryResult<WaitTiming>;

    /// Adds the open delta and rebases its tick without closing. Closed rows are unchanged.
    [[nodiscard]] auto rebase(jb::core::Uuid const& run_id, WaitSample const& sample) -> TelemetryResult<WaitTiming>;

    /// The caller has established current eligibility. Validates and evaluates known wait,
    /// then conditionally claims delay_warned in the caller's transaction. Zero disables claims.
    /// Thresholds beyond the durable counter range cannot be crossed and produce no deadline.
    [[nodiscard]] auto claim_warning(jb::core::Uuid const&     run_id,
                                     WaitSample const&         sample,
                                     std::chrono::milliseconds threshold) -> TelemetryResult<WaitWarning>;

private:
    [[nodiscard]] auto accumulate(jb::core::Uuid const& run_id, WaitSample const& sample, bool close)
        -> TelemetryResult<WaitTiming>;
    [[nodiscard]] auto write(jb::core::Uuid const& run_id, WaitTiming const& timing) -> TelemetryResult<void>;

    jb::db::Database& _database;
};

} // namespace jb::jobu::detail
