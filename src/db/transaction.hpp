/** @file transaction.hpp
 * @brief Defines the move-only RAII guard for generic database transactions.
 */
#pragma once

#include "database.hpp"

#include <cstdint>

namespace jb::db {

namespace detail {
/// Private white-box accessor used to verify defensive ownership-token handling.
struct TransactionAccess;
} // namespace detail

/** Owns one guarded top-level transaction and rolls it back when scope exits without successful completion.
 *
 * Create a guard with begin(), then call commit() or rollback() explicitly when appropriate. Declare the guard before
 * Query objects in the same scope so those queries are destroyed before automatic rollback during stack unwinding. The
 * referenced Database must outlive the guard.
 */
class Transaction final {
public:
    /// Creates an inactive transaction guard.
    Transaction() noexcept;

    /// Rolls back an active guarded transaction best-effort.
    /// @warning The referenced Database must still exist and all queries declared after this guard must be destroyed.
    ~Transaction();

    /// Prevents sharing transaction ownership.
    /// @param other Transaction that would otherwise be copied.
    Transaction(Transaction const& other) = delete;

    /// Transfers guarded transaction ownership and deactivates the source.
    /// @param other Transaction guard to transfer from.
    Transaction(Transaction&& other) noexcept;

    /// Prevents sharing transaction ownership.
    /// @param other Transaction that would otherwise be copied.
    /// @return This Transaction, if assignment were supported.
    auto operator=(Transaction const& other) -> Transaction& = delete;

    /// Prevents replacing an active guard because that could hide rollback failures.
    /// @param other Transaction that would otherwise be moved.
    /// @return This Transaction, if assignment were supported.
    auto operator=(Transaction&& other) -> Transaction& = delete;

    /// Begins a guarded top-level transaction on an open database.
    /// @param database Database that must outlive the returned guard.
    /// @param mode Backend transaction-start behavior.
    /// @return An active guard, or a generic/driver Error.
    [[nodiscard]] static auto begin(Database& database, TransactionMode mode = TransactionMode::Immediate)
        -> jb::core::Result<Transaction, jb::core::Error>;

    /// Tests whether this guard still owns an active transaction.
    /// @return True until successful completion or a rollback failure deactivates the guard.
    [[nodiscard]] auto is_active() const noexcept -> bool;

    /// Commits the guarded transaction.
    /// @return Success, or a generic/driver Error. A failed driver commit leaves this guard active so rollback() can be
    /// attempted.
    [[nodiscard]] auto commit() -> jb::core::Result<void, jb::core::Error>;

    /// Rolls back the guarded transaction.
    /// @return Success, or a generic/driver Error. A failed driver rollback deactivates this guard and poisons the open
    /// connection until it is closed.
    [[nodiscard]] auto rollback() -> jb::core::Result<void, jb::core::Error>;

private:
    friend struct detail::TransactionAccess;

    /// Creates an active guard for a token assigned by Database.
    /// @param database Database that owns the active native transaction.
    /// @param token Nonzero generic ownership token.
    Transaction(Database& database, std::uint64_t token) noexcept;

    Database*     _database{nullptr};
    std::uint64_t _token{0};
};

} // namespace jb::db
