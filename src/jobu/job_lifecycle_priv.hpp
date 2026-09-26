#pragma once

#include "job.hpp"
#include "result.hpp"

#include <cstdint>

namespace jb::db {
class Database;
}

namespace jb::jobu::detail {

/// Counts only Scheduled, Running, and RetryWait runs owned by one definition.
struct NonterminalRunCounts {
    std::uint64_t scheduled{0};
    std::uint64_t manual{0};
};

/// Checks the shared barrier cardinalities. This permits zero live runs for a one-time definition
/// during final-run reconciliation; validate_job_lifecycle() rejects that shape at a committed
/// validation boundary when the definition is still unfinished.
[[nodiscard]] constexpr auto
valid_nonterminal_run_relationship(JobState state, bool one_time, NonterminalRunCounts counts) noexcept -> bool
{
    if (counts.scheduled > 1 || counts.manual > 1) {
        return false;
    }
    if (is_terminal_job_state(state)) {
        return one_time && counts.scheduled == 0 && counts.manual == 0;
    }
    if (state == JobState::Deleted) {
        return true; // Deleted-owner validation remains with its existing callers.
    }
    if (counts.manual == 1 && counts.scheduled == 0) {
        return one_time; // An accepted manual run may outlive its one-time scheduled sibling.
    }
    return true;
}

/// Reconciles current-format one-time definitions without owning the caller's transaction.
/// The database and attribute registry must outlive this repository on their owner thread.
class JobLifecycleRepository final {
public:
    JobLifecycleRepository(jb::db::Database& database, AttributeRegistry const& attributes) noexcept;

    /// Requires a caller-owned transaction and a fully persisted terminal run. Returns true only
    /// when the current one-time definition changes state. Failure requires whole-unit rollback.
    [[nodiscard]] auto finish_after_terminal_run(jb::core::Uuid const& run_id, jb::core::UtcTimePoint transition_time)
        -> jb::core::Result<bool, jb::core::Error>;

    /// Validates the current definition's live-run relationships using bounded reads. The caller
    /// supplies a current-format definition read in the same database ownership context.
    [[nodiscard]] auto validate_job_lifecycle(JobDefinition const& job) -> jb::core::Result<void, jb::core::Error>;

private:
    jb::db::Database&        _database;
    AttributeRegistry const& _attributes;
};

} // namespace jb::jobu::detail
