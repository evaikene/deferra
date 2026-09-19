#pragma once

#include "attempt.hpp"
#include "attribute.hpp"
#include "job.hpp"
#include "queue.hpp"
#include "result.hpp"
#include "retry_policy_priv.hpp"
#include "run.hpp"

#include <cstddef>
#include <optional>
#include <vector>

namespace jb::db {
class Database;
}

namespace jb::jobu {
class CronEngine;
}

namespace jb::jobu::detail {

/// Stable lexicographic cursor shared by attempt and output scans.
struct RecoveryAttemptKey {
    jb::core::Uuid run_id;
    AttemptNumber  attempt_number{1};

    auto operator==(RecoveryAttemptKey const&) const -> bool = default;
};

/// Recovery storage under exclusive database ownership on the database's owner thread.
/// Pages own their values and retain no query. Limits are 1..4096; cursors are exclusive.
/// UUID ordering follows the stored UUID bytes, not display strings or insertion order.
///
/// A run page checks ownership and aggregate history; attempt pages decode individual history
/// rows, and output pages check capture relationships without reading captured bytes. Callers
/// must exhaust all five scans for complete row and relationship validation, including
/// owners with no runs. A filtered
/// Running page is a repair candidate scan, not proof that the remaining database is valid.
/// Missing recurring work and drained suspensions are repair candidates, not scan failures.
class RecoveryRepository final {
public:
    RecoveryRepository(jb::db::Database& database, AttributeRegistry const& attributes) noexcept;

    [[nodiscard]] auto
    list_runs(std::size_t limit, std::optional<jb::core::Uuid> after_id = {}, std::optional<RunState> state = {})
        -> jb::core::Result<std::vector<JobRun>, jb::core::Error>;
    [[nodiscard]] auto list_attempts(std::size_t limit, std::optional<RecoveryAttemptKey> after = {})
        -> jb::core::Result<std::vector<JobAttempt>, jb::core::Error>;
    [[nodiscard]] auto list_outputs(std::size_t limit, std::optional<RecoveryAttemptKey> after = {})
        -> jb::core::Result<std::vector<RecoveryAttemptKey>, jb::core::Error>;

    /// Owner pages expose missing recurring work and suspending owners to repair callers.
    [[nodiscard]] auto list_jobs(std::size_t limit, std::optional<jb::core::Uuid> after_id = {})
        -> jb::core::Result<std::vector<JobDefinition>, jb::core::Error>;
    [[nodiscard]] auto list_queues(std::size_t limit, std::optional<jb::core::Uuid> after_id = {})
        -> jb::core::Result<std::vector<Queue>, jb::core::Error>;

    /// Re-reads one run and its relationships, suitable for a caller-owned repair transaction.
    /// Absence is an invariant failure; this method neither begins nor commits a transaction.
    [[nodiscard]] auto find_run(jb::core::Uuid const& id) -> jb::core::Result<JobRun, jb::core::Error>;

    /// Revalidates a Running candidate and reads current owners/policy in the caller's transaction.
    /// Call before interrupt_attempt(), in that same transaction, so numbering, policy and time
    /// errors precede writes. Uses immutable run attributes; never creates a speculative attempt.
    [[nodiscard]] auto find_retry_decision(RecoveryAttemptKey const& key, jb::core::UtcTimePoint recovery_time)
        -> jb::core::Result<RetryDecision, jb::core::Error>;

    /// Revalidates a Running run and its latest attempt, then completes that attempt as Interrupted
    /// and inserts empty capture with capture_lost=true. Existing output is an invariant failure.
    /// Requires a caller-owned transaction; this method neither begins nor commits one. On any
    /// failure the caller must roll back the entire unit, including writes that already succeeded.
    /// On success the caller must transition the run before committing; the temporary Running run
    /// with a Completed attempt is not a valid committed state. No retry policy is selected here.
    [[nodiscard]] auto interrupt_attempt(RecoveryAttemptKey const& key, jb::core::UtcTimePoint recovery_time)
        -> jb::core::Result<void, jb::core::Error>;

    /// Terminal branch after interrupt_attempt(), in the same caller-owned transaction and with
    /// the same timestamp. Requires the matching latest Interrupted attempt and lost-capture row.
    /// Changes only the Running run's state, completion time and result; duplicate/stale calls fail.
    /// The caller must add any required recurrence/suspension repairs before committing, or roll
    /// back the whole unit on failure. This primitive does not choose recovery policy or commit.
    [[nodiscard]] auto set_run_interrupted(RecoveryAttemptKey const& key, jb::core::UtcTimePoint recovery_time)
        -> jb::core::Result<void, jb::core::Error>;

    /// Retry branch after interrupt_attempt(), using the due time from find_retry_decision() in
    /// the same caller-owned transaction. Requires matching Interrupted history/lost capture and
    /// current nondeleted owners with RetryInterrupted policy. Changes only state and runnable time.
    /// Preserves first start/snapshot, leaves completion/result unset, and creates no next attempt.
    /// Add required suspension repairs before commit; roll back the entire unit on any failure.
    [[nodiscard]] auto set_run_retry_wait(RecoveryAttemptKey const& key,
                                          jb::core::UtcTimePoint    recovery_time,
                                          jb::core::UtcTimePoint    due_at) -> jb::core::Result<void, jb::core::Error>;

    /// Re-reads a terminal Interrupted run and creates its recurring successor from the current definition.
    /// Call after set_run_interrupted() in the same transaction. Manual/Once/deleted cases return false;
    /// a duplicate successor is an error. RetryWait must never use this terminal-only operation.
    /// Pass max(recovery_time, now sampled immediately before the transaction) as the exclusive bound.
    /// Returns true for one insertion; no commit is performed and failure requires whole-unit rollback.
    [[nodiscard]] auto insert_interrupted_successor(jb::core::Uuid const&    run_id,
                                                    jb::core::UtcTimePoint   lower_bound,
                                                    CronEngine const&        cron,
                                                    jb::core::UuidGenerator& uuid_generator)
        -> jb::core::Result<bool, jb::core::Error>;

    /// Re-reads a definition, its owner and schedule-owned work inside the caller's repair transaction.
    /// Inserts missing recurring work, including suspended/suspending definitions. Existing nonterminal
    /// work is preserved exactly; Once/deleted definitions return false. Unexpected conflicts fail.
    /// Uses the same bound/rollback contract as insert_interrupted_successor(); returns true on insertion.
    [[nodiscard]] auto repair_missing_successor(jb::core::Uuid const&    job_id,
                                                jb::core::UtcTimePoint   lower_bound,
                                                CronEngine const&        cron,
                                                jb::core::UuidGenerator& uuid_generator)
        -> jb::core::Result<bool, jb::core::Error>;

    /// Revalidates the owner and completes a drained Suspending state in the caller's transaction.
    /// Includes owners with no runs. RetryWait does not prevent completion; nothing resumes owners.
    /// Returns true only when changed, for counting after commit. Failure requires whole-unit rollback.
    [[nodiscard]] auto complete_drained_job_suspension(jb::core::Uuid const& job_id, jb::core::UtcTimePoint updated_at)
        -> jb::core::Result<bool, jb::core::Error>;

    /// Queue-only form of suspension repair, with the same transaction/change-result contract.
    /// Does not require a job: an empty queue can also be drained.
    [[nodiscard]] auto complete_drained_queue_suspension(jb::core::Uuid const&  queue_id,
                                                         jb::core::UtcTimePoint updated_at)
        -> jb::core::Result<bool, jb::core::Error>;

private:
    jb::db::Database&        _database;
    AttributeRegistry const& _attributes;
};

} // namespace jb::jobu::detail
