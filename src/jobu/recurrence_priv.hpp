#pragma once

#include "attribute.hpp"
#include "job.hpp"
#include "result.hpp"
#include "run.hpp"

namespace jb::db {
class Database;
}

namespace jb::jobu {
class CronEngine;
}

namespace jb::jobu::detail {

/// Inserts one schedule-owned snapshot from a transaction-local definition, strictly after lower_bound.
/// Deleted and Once definitions return false. Suspended/suspending definitions retain their schedule.
/// The caller owns the transaction and must establish that no nonterminal schedule-owned run exists;
/// an insertion conflict is an error, never an already-done result. Returns true on insertion.
/// Roll back the whole unit on failure.
[[nodiscard]] auto insert_recurring_run(jb::db::Database&        database,
                                        AttributeRegistry const& attributes,
                                        CronEngine const&        cron,
                                        jb::core::UuidGenerator& uuid_generator,
                                        JobDefinition            definition,
                                        jb::core::UtcTimePoint lower_bound) -> jb::core::Result<bool, jb::core::Error>;

/// Re-reads the latest definition after a schedule-owned run becomes terminal in the caller's transaction.
/// Manual runs return false. Normal completion/cancellation passes max(terminal_at, old planned_at);
/// recovery passes max(recovery_time, freshly sampled now), without replaying historical ticks.
/// Does not commit or validate the completed run's persisted transition; the caller owns that boundary.
[[nodiscard]] auto insert_recurring_successor(jb::db::Database&        database,
                                              AttributeRegistry const& attributes,
                                              CronEngine const&        cron,
                                              jb::core::UuidGenerator& uuid_generator,
                                              JobRun const&            completed_run,
                                              jb::core::UtcTimePoint   lower_bound)
    -> jb::core::Result<bool, jb::core::Error>;

} // namespace jb::jobu::detail
