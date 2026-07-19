/** @file fake_database_driver.hpp
 * @brief Defines deterministic database-driver state and implementations for generic database tests.
 */
#pragma once

#include "driver.hpp"
#include "driver_query.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace jb::test {

/// Shared configuration and observations for a FakeDatabaseDriver and its queries.
struct FakeDatabaseDriverState {
    bool open{false};

    std::optional<jb::core::Error> open_error;
    std::optional<jb::core::Error> close_error;
    std::optional<jb::core::Error> create_query_error;
    std::optional<jb::core::Error> prepare_error;
    std::optional<jb::core::Error> bind_error;
    std::optional<jb::core::Error> exec_error;
    std::optional<jb::core::Error> next_error;
    std::optional<jb::core::Error> finish_error;
    std::optional<jb::core::Error> begin_error;
    std::optional<jb::core::Error> commit_error;
    std::optional<jb::core::Error> rollback_error;

    std::vector<std::string>    parameter_names;
    jb::db::ExecutionInfo       execution_info;
    std::vector<jb::db::Record> records;

    std::vector<std::string>                           calls;
    std::string                                        prepared_sql;
    std::vector<std::pair<std::size_t, jb::db::Value>> bindings;
    std::size_t                                        next_record_index{0};
    std::size_t                                        clear_count{0};
    std::optional<jb::db::TransactionMode>             last_transaction_mode;
};

/// Deterministic Driver implementation used to test the generic database layer without SQLite.
class FakeDatabaseDriver final : public jb::db::Driver {
public:
    /// Creates a fake driver that records operations in shared state.
    /// @param state Configuration and observation state retained by the caller.
    explicit FakeDatabaseDriver(std::shared_ptr<FakeDatabaseDriverState> state);

    /// Returns the stable fake backend name.
    /// @return The string "fake".
    [[nodiscard]] auto name() const noexcept -> std::string_view override;

private:
    std::shared_ptr<FakeDatabaseDriverState> _state;

    [[nodiscard]] auto open() -> jb::core::Result<void, jb::core::Error> override;
    [[nodiscard]] auto close() -> jb::core::Result<void, jb::core::Error> override;
    [[nodiscard]] auto is_open() const noexcept -> bool override;
    [[nodiscard]] auto create_query()
        -> jb::core::Result<std::unique_ptr<jb::db::DriverQuery>, jb::core::Error> override;
    [[nodiscard]] auto begin(jb::db::TransactionMode mode) -> jb::core::Result<void, jb::core::Error> override;
    [[nodiscard]] auto commit() -> jb::core::Result<void, jb::core::Error> override;
    [[nodiscard]] auto rollback() -> jb::core::Result<void, jb::core::Error> override;
};

} // namespace jb::test
