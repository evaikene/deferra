#pragma once

#include "error.hpp"
#include "result.hpp"

#include <cstdint>

namespace jb::db {
class Database;
}

namespace jb::jobu::detail {

/// Fixed-size post-recovery snapshot of live definitions and persisted pending work.
/// Pending counts include future, suspended, blocked and unavailable-type work; they do not imply eligibility.
struct StartupInventory {
    std::uint64_t queues_active{};
    std::uint64_t queues_suspending{};
    std::uint64_t queues_suspended{};
    std::uint64_t jobs_active{};
    std::uint64_t jobs_suspending{};
    std::uint64_t jobs_suspended{};
    std::uint64_t runs_scheduled{};
    std::uint64_t runs_retry_wait{};
};

/// Reads metadata counts in one consistent, read-only transaction after recovery and before dispatch.
/// Excludes Deleted/terminal definitions and terminal run history. Unexpected Running work, malformed counts,
/// overflowing totals and storage/cleanup failures return an error with safe fixed diagnostics. All queries and
/// transaction ownership are released before return; the caller must fail startup on an error.
[[nodiscard]] auto read_startup_inventory(jb::db::Database& database)
    -> jb::core::Result<StartupInventory, jb::core::Error>;

} // namespace jb::jobu::detail
