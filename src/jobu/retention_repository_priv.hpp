#pragma once

#include "idempotency_repository_priv.hpp"
#include "queue_repository_priv.hpp"
#include "result.hpp"
#include "retention.hpp"
#include "retention_owner_repository_priv.hpp"
#include "run_repository_priv.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace jb::db {
class Database;
}

namespace jb::jobu::detail {

enum class RetentionSweepPhase : std::uint8_t {
    History,
    OnceKeys,
    DeletedJobs,
    DeletedQueues
};

/// Owning progress only; no query, transaction or per-queue cursor map survives a call.
struct RetentionSweepCursor {
    std::optional<jb::core::Uuid> after_queue;
    RetentionSweepPhase           phase{RetentionSweepPhase::History};
    std::optional<jb::core::Uuid> after_owner;
};

/// Committed parent counts and owning continuation, with no live query or transaction.
struct RetentionBatchResult {
    RetentionPurgeCounts purged;
    RetentionSweepCursor next;
    bool                 sweep_complete{false};
};

class RetentionRepository final {
public:
    RetentionRepository(jb::db::Database& database, AttributeRegistry const& attributes) noexcept;

    /// Visits one queue or one bounded metadata/owner page per call.
    /// Reuse the same sweep_now and nonnegative daemon_retention throughout a sweep.
    /// Limit is 1..1000 resource parents (runs, jobs or queues) per phase. Attached
    /// replay retirement is atomic with its parent and counted separately, like cascaded
    /// timing/attempt/output rows. Reference reads use bounded pages; one owner's
    /// reference validation may traverse multiple pages synchronously.
    /// A full batch still advances to the next queue; remaining history waits for a later sweep.
    /// Only the final deleted-queue phase returns sweep completion and a reset cursor.
    /// Errors preserve stable codes with sanitized mutation diagnostics; retry after uncertain commit is unsafe.
    /// Classify jobu.retention.invalid_relationship with PersistedData origin at the service boundary.
    [[nodiscard]] auto purge_next_batch(jb::core::UtcTimePoint      sweep_now,
                                        std::chrono::seconds        daemon_retention,
                                        std::size_t                 limit,
                                        RetentionSweepCursor const& cursor)
        -> jb::core::Result<RetentionBatchResult, jb::core::Error>;

private:
    jb::db::Database&        _database;
    RunRepository            _runs;
    QueueRepository          _queues;
    IdempotencyRepository    _idempotency;
    RetentionOwnerRepository _owners;
    AttributeRegistry const& _attributes;
};

} // namespace jb::jobu::detail
