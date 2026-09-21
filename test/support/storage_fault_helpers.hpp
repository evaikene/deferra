/// @file storage_fault_helpers.hpp
/// @brief Durable snapshots and safe-error assertions for SQLite-backed fault matrices.
#pragma once

#include "fault_database_driver.hpp"

#include "database.hpp"
#include "query.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace jb::test {

using StorageSnapshot = std::map<std::string, std::vector<std::vector<db::Value>>>;

/// Reads every column of the mutable JobU tables, including idempotency and secret references.
/// Call only with no pending injected read fault and no poisoned connection.
inline auto storage_snapshot(db::Database& database) -> StorageSnapshot
{
    StorageSnapshot result;
    for (auto const* table : {"jobu_queues",
                              "jobu_jobs",
                              "jobu_runs",
                              "jobu_attempts",
                              "jobu_attempt_output",
                              "jobu_idempotency",
                              "jobu_secrets",
                              "jobu_secret_refs"}) {
        db::Query query{database};
        // The first three columns contain every composite primary key used by these tables.
        REQUIRE(query.exec(std::string{"SELECT * FROM "} + table + " ORDER BY 1, 2, 3"));
        auto& rows = result[table];
        for (;;) {
            auto next = query.next();
            REQUIRE(next);
            if (!*next) {
                break;
            }
            auto& row = rows.emplace_back();
            for (std::size_t column = 0; column < query.record().count(); ++column) {
                row.push_back(query.value(column));
            }
        }
    }
    return result;
}

inline auto fault_error(std::string code = "db.io") -> core::Error
{
    return {.category = core::ErrorCategory::Io,
            .code     = std::move(code),
            .message  = "private-backend-marker",
            .detail   = "private-backend-marker"};
}

inline void check_safe_error(core::Error const& error, std::string_view code)
{
    CHECK(error.code == code);
    CHECK(error.message.find("private-backend-marker") == std::string::npos);
    CHECK(error.detail.find("private-backend-marker") == std::string::npos);
}

inline void require_consumed_faults(DatabaseFaultState const& state)
{
    REQUIRE_FALSE(state.faults.empty());
    for (auto const& fault : state.faults) {
        CHECK(fault.fired);
    }
}

} // namespace jb::test
