#pragma once

#include "recovery.hpp"

#include <functional>

namespace jb::jobu::detail {

/// Owner-thread startup polling seam. The predicate is borrowed synchronously and never retained.
/// It must not throw, mutate the database, or reenter recovery. An empty predicate never stops.
/// Checks occur between pages/transactions and before commit. A stop rolls back the current
/// unit and returns jobu.recovery.cancelled; earlier committed units survive. A rollback failure
/// takes precedence over cancellation so a poisoned connection cannot look like a normal stop.
[[nodiscard]] auto recover_startup(jb::db::Database&            database,
                                   AttributeRegistry const&     attributes,
                                   CronEngine const&            cron,
                                   jb::core::UuidGenerator&     uuid_generator,
                                   jb::core::TimeSource&        time_source,
                                   RecoveryOptions              options,
                                   std::function<bool()> const& should_stop)
    -> jb::core::Result<RecoveryReport, jb::core::Error>;

} // namespace jb::jobu::detail
