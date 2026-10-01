#pragma once

#include "queue_repository_priv.hpp"
#include "result.hpp"
#include "run_repository_priv.hpp"

#include <chrono>
#include <cstddef>
#include <optional>

namespace jb::db {
class Database;
}

namespace jb::jobu::detail {

struct RetentionPurgeCounts {
    std::size_t runs{0};
    std::size_t idempotency_records{0};
    std::size_t jobs{0};
    std::size_t queues{0};
};

/// Queue progress only; each visit pages its run keyset within one transaction.
struct RetentionSweepCursor {
    std::optional<jb::core::Uuid> after_queue;
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

    /// Visits at most one queue, including empty/unlimited/deleted queues, per call.
    /// Reuse the same sweep_now and nonnegative daemon_retention throughout a sweep.
    /// Limit is 1..1000 run parents, not cascaded attempt counts or output bytes.
    /// A full batch still advances to the next queue; remaining history waits for a later sweep.
    /// End-of-sweep returns a reset cursor. Owner and replay retirement are deliberately deferred.
    /// Errors preserve stable codes with sanitized mutation diagnostics; retry after uncertain commit is unsafe.
    /// Classify jobu.retention.invalid_relationship with PersistedData origin at the service boundary.
    [[nodiscard]] auto purge_next_batch(jb::core::UtcTimePoint      sweep_now,
                                        std::chrono::seconds        daemon_retention,
                                        std::size_t                 limit,
                                        RetentionSweepCursor const& cursor)
        -> jb::core::Result<RetentionBatchResult, jb::core::Error>;

private:
    jb::db::Database& _database;
    RunRepository     _runs;
    QueueRepository   _queues;
};

} // namespace jb::jobu::detail
