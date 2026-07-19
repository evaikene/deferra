/** @file database.hpp
 * @brief Defines the move-only generic database connection owned by application code.
 */
#pragma once

#include "driver.hpp"

#include <memory>
#include <optional>
#include <string_view>

namespace jb::db {

class Query;
class Transaction;

/** Owns one backend Driver and enforces generic connection lifetime and thread-affinity rules.
 *
 * Construct a Database with a concrete backend driver, call open(), then create Query objects that reference it. Use
 * transaction(), commit(), and rollback() for directly managed transactions, or Transaction::begin() for scoped
 * rollback. Queries and transaction guards must be destroyed before closing, moving, or destroying their Database. A
 * default-constructed Database is an invalid handle useful as a moved-from or deferred-selection value.
 */
class Database final {
public:
    /// Creates an invalid database with no driver.
    Database() noexcept;

    /// Creates a closed database by taking ownership of a backend driver.
    /// @param driver Concrete backend driver, or nullptr to create an invalid database.
    explicit Database(std::unique_ptr<Driver> driver);

    /// Closes an open backend best-effort and destroys its driver.
    /// @warning All Query objects and Transaction guards referencing this Database must already be destroyed.
    ~Database();

    /// Prevents copying exclusive backend ownership.
    /// @param other Database that would otherwise be copied.
    Database(Database const& other) = delete;

    /** Transfers an idle database connection from another object.
     * @param other Database to transfer from.
     * @warning The transfer requires no live Query objects or Transaction guard and, for an open database, the owning
     *          thread. If the requirement is not met, this object remains invalid and other records the corresponding
     *          error.
     */
    Database(Database&& other) noexcept;

    /// Prevents replacing a connection because that could hide close failures.
    /// @param other Database that would otherwise be copied.
    /// @return This Database, if assignment were supported.
    auto operator=(Database const& other) -> Database& = delete;

    /// Prevents replacing a connection because that could hide close failures.
    /// @param other Database that would otherwise be moved.
    /// @return This Database, if assignment were supported.
    auto operator=(Database&& other) -> Database& = delete;

    /// Tests whether this database owns a driver.
    /// @return True when backend operations may be requested after opening.
    [[nodiscard]] auto is_valid() const noexcept -> bool;

    /// Tests whether this database has an open backend connection.
    /// @return True after successful open() and before successful close().
    [[nodiscard]] auto is_open() const noexcept -> bool;

    /// Returns the selected backend name.
    /// @return Driver-owned name, or an empty view for an invalid database.
    [[nodiscard]] auto driver_name() const noexcept -> std::string_view;

    /// Returns the last error produced by a Database operation.
    /// @return A copy of the last error, or no value when none is stored.
    [[nodiscard]] auto last_error() const -> std::optional<jb::core::Error>;

    /// Opens and configures the selected backend on the calling thread.
    /// @return Success, or a generic/driver Error. Calling open() again on the owning thread succeeds without
    /// reopening.
    [[nodiscard]] auto open() -> jb::core::Result<void, jb::core::Error>;

    /// Closes the backend after verifying thread, Query, and Transaction lifetime rules.
    /// @return Success, or a generic/driver Error. An already closed valid database succeeds when no Query or guarded
    /// transaction is alive. A directly owned active transaction is rolled back before closing.
    auto close() -> jb::core::Result<void, jb::core::Error>;

    /// Begins a directly owned top-level transaction.
    /// @param mode Backend transaction-start behavior.
    /// @return Success, or a generic/driver Error. A nested top-level transaction is rejected.
    [[nodiscard]] auto transaction(TransactionMode mode = TransactionMode::Immediate)
        -> jb::core::Result<void, jb::core::Error>;

    /// Commits the directly owned active transaction.
    /// @return Success, or a generic/driver Error. A failed driver commit leaves the transaction active so rollback()
    /// can still be attempted.
    [[nodiscard]] auto commit() -> jb::core::Result<void, jb::core::Error>;

    /// Rolls back the directly owned active transaction.
    /// @return Success, or a generic/driver Error. A failed driver rollback poisons the open connection until close().
    [[nodiscard]] auto rollback() -> jb::core::Result<void, jb::core::Error>;

private:
    friend class Query;
    friend class Transaction;

    /// Begins a transaction owned by a Transaction guard.
    /// @param mode Backend transaction-start behavior.
    /// @return A nonzero ownership token, or a generic/driver Error.
    [[nodiscard]] auto begin_guarded_transaction(TransactionMode mode)
        -> jb::core::Result<std::uint64_t, jb::core::Error>;

    /// Commits the guarded transaction when its token matches.
    /// @param token Nonzero token assigned when the guard began.
    /// @return Success, or a generic/driver Error. A mismatch poisons the database.
    [[nodiscard]] auto commit_guarded_transaction(std::uint64_t token) -> jb::core::Result<void, jb::core::Error>;

    /// Rolls back the guarded transaction when its token matches.
    /// @param token Nonzero token assigned when the guard began.
    /// @return Success, or a generic/driver Error. A mismatch or driver rollback failure poisons the database.
    [[nodiscard]] auto rollback_guarded_transaction(std::uint64_t token) -> jb::core::Result<void, jb::core::Error>;

    /// Tests whether a token still identifies the guarded transaction.
    /// @param token Token to compare with current ownership state.
    /// @return True only when the guarded transaction remains active with this token.
    [[nodiscard]] auto owns_guarded_transaction(std::uint64_t token) const noexcept -> bool;

    struct Private;
    std::unique_ptr<Private> _data;
};

} // namespace jb::db
