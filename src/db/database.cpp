#include "database.hpp"

#include "database_priv.hpp"
#include "logging.hpp"

#include <thread>
#include <utility>

namespace jb::db {

using DatabaseResult = jb::core::Result<void, jb::core::Error>;

Database::Database() noexcept
    : _data{std::make_unique<Private>()}
{}

Database::Database(std::unique_ptr<Driver> driver)
    : _data{std::make_unique<Private>(std::move(driver))}
{}

Database::~Database()
{
    if (!_data) {
        return;
    }
    if (_data->query_count != 0) {
        jb::core::log_fatal("Destroying a database with {} live queries", _data->query_count);
        return;
    }
    if (_data->transaction_owner == TransactionOwner::Guarded) {
        jb::core::log_fatal("Destroying a database with a live transaction guard");
        return;
    }
    if (_data->open) {
        if (_data->transaction_owner == TransactionOwner::Direct) {
            auto rolled_back = _data->driver->rollback();
            if (!rolled_back) {
                auto const& error = rolled_back.error();
                jb::core::log_fatal("Failed to roll back database during destruction: {} ({})",
                                    error.message,
                                    error.code);
            }
        }
        auto closed = _data->driver->close();
        if (!closed) {
            auto const& error = closed.error();
            jb::core::log_fatal("Failed to close database during destruction: {} ({})", error.message, error.code);
        }
    }
}

Database::Database(Database&& other) noexcept
{
    if (!other._data) {
        return;
    }
    if (other._data->query_count != 0) {
        other._data->last_error =
            make_database_error(jb::core::ErrorCategory::Conflict, "db.query_active", "The database has live queries");
        return;
    }
    if (other._data->transaction_owner == TransactionOwner::Guarded) {
        other._data->last_error = make_database_error(jb::core::ErrorCategory::Conflict,
                                                      "db.transaction_guard_active",
                                                      "The database has a live transaction guard");
        return;
    }
    if (other._data->open && other._data->owner_thread != std::this_thread::get_id()) {
        other._data->last_error = make_database_error(jb::core::ErrorCategory::Internal,
                                                      "db.wrong_thread",
                                                      "The database cannot be moved from a different thread");
        return;
    }
    _data = std::move(other._data);
}

auto Database::is_valid() const noexcept -> bool
{
    return _data && _data->driver;
}

auto Database::is_open() const noexcept -> bool
{
    return is_valid() && _data->open && _data->driver->is_open();
}

auto Database::driver_name() const noexcept -> std::string_view
{
    return is_valid() ? _data->driver->name() : std::string_view{};
}

auto Database::last_error() const -> std::optional<jb::core::Error>
{
    return _data ? _data->last_error : std::nullopt;
}

auto Database::open() -> DatabaseResult
{
    if (!_data) {
        _data = std::make_unique<Private>();
    }
    if (!_data->driver) {
        return _data->failure(make_database_error(jb::core::ErrorCategory::InvalidArgument,
                                                  "db.invalid_database",
                                                  "The database has no driver"));
    }
    if (_data->open) {
        if (_data->owner_thread != std::this_thread::get_id()) {
            return _data->failure(make_database_error(jb::core::ErrorCategory::Internal,
                                                      "db.wrong_thread",
                                                      "The database operation was requested from a different thread"));
        }
        if (_data->poisoned) {
            return _data->failure(
                make_database_error(jb::core::ErrorCategory::Internal,
                                    "db.connection_failed",
                                    "The database connection is unusable after an unrecoverable failure"));
        }
        _data->last_error.reset();
        return DatabaseResult::success();
    }

    auto opened = _data->driver->open();
    if (!opened) {
        return _data->failure(std::move(opened).error());
    }

    _data->open         = true;
    _data->poisoned     = false;
    _data->owner_thread = std::this_thread::get_id();
    _data->last_error.reset();
    return DatabaseResult::success();
}

auto Database::close() -> DatabaseResult
{
    if (!_data || !_data->driver) {
        if (!_data) {
            _data = std::make_unique<Private>();
        }
        return _data->failure(make_database_error(jb::core::ErrorCategory::InvalidArgument,
                                                  "db.invalid_database",
                                                  "The database has no driver"));
    }
    if (_data->open && _data->owner_thread != std::this_thread::get_id()) {
        return _data->failure(make_database_error(jb::core::ErrorCategory::Internal,
                                                  "db.wrong_thread",
                                                  "The database operation was requested from a different thread"));
    }
    if (_data->query_count != 0) {
        return _data->failure(
            make_database_error(jb::core::ErrorCategory::Conflict, "db.query_active", "The database has live queries"));
    }
    if (_data->transaction_owner == TransactionOwner::Guarded) {
        return _data->failure(make_database_error(jb::core::ErrorCategory::Conflict,
                                                  "db.transaction_guard_active",
                                                  "A transaction guard owns the active transaction"));
    }
    if (!_data->open) {
        _data->poisoned = false;
        _data->last_error.reset();
        return DatabaseResult::success();
    }

    if (_data->transaction_owner == TransactionOwner::Direct) {
        auto rolled_back         = _data->driver->rollback();
        _data->transaction_owner = TransactionOwner::None;
        if (!rolled_back) {
            _data->poisoned = true;
            return _data->failure(std::move(rolled_back).error());
        }
    }

    auto closed = _data->driver->close();
    if (!closed) {
        return _data->failure(std::move(closed).error());
    }

    _data->open              = false;
    _data->poisoned          = false;
    _data->transaction_owner = TransactionOwner::None;
    _data->transaction_token = 0;
    _data->owner_thread.reset();
    _data->last_error.reset();
    return DatabaseResult::success();
}

auto Database::transaction(TransactionMode mode) -> DatabaseResult
{
    if (!_data) {
        _data = std::make_unique<Private>();
    }
    if (auto error = _data->operation_error()) {
        return _data->failure(std::move(*error));
    }
    if (_data->transaction_owner != TransactionOwner::None) {
        return _data->failure(make_database_error(jb::core::ErrorCategory::Conflict,
                                                  "db.transaction_active",
                                                  "The database already has an active transaction"));
    }

    auto begun = _data->driver->begin(mode);
    if (!begun) {
        return _data->failure(std::move(begun).error());
    }

    _data->transaction_owner = TransactionOwner::Direct;
    _data->last_error.reset();
    return DatabaseResult::success();
}

auto Database::commit() -> DatabaseResult
{
    if (!_data) {
        _data = std::make_unique<Private>();
    }
    if (auto error = _data->operation_error()) {
        return _data->failure(std::move(*error));
    }
    if (_data->transaction_owner == TransactionOwner::None) {
        return _data->failure(make_database_error(jb::core::ErrorCategory::Conflict,
                                                  "db.no_transaction",
                                                  "The database has no active transaction"));
    }
    if (_data->transaction_owner == TransactionOwner::Guarded) {
        return _data->failure(make_database_error(jb::core::ErrorCategory::Conflict,
                                                  "db.transaction_guard_active",
                                                  "A transaction guard owns the active transaction"));
    }

    auto committed = _data->driver->commit();
    if (!committed) {
        return _data->failure(std::move(committed).error());
    }

    _data->transaction_owner = TransactionOwner::None;
    _data->last_error.reset();
    return DatabaseResult::success();
}

auto Database::rollback() -> DatabaseResult
{
    if (!_data) {
        _data = std::make_unique<Private>();
    }
    if (auto error = _data->operation_error()) {
        return _data->failure(std::move(*error));
    }
    if (_data->transaction_owner == TransactionOwner::None) {
        return _data->failure(make_database_error(jb::core::ErrorCategory::Conflict,
                                                  "db.no_transaction",
                                                  "The database has no active transaction"));
    }
    if (_data->transaction_owner == TransactionOwner::Guarded) {
        return _data->failure(make_database_error(jb::core::ErrorCategory::Conflict,
                                                  "db.transaction_guard_active",
                                                  "A transaction guard owns the active transaction"));
    }

    auto rolled_back         = _data->driver->rollback();
    _data->transaction_owner = TransactionOwner::None;
    if (!rolled_back) {
        _data->poisoned = true;
        return _data->failure(std::move(rolled_back).error());
    }

    _data->last_error.reset();
    return DatabaseResult::success();
}

auto Database::begin_guarded_transaction(TransactionMode mode) -> jb::core::Result<std::uint64_t, jb::core::Error>
{
    if (!_data) {
        _data = std::make_unique<Private>();
    }
    if (auto error = _data->operation_error()) {
        return _data->failure<std::uint64_t>(std::move(*error));
    }
    if (_data->transaction_owner != TransactionOwner::None) {
        return _data->failure<std::uint64_t>(make_database_error(jb::core::ErrorCategory::Conflict,
                                                                 "db.transaction_active",
                                                                 "The database already has an active transaction"));
    }

    auto begun = _data->driver->begin(mode);
    if (!begun) {
        return _data->failure<std::uint64_t>(std::move(begun).error());
    }

    auto const token = _data->next_transaction_token++;
    if (_data->next_transaction_token == 0) {
        _data->next_transaction_token = 1;
    }
    _data->transaction_owner = TransactionOwner::Guarded;
    _data->transaction_token = token;
    _data->last_error.reset();
    return jb::core::Result<std::uint64_t, jb::core::Error>::success(token);
}

auto Database::commit_guarded_transaction(std::uint64_t token) -> DatabaseResult
{
    if (!_data) {
        _data = std::make_unique<Private>();
    }
    if (auto error = _data->operation_error()) {
        return _data->failure(std::move(*error));
    }
    if (_data->transaction_owner != TransactionOwner::Guarded || token == 0 || token != _data->transaction_token) {
        _data->transaction_owner = TransactionOwner::None;
        _data->transaction_token = 0;
        _data->poisoned          = true;
        return _data->failure(make_database_error(jb::core::ErrorCategory::Internal,
                                                  "db.transaction_owner_mismatch",
                                                  "The transaction guard does not own the active transaction"));
    }

    auto committed = _data->driver->commit();
    if (!committed) {
        return _data->failure(std::move(committed).error());
    }

    _data->transaction_owner = TransactionOwner::None;
    _data->transaction_token = 0;
    _data->last_error.reset();
    return DatabaseResult::success();
}

auto Database::rollback_guarded_transaction(std::uint64_t token) -> DatabaseResult
{
    if (!_data) {
        _data = std::make_unique<Private>();
    }
    if (auto error = _data->operation_error()) {
        return _data->failure(std::move(*error));
    }
    if (_data->transaction_owner != TransactionOwner::Guarded || token == 0 || token != _data->transaction_token) {
        _data->transaction_owner = TransactionOwner::None;
        _data->transaction_token = 0;
        _data->poisoned          = true;
        return _data->failure(make_database_error(jb::core::ErrorCategory::Internal,
                                                  "db.transaction_owner_mismatch",
                                                  "The transaction guard does not own the active transaction"));
    }

    auto rolled_back         = _data->driver->rollback();
    _data->transaction_owner = TransactionOwner::None;
    _data->transaction_token = 0;
    if (!rolled_back) {
        _data->poisoned = true;
        return _data->failure(std::move(rolled_back).error());
    }

    _data->last_error.reset();
    return DatabaseResult::success();
}

auto Database::owns_guarded_transaction(std::uint64_t token) const noexcept -> bool
{
    return _data && token != 0 && _data->transaction_owner == TransactionOwner::Guarded &&
           _data->transaction_token == token;
}

} // namespace jb::db
