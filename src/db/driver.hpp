/** @file driver.hpp
 * @brief Defines the backend-driver contract used by generic database objects.
 */
#pragma once

#include "error.hpp"
#include "result.hpp"

#include <cstdint>
#include <memory>
#include <string_view>

namespace jb::db {

class Database;
class DriverQuery;
class Query;

/// Native transaction-start behavior requested from a database driver.
enum class TransactionMode : std::uint8_t {
    /// Defers native locking until the transaction first accesses data.
    Deferred,
    /// Acquires the backend's normal write reservation when the transaction begins.
    Immediate,
    /// Acquires the backend's strongest supported transaction lock when the transaction begins.
    Exclusive,
};

/** Abstracts one backend connection for the generic Database API.
 *
 * Backend authors derive from Driver and keep all native connection state in the implementation. Application code
 * selects a backend by constructing its concrete driver and transferring ownership to Database.
 */
class Driver {
public:
    /// Destroys a backend driver through its interface.
    virtual ~Driver() = default;

    /// Prevents copying native connection ownership.
    /// @param other Driver that would otherwise be copied.
    Driver(Driver const& other) = delete;

    /// Prevents moving drivers after construction.
    /// @param other Driver that would otherwise be moved.
    Driver(Driver&& other) = delete;

    /// Prevents assigning native connection ownership.
    /// @param other Driver that would otherwise be copied.
    /// @return This Driver, if assignment were supported.
    auto operator=(Driver const& other) -> Driver& = delete;

    /// Prevents move-assigning drivers after construction.
    /// @param other Driver that would otherwise be moved.
    /// @return This Driver, if assignment were supported.
    auto operator=(Driver&& other) -> Driver& = delete;

    /// Returns the stable backend name used for diagnostics and capability reporting.
    /// @return Backend-owned name valid for the lifetime of the driver.
    [[nodiscard]] virtual auto name() const noexcept -> std::string_view = 0;

protected:
    /// Constructs the base portion of a concrete backend driver.
    Driver() = default;

private:
    friend class Database;
    friend class Query;

    /// Opens and configures the native connection.
    /// @return Success or a stable project-owned Error.
    [[nodiscard]] virtual auto open() -> jb::core::Result<void, jb::core::Error> = 0;

    /// Closes the native connection and releases backend resources.
    /// @return Success or a stable project-owned Error.
    [[nodiscard]] virtual auto close() -> jb::core::Result<void, jb::core::Error> = 0;

    /// Tests whether the native connection is open.
    /// @return True when backend operations may be attempted.
    [[nodiscard]] virtual auto is_open() const noexcept -> bool = 0;

    /// Creates a backend query bound to this native connection.
    /// @return An owned query implementation or a stable project-owned Error.
    [[nodiscard]] virtual auto create_query() -> jb::core::Result<std::unique_ptr<DriverQuery>, jb::core::Error> = 0;

    /// Begins a native transaction.
    /// @param mode Requested native transaction-start behavior.
    /// @return Success or a stable project-owned Error.
    [[nodiscard]] virtual auto begin(TransactionMode mode) -> jb::core::Result<void, jb::core::Error> = 0;

    /// Commits the active native transaction.
    /// @return Success or a stable project-owned Error.
    [[nodiscard]] virtual auto commit() -> jb::core::Result<void, jb::core::Error> = 0;

    /// Rolls back the active native transaction.
    /// @return Success or a stable project-owned Error.
    [[nodiscard]] virtual auto rollback() -> jb::core::Result<void, jb::core::Error> = 0;
};

} // namespace jb::db
