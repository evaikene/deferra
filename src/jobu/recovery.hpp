/// @file recovery.hpp
/// @brief Synchronous startup validation and repair under exclusive database ownership.
#pragma once

#include "error.hpp"
#include "result.hpp"

#include <cstddef>
#include <cstdint>

namespace jb::core {
class TimeSource;
class UuidGenerator;
} // namespace jb::core

namespace jb::db {
class Database;
}

namespace jb::jobu {
class AttributeRegistry;
class CronEngine;

/// Bounds memory used by each keyset scan. Accepted batch sizes are 1..4096.
struct RecoveryOptions {
    std::size_t scan_batch_size{256};
};

/// Changes committed by this invocation, returned only after all recovery checks succeed.
/// A second invocation over unchanged recovered data reports zero changes.
/// Counts describe repairs, not external execution or the database's total history.
struct RecoveryReport {
    /// Previously Running attempts completed as Interrupted with lost-capture metadata.
    std::uint64_t interrupted_attempts{};
    /// Interrupted runs moved to RetryWait; their next attempts have not been created.
    std::uint64_t retrying_runs{};
    /// Interrupted runs made terminal instead of retried.
    std::uint64_t terminal_runs{};
    /// Recurring runs inserted during interruption or independent missing-successor repair.
    std::uint64_t inserted_successors{};
    /// Jobs changed from Suspending to Suspended after their running work was gone.
    std::uint64_t suspended_jobs{};
    /// Queues changed from Suspending to Suspended after their running work was gone.
    std::uint64_t suspended_queues{};
};

/// Validates and repairs durable state before admitting mutations or dispatching work.
///
/// Call on the open, idle, schema-prepared database's owner thread while holding exclusive
/// daemon database ownership. Dependencies are borrowed only for this call. No executor is invoked,
/// and no listener, event loop, or scheduler is started. Recovery uses current queue policy and
/// immutable run retry attributes; interrupted external outcomes remain unknown.
///
/// Each repair unit commits independently. Failure can leave earlier units committed; close
/// and reopen after storage failure and rerun recovery before serving. A commit error can also
/// mean the current unit committed but its acknowledgement was lost; inspect reopened state.
/// Repeated recovery is safe and does not duplicate retries or recurring successors. Success proves
/// no Running work remains, required recurring work exists, barriers are valid, and suspensions drained.
///
/// @return Committed change counts, `jobu.recovery.invalid_options` for an out-of-range batch
/// size, `jobu.recovery.invariant` for inconsistent durable state or counter overflow, or a
/// sanitized dependency/storage error retaining its stable code. No partial report is returned.
[[nodiscard]] auto recover_startup(jb::db::Database&        database,
                                   AttributeRegistry const& attributes,
                                   CronEngine const&        cron,
                                   jb::core::UuidGenerator& uuid_generator,
                                   jb::core::TimeSource&    time_source,
                                   RecoveryOptions options = {}) -> jb::core::Result<RecoveryReport, jb::core::Error>;

} // namespace jb::jobu
