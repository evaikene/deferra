#include "transaction.hpp"

#include "database_priv.hpp"
#include "logging.hpp"

#include <utility>

namespace jb::db {

namespace {

auto inactive_transaction_error() -> jb::core::Error
{
    return make_database_error(jb::core::ErrorCategory::Conflict,
                               "db.no_transaction",
                               "The transaction guard is inactive");
}

} // anonymous namespace

Transaction::Transaction() noexcept = default;

Transaction::~Transaction()
{
    if (!is_active()) {
        return;
    }
    auto rolled_back = rollback();
    if (!rolled_back) {
        auto const& error = rolled_back.error();
        jb::core::log_fatal("Failed to roll back guarded transaction during destruction: {} ({})",
                            error.message,
                            error.code);
    }
}

Transaction::Transaction(Transaction&& other) noexcept
    : _database{std::exchange(other._database, nullptr)}
    , _token{std::exchange(other._token, 0)}
{}

auto Transaction::begin(Database& database, TransactionMode mode) -> jb::core::Result<Transaction, jb::core::Error>
{
    auto begun = database.begin_guarded_transaction(mode);
    if (!begun) {
        return jb::core::Result<Transaction, jb::core::Error>::failure(std::move(begun).error());
    }
    return jb::core::Result<Transaction, jb::core::Error>::success(Transaction{database, begun.value()});
}

auto Transaction::is_active() const noexcept -> bool
{
    return _database != nullptr && _token != 0;
}

auto Transaction::commit() -> jb::core::Result<void, jb::core::Error>
{
    if (!is_active()) {
        return jb::core::Result<void, jb::core::Error>::failure(inactive_transaction_error());
    }

    auto* database = _database;
    auto  result   = database->commit_guarded_transaction(_token);
    if (!database->owns_guarded_transaction(_token)) {
        _database = nullptr;
        _token    = 0;
    }
    return result;
}

auto Transaction::rollback() -> jb::core::Result<void, jb::core::Error>
{
    if (!is_active()) {
        return jb::core::Result<void, jb::core::Error>::failure(inactive_transaction_error());
    }

    auto* database = _database;
    auto  result   = database->rollback_guarded_transaction(_token);
    if (!database->owns_guarded_transaction(_token)) {
        _database = nullptr;
        _token    = 0;
    }
    return result;
}

Transaction::Transaction(Database& database, std::uint64_t token) noexcept
    : _database{&database}
    , _token{token}
{}

} // namespace jb::db
