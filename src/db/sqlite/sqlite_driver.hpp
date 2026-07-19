/** @file sqlite_driver.hpp
 * @brief Defines explicit application selection and configuration of the file-backed SQLite driver.
 */
#pragma once

#include "driver.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>

namespace jb::db::sqlite {

/** Selects SQLite write-durability behavior for a WAL database.
 *
 * Choose Normal for the usual performance/safety balance or Full when commits must be synchronized before returning.
 */
enum class Durability : std::uint8_t {
    /// Uses SQLite `synchronous=NORMAL` behavior.
    Normal,
    /// Uses SQLite `synchronous=FULL` behavior.
    Full,
};

/** Configures one file-backed SQLite connection.
 *
 * Set database_file to a normal filesystem path. The driver rejects empty paths, SQLite URI forms, and `:memory:`;
 * creates or opens the file; and holds a non-blocking adjacent `.lock` file while open.
 */
struct Options {
    /// Database file to create or open. Its parent directory must already exist.
    std::filesystem::path     database_file;
    /// Maximum time SQLite waits for a busy database, from zero through five seconds.
    std::chrono::milliseconds busy_timeout{1000};
    /// WAL synchronization policy applied and verified during open.
    Durability                durability{Durability::Normal};
};

/** Implements the generic database driver contract using one private file-backed SQLite connection.
 *
 * Construct Driver with Options, transfer it to jb::db::Database, and use the generic Database, Query, Record, Value,
 * and Transaction APIs. The driver enables WAL and foreign keys, applies the requested timeout and durability, and
 * prevents a second JobU connection from owning the same file through an adjacent process lock.
 */
class Driver final : public jb::db::Driver {
public:
    /// Creates a closed SQLite driver by taking ownership of its configuration.
    /// @param options File, timeout, and durability settings validated by Database::open().
    explicit Driver(Options options);

    /// Releases the native connection and adjacent process lock best-effort.
    ~Driver() override;

    /// Returns the stable backend name.
    /// @return The string `sqlite`.
    [[nodiscard]] auto name() const noexcept -> std::string_view override;

private:
    struct Private;
    std::unique_ptr<Private> _data;

    /// Validates options, acquires the adjacent lock, opens SQLite, and verifies required configuration.
    /// @return Success or a stable project-owned Error.
    [[nodiscard]] auto open() -> jb::core::Result<void, jb::core::Error> override;

    /// Closes SQLite and releases the adjacent lock.
    /// @return Success or a stable project-owned Error.
    [[nodiscard]] auto close() -> jb::core::Result<void, jb::core::Error> override;

    /// Tests whether the native SQLite connection is open.
    /// @return True while this driver owns a native connection.
    [[nodiscard]] auto is_open() const noexcept -> bool override;

    /// Creates a prepared-query implementation for this connection.
    /// @return An owned generic DriverQuery or a stable project-owned Error.
    [[nodiscard]] auto create_query()
        -> jb::core::Result<std::unique_ptr<jb::db::DriverQuery>, jb::core::Error> override;

    /// Begins a native top-level SQLite transaction.
    /// @param mode Deferred, immediate, or exclusive SQLite begin behavior.
    /// @return Success or a stable project-owned Error.
    [[nodiscard]] auto begin(jb::db::TransactionMode mode) -> jb::core::Result<void, jb::core::Error> override;

    /// Commits the native SQLite transaction.
    /// @return Success or a stable project-owned Error.
    [[nodiscard]] auto commit() -> jb::core::Result<void, jb::core::Error> override;

    /// Rolls back the native SQLite transaction.
    /// @return Success or a stable project-owned Error.
    [[nodiscard]] auto rollback() -> jb::core::Result<void, jb::core::Error> override;
};

} // namespace jb::db::sqlite
