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

/// Read-only recovery scans under exclusive database ownership on the database's owner thread.
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

private:
    jb::db::Database&        _database;
    AttributeRegistry const& _attributes;
};

} // namespace jb::jobu::detail
