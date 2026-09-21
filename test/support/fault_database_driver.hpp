/// @file fault_database_driver.hpp
/// @brief Test-only forwarding database driver with named, one-shot failure boundaries.
#pragma once

#include "driver.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace jb::test {

enum class DatabaseOperation : std::uint8_t {
    Open,
    Close,
    CreateQuery,
    Begin,
    Commit,
    Rollback,
    Prepare,
    Bind,
    Execute,
    Fetch,
    Finish
};
enum class DatabaseFaultPhase : std::uint8_t {
    Before,
    AfterSuccess
};

struct DatabaseCall {
    std::string        boundary;
    DatabaseOperation  operation;
    DatabaseFaultPhase phase{DatabaseFaultPhase::Before};
    auto               operator==(DatabaseCall const&) const -> bool = default;
};

struct DatabaseFault {
    DatabaseCall at;
    core::Error  error;
    bool         fired{false};
};

/// Owner-thread test controls shared by the decorator and its queries.
///
/// The classifier maps prepared SQL to logical names; it must own its captures and outlive queries. Driver operations
/// use the boundary "connection". A fault fires once at its exact boundary/operation/phase. AfterSuccess faults run
/// only after backend success, so a commit acknowledgement error cannot be mistaken for rollback evidence.
struct DatabaseFaultState {
    std::function<std::string(std::string_view)> classify;
    std::vector<DatabaseFault>                   faults;
    std::vector<DatabaseCall>                    calls;

    /// Records a checkpoint and consumes the first matching unconsumed fault, if any.
    [[nodiscard]] auto checkpoint(std::string_view boundary, DatabaseOperation operation, DatabaseFaultPhase phase)
        -> std::optional<core::Error>;
};

/// Owns any generic backend and wraps each query; no SQLite or JobU knowledge is embedded here.
/// Queries must be destroyed before the outer Database, as with an undecorated driver.
class FaultDatabaseDriver final : public db::Driver {
public:
    /// Both arguments must be non-null; the wrapped driver must be closed and exclusively owned.
    FaultDatabaseDriver(std::unique_ptr<db::Driver> driver, std::shared_ptr<DatabaseFaultState> state);
    [[nodiscard]] auto name() const noexcept -> std::string_view override;

private:
    std::unique_ptr<db::Driver>         _driver;
    std::shared_ptr<DatabaseFaultState> _state;

    [[nodiscard]] auto open() -> core::Result<void, core::Error> override;
    [[nodiscard]] auto close() -> core::Result<void, core::Error> override;
    [[nodiscard]] auto is_open() const noexcept -> bool override;
    [[nodiscard]] auto create_query() -> core::Result<std::unique_ptr<db::DriverQuery>, core::Error> override;
    [[nodiscard]] auto begin(db::TransactionMode mode) -> core::Result<void, core::Error> override;
    [[nodiscard]] auto commit() -> core::Result<void, core::Error> override;
    [[nodiscard]] auto rollback() -> core::Result<void, core::Error> override;
};

} // namespace jb::test
