#include "startup_inventory_priv.hpp"

#include "database.hpp"
#include "query.hpp"
#include "storage_failure_priv.hpp"
#include "transaction.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <variant>

namespace jb::jobu::detail {
namespace {

template <typename T>
using InventoryResult = jb::core::Result<T, jb::core::Error>;

using StateCounts = std::array<std::uint64_t, 3>;

auto invariant(std::string_view reason) -> jb::core::Error
{
    return {.category = jb::core::ErrorCategory::Internal,
            .code     = "jobu.storage.inventory_invariant",
            .message  = "Startup inventory violates a JobU invariant",
            .detail   = "reason=" + std::string{reason}};
}

auto storage_error(jb::core::Error const& error) -> jb::core::Error
{
    return sanitized_storage_error(error, StorageOperation::Read);
}

auto read_state_counts(jb::db::Database& database, std::string_view sql, std::array<std::string_view, 3> const& states)
    -> InventoryResult<StateCounts>
{
    jb::db::Query query{database};
    auto          executed = query.exec(sql);
    if (!executed) {
        return InventoryResult<StateCounts>::failure(storage_error(executed.error()));
    }

    StateCounts         counts{};
    std::array<bool, 3> seen{};
    std::uint64_t       total{};
    for (;;) {
        auto next = query.next();
        if (!next) {
            return InventoryResult<StateCounts>::failure(storage_error(next.error()));
        }
        if (!*next) {
            break;
        }

        // GROUP BY returns at most one row per selected state. Decode exact representations rather than
        // coercing backend values, and reject duplicates so malformed results cannot overwrite counts.
        auto const* state_value = query.value("inventory_state");
        auto const* state       = state_value ? std::get_if<std::string>(state_value) : nullptr;
        auto const* count_value = query.value("inventory_count");
        auto const* count       = count_value ? std::get_if<std::int64_t>(count_value) : nullptr;
        if (!state || !count || *count < 0) {
            return InventoryResult<StateCounts>::failure(invariant("invalid_count_row"));
        }
        auto const* position = std::ranges::find(states, *state);
        if (position == states.end()) {
            return InventoryResult<StateCounts>::failure(invariant("unexpected_state"));
        }
        auto const index = static_cast<std::size_t>(position - states.begin());
        if (seen[index]) {
            return InventoryResult<StateCounts>::failure(invariant("duplicate_state"));
        }
        auto const value = static_cast<std::uint64_t>(*count);
        if (value > std::numeric_limits<std::uint64_t>::max() - total) {
            return InventoryResult<StateCounts>::failure(invariant("count_overflow"));
        }
        total         += value;
        counts[index]  = value;
        seen[index]    = true;
    }

    // Check cursor cleanup explicitly; destruction alone cannot return a backend finish failure.
    auto finished = query.finish();
    if (!finished) {
        return InventoryResult<StateCounts>::failure(storage_error(finished.error()));
    }
    return InventoryResult<StateCounts>::success(counts);
}

} // namespace

auto read_startup_inventory(jb::db::Database& database) -> InventoryResult<StartupInventory>
{
    // One short snapshot spans all three tables. Only state/count projections cross the database boundary;
    // definitions, payloads, secrets, outputs and retained history are never materialized here.
    auto transaction = jb::db::Transaction::begin(database, jb::db::TransactionMode::Deferred);
    if (!transaction) {
        return InventoryResult<StartupInventory>::failure(storage_error(transaction.error()));
    }
    constexpr auto definition_states = std::array<std::string_view, 3>{"active", "suspending", "suspended"};
    auto queues = read_state_counts(database,
                                    "SELECT state AS inventory_state, COUNT(*) AS inventory_count FROM jobu_queues "
                                    "WHERE state IN ('active', 'suspending', 'suspended') GROUP BY state",
                                    definition_states);
    if (!queues) {
        return InventoryResult<StartupInventory>::failure(queues.error());
    }
    auto jobs = read_state_counts(database,
                                  "SELECT state AS inventory_state, COUNT(*) AS inventory_count FROM jobu_jobs "
                                  "WHERE state IN ('active', 'suspending', 'suspended') GROUP BY state",
                                  definition_states);
    if (!jobs) {
        return InventoryResult<StartupInventory>::failure(jobs.error());
    }

    // Pending is a stored state, independent of due time, owner suspension, barriers or executor availability.
    // Include Running in the projection solely to prove recovery completed before this boundary.
    auto runs = read_state_counts(database,
                                  "SELECT state AS inventory_state, COUNT(*) AS inventory_count FROM jobu_runs "
                                  "WHERE state IN ('scheduled', 'retry_wait', 'running') GROUP BY state",
                                  {"scheduled", "retry_wait", "running"});
    if (!runs) {
        return InventoryResult<StartupInventory>::failure(runs.error());
    }
    if ((*runs)[2] != 0) {
        return InventoryResult<StartupInventory>::failure(invariant("running_after_recovery"));
    }

    // Each helper has returned and destroyed its query before the commit and before any caller can log/dispatch.
    auto committed = transaction->commit();
    if (!committed) {
        return InventoryResult<StartupInventory>::failure(storage_error(committed.error()));
    }
    return InventoryResult<StartupInventory>::success({.queues_active     = (*queues)[0],
                                                       .queues_suspending = (*queues)[1],
                                                       .queues_suspended  = (*queues)[2],
                                                       .jobs_active       = (*jobs)[0],
                                                       .jobs_suspending   = (*jobs)[1],
                                                       .jobs_suspended    = (*jobs)[2],
                                                       .runs_scheduled    = (*runs)[0],
                                                       .runs_retry_wait   = (*runs)[1]});
}

} // namespace jb::jobu::detail
