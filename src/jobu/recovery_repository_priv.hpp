#pragma once

#include "attempt.hpp"
#include "attribute.hpp"
#include "job.hpp"
#include "queue.hpp"
#include "result.hpp"
#include "run.hpp"

#include <cstddef>
#include <optional>
#include <vector>

namespace jb::db {
class Database;
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
/// Missing recurring work and drained suspensions remain inputs for later repair stages.
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

    /// Owner pages also expose missing recurring work and suspending owners to later stages.
    [[nodiscard]] auto list_jobs(std::size_t limit, std::optional<jb::core::Uuid> after_id = {})
        -> jb::core::Result<std::vector<JobDefinition>, jb::core::Error>;
    [[nodiscard]] auto list_queues(std::size_t limit, std::optional<jb::core::Uuid> after_id = {})
        -> jb::core::Result<std::vector<Queue>, jb::core::Error>;

    /// Re-reads one run and its relationships, suitable for a caller-owned repair transaction.
    /// Absence is an invariant failure; this method neither begins nor commits a transaction.
    [[nodiscard]] auto find_run(jb::core::Uuid const& id) -> jb::core::Result<JobRun, jb::core::Error>;

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

private:
    jb::db::Database&        _database;
    AttributeRegistry const& _attributes;
};

} // namespace jb::jobu::detail
