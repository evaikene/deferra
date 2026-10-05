/// @file retention.hpp
/// @brief Owner-thread incremental retained-history maintenance.
#pragma once

#include "error.hpp"
#include "object.hpp"
#include "result.hpp"
#include "signal.hpp"

#include <chrono>
#include <cstddef>

namespace jb::db {
class Database;
}

namespace jb::core {
class TimeSource;
}

namespace jb::jobu {

class AttributeRegistry;

/// Committed deletions from one maintenance batch. All values are owning counts.
/// The parent bound applies to runs/jobs/queues, not attached replay records or
/// cascaded attempts/output/timing rows. Zero counts do not imply sweep completion.
struct RetentionPurgeCounts {
    std::size_t runs{0};                ///< Terminal runs removed, including their cascaded history.
    std::size_t idempotency_records{0}; ///< Replay records retired with their resource lifetime.
    std::size_t jobs{0};                ///< Soft-deleted definitions physically removed.
    std::size_t queues{0};              ///< Soft-deleted queues physically removed.
};

/// Immutable maintenance policy, copied by RetentionService.
struct RetentionOptions {
    std::chrono::seconds      default_retention{2'592'000}; ///< Nonnegative inherited policy; zero is unlimited.
    std::chrono::seconds      sweep_interval{60};           ///< Positive delay after a complete sweep.
    std::chrono::milliseconds inter_batch_delay{10};        ///< Positive delay before the first and each next batch.
    std::size_t               batch_size{100};              ///< 1..1000 parent deletions/owner candidates per batch.
};

/// Drives phased retention on the existing event loop, without a separate database writer.
/// Borrows an open, schema-prepared database, registry and clock; all must outlive the service.
/// Construct, start, stop and destroy on the database/event-loop owner thread. Start only
/// after recovery and runtime readiness. Construction/start/stop perform no SQL.
/// Each sweep samples UTC once. A run's persisted queue supplies its current inherited,
/// finite or unlimited policy; only terminal history strictly older than its cutoff is removed.
/// Unlimited defaults still permit finite queue overrides and eligible replay/deleted-owner cleanup.
/// Each timer callback finishes one repository transaction before emitting or yielding.
/// Cascades and one owner's attached replay records can make a batch's work size-dependent.
/// The optional parent owns the service; destruction disarms pending maintenance.
class RetentionService final : public jb::core::Object {
public:
    RetentionService(jb::db::Database&        database,
                     AttributeRegistry const& attributes,
                     jb::core::TimeSource&    time_source,
                     RetentionOptions         options = {},
                     jb::core::Object*        parent  = nullptr);
    ~RetentionService() override;

    RetentionService(RetentionService const&)                    = delete;
    RetentionService(RetentionService&&)                         = delete;
    auto operator=(RetentionService const&) -> RetentionService& = delete;
    auto operator=(RetentionService&&) -> RetentionService&      = delete;

    /// Validates options/owner and timer affinity, then arms the first asynchronous batch.
    /// Already running succeeds without changing sweep progress. After an ordinary stop,
    /// start begins a fresh sweep; after maintenance failure it returns the latched error.
    /// Rejects negative retention, nonpositive/unrepresentable timer delays or batch sizes
    /// outside 1..1000 with jobu.retention.invalid_options (InvalidArgument). Missing/invalid
    /// owner-thread event-loop affinity returns jobu.retention.event_loop_unavailable (Unavailable).
    /// Startup rejection emits no signal and performs no maintenance.
    [[nodiscard]] auto start() -> jb::core::Result<void, jb::core::Error>;

    /// Disarms maintenance and discards sweep continuation. Idempotent, owner-thread and non-SQL.
    /// Safe from either signal; a stopped activation cannot resume after a reentrant restart.
    void stop() noexcept;

    /// Emitted after each successful commit and complete query/transaction cleanup, including empty batches.
    /// Queued receivers get owning values. Direct receivers may stop or destroy the service.
    jb::core::Signal<RetentionPurgeCounts> batch_completed;

    /// Committed totals from one complete sweep, including sweeps with zero deletions.
    /// Emitted after the final batch notification and SQL cleanup, while that activation remains alive.
    /// A batch receiver stopping/destroying/restarting the service can suppress this diagnostic.
    /// Counts reset between sweeps and activations; stopped/failed partial sweeps emit no summary.
    /// Queued receivers own their totals. Direct receivers may stop, restart or destroy the service.
    jb::core::Signal<RetentionPurgeCounts> sweep_completed;

    /// Emitted once after failed maintenance has unwound transaction cleanup and disarmed work.
    /// Preserves trusted stable codes with sanitized Mutation diagnostics; persisted relationship
    /// failures are fatal. Unrepresentable sweep totals fail with jobu.retention.counter_overflow.
    /// No subsequent start, stop or callback retries a failed batch.
    jb::core::Signal<jb::core::Error> failed;

private:
    struct Private;
};

} // namespace jb::jobu
