#include "sqlite_driver.hpp"

#include "sqlite_driver_priv.hpp"
#include "sqlite_error.hpp"
#include "sqlite_query_priv.hpp"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <string>
#include <string_view>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace jb::db::sqlite {

namespace {

using VoidResult = jb::core::Result<void, jb::core::Error>;

auto options_error(std::string_view code, std::string_view message) -> jb::core::Error
{
    return {
        .category = jb::core::ErrorCategory::InvalidArgument,
        .code     = std::string{code},
        .message  = std::string{message},
    };
}

auto configuration_error(std::string_view message) -> jb::core::Error
{
    return {
        .category = jb::core::ErrorCategory::Internal,
        .code     = "db.sqlite.configuration_failed",
        .message  = std::string{message},
    };
}

auto filesystem_error(int error_number, std::string_view fallback_code, std::string_view message) -> jb::core::Error
{
    auto const permission_denied = error_number == EACCES || error_number == EPERM;
    return {
        .category = permission_denied ? jb::core::ErrorCategory::PermissionDenied : jb::core::ErrorCategory::Io,
        .code     = permission_denied ? "db.permission_denied" : std::string{fallback_code},
        .message  = std::string{message},
        .detail   = "errno=" + std::to_string(error_number) + " message=" + std::strerror(error_number),
    };
}

template <typename Data>
void release_lock(Data& data) noexcept
{
    if (data.lock_fd >= 0) {
        static_cast<void>(::close(data.lock_fd));
        data.lock_fd = -1;
    }
}

template <typename Data>
void cleanup_failed_open(Data& data) noexcept
{
    if (data.connection) {
        static_cast<void>(sqlite3_close_v2(data.connection));
        data.connection = nullptr;
    }
    release_lock(data);
}

auto normalize_database_path(Options const& options) -> jb::core::Result<std::filesystem::path, jb::core::Error>
{
    auto const raw_path = options.database_file.string();
    if (raw_path.empty() || options.database_file.filename().empty()) {
        return jb::core::Result<std::filesystem::path, jb::core::Error>::failure(
            options_error("db.sqlite.invalid_database_file", "The SQLite database file path must not be empty"));
    }
    if (raw_path == ":memory:" || raw_path.starts_with("file:") || raw_path.find('\0') != std::string::npos) {
        return jb::core::Result<std::filesystem::path, jb::core::Error>::failure(
            options_error("db.sqlite.invalid_database_file",
                          "The SQLite driver requires a normal file-backed database path"));
    }

    auto const timeout = options.busy_timeout.count();
    if (timeout < 0 || timeout > 5000) {
        return jb::core::Result<std::filesystem::path, jb::core::Error>::failure(
            options_error("db.sqlite.invalid_busy_timeout",
                          "The SQLite busy timeout must be between zero and five seconds"));
    }
    if (options.durability != Durability::Normal && options.durability != Durability::Full) {
        return jb::core::Result<std::filesystem::path, jb::core::Error>::failure(
            options_error("db.sqlite.invalid_durability", "The SQLite durability setting is invalid"));
    }

    std::error_code error;
    auto            absolute_path = std::filesystem::absolute(options.database_file, error);
    if (error) {
        return jb::core::Result<std::filesystem::path, jb::core::Error>::failure(
            filesystem_error(error.value(), "db.sqlite.open_failed", "Unable to resolve the SQLite database path"));
    }
    absolute_path = absolute_path.lexically_normal();

    auto const parent = absolute_path.parent_path();
    if (!std::filesystem::exists(parent, error) || error || !std::filesystem::is_directory(parent, error) || error) {
        auto result = jb::core::Error{
            .category = jb::core::ErrorCategory::Io,
            .code     = "db.sqlite.open_failed",
            .message  = "The SQLite database parent directory does not exist or is not accessible",
        };
        if (error) {
            result.detail = "filesystem=" + error.message();
            if (error == std::errc::permission_denied) {
                result.category = jb::core::ErrorCategory::PermissionDenied;
                result.code     = "db.permission_denied";
            }
        }
        return jb::core::Result<std::filesystem::path, jb::core::Error>::failure(std::move(result));
    }

    auto canonical_path = std::filesystem::weakly_canonical(absolute_path, error);
    if (error) {
        return jb::core::Result<std::filesystem::path, jb::core::Error>::failure(
            filesystem_error(error.value(), "db.sqlite.open_failed", "Unable to normalize the SQLite database path"));
    }
    error.clear();
    auto const path_exists = std::filesystem::exists(canonical_path, error);
    if (error) {
        auto result = jb::core::Error{
            .category = jb::core::ErrorCategory::Io,
            .code     = "db.sqlite.open_failed",
            .message  = "Unable to inspect the SQLite database path",
            .detail   = "filesystem=" + error.message(),
        };
        if (error == std::errc::permission_denied) {
            result.category = jb::core::ErrorCategory::PermissionDenied;
            result.code     = "db.permission_denied";
        }
        return jb::core::Result<std::filesystem::path, jb::core::Error>::failure(std::move(result));
    }
    if (path_exists && std::filesystem::is_directory(canonical_path, error)) {
        return jb::core::Result<std::filesystem::path, jb::core::Error>::failure({
            .category = jb::core::ErrorCategory::Io,
            .code     = "db.sqlite.open_failed",
            .message  = "The SQLite database path must identify a file",
        });
    }
    if (error) {
        return jb::core::Result<std::filesystem::path, jb::core::Error>::failure(
            filesystem_error(error.value(), "db.sqlite.open_failed", "Unable to inspect the SQLite database path"));
    }
    return jb::core::Result<std::filesystem::path, jb::core::Error>::success(std::move(canonical_path));
}

template <typename Data>
auto acquire_lock(Data& data) -> VoidResult
{
    data.lock_path        = data.database_path;
    data.lock_path       += ".lock";
    auto const lock_path  = data.lock_path.string();
    data.lock_fd          = ::open(lock_path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, S_IRUSR | S_IWUSR);
    if (data.lock_fd < 0) {
        return VoidResult::failure(
            filesystem_error(errno, "db.sqlite.open_failed", "Unable to open the adjacent SQLite lock file"));
    }
    if (::flock(data.lock_fd, LOCK_EX | LOCK_NB) != 0) {
        auto const error_number = errno;
        release_lock(data);
        if (error_number == EWOULDBLOCK || error_number == EAGAIN) {
            return VoidResult::failure({
                .category = jb::core::ErrorCategory::Conflict,
                .code     = "db.sqlite.already_in_use",
                .message  = "The SQLite database is already owned by another JobU connection",
            });
        }
        return VoidResult::failure(
            filesystem_error(error_number, "db.sqlite.open_failed", "Unable to lock the adjacent SQLite lock file"));
    }
    return VoidResult::success();
}

auto execute(sqlite3* connection, char const* sql, std::string_view message) -> VoidResult
{
    auto const result = sqlite3_exec(connection, sql, nullptr, nullptr, nullptr);
    if (result != SQLITE_OK) {
        return VoidResult::failure(detail::make_sqlite_error(connection,
                                                             result,
                                                             "db.exec_failed",
                                                             jb::core::ErrorCategory::Internal,
                                                             message));
    }
    return VoidResult::success();
}

auto scalar_integer(sqlite3* connection, std::string_view sql, std::string_view message)
    -> jb::core::Result<std::int64_t, jb::core::Error>
{
    sqlite3_stmt* statement{nullptr};
    auto const    prepared =
        sqlite3_prepare_v3(connection, sql.data(), static_cast<int>(sql.size()), 0, &statement, nullptr);
    if (prepared != SQLITE_OK) {
        return jb::core::Result<std::int64_t, jb::core::Error>::failure(
            detail::make_sqlite_error(connection,
                                      prepared,
                                      "db.prepare_failed",
                                      jb::core::ErrorCategory::Internal,
                                      message));
    }
    auto const stepped = sqlite3_step(statement);
    if (stepped != SQLITE_ROW || sqlite3_column_type(statement, 0) != SQLITE_INTEGER) {
        auto error = stepped == SQLITE_ROW ? configuration_error(message)
                                           : detail::make_sqlite_error(connection,
                                                                       stepped,
                                                                       "db.fetch_failed",
                                                                       jb::core::ErrorCategory::Internal,
                                                                       message);
        static_cast<void>(sqlite3_finalize(statement));
        return jb::core::Result<std::int64_t, jb::core::Error>::failure(std::move(error));
    }
    auto const value = sqlite3_column_int64(statement, 0);
    static_cast<void>(sqlite3_finalize(statement));
    return jb::core::Result<std::int64_t, jb::core::Error>::success(value);
}

auto scalar_text(sqlite3* connection, std::string_view sql, std::string_view message)
    -> jb::core::Result<std::string, jb::core::Error>
{
    sqlite3_stmt* statement{nullptr};
    auto const    prepared =
        sqlite3_prepare_v3(connection, sql.data(), static_cast<int>(sql.size()), 0, &statement, nullptr);
    if (prepared != SQLITE_OK) {
        return jb::core::Result<std::string, jb::core::Error>::failure(
            detail::make_sqlite_error(connection,
                                      prepared,
                                      "db.prepare_failed",
                                      jb::core::ErrorCategory::Internal,
                                      message));
    }
    auto const stepped = sqlite3_step(statement);
    if (stepped != SQLITE_ROW || sqlite3_column_type(statement, 0) != SQLITE_TEXT) {
        auto error = stepped == SQLITE_ROW ? configuration_error(message)
                                           : detail::make_sqlite_error(connection,
                                                                       stepped,
                                                                       "db.fetch_failed",
                                                                       jb::core::ErrorCategory::Internal,
                                                                       message);
        static_cast<void>(sqlite3_finalize(statement));
        return jb::core::Result<std::string, jb::core::Error>::failure(std::move(error));
    }
    auto const* bytes = sqlite3_column_text(statement, 0);
    auto const  size  = sqlite3_column_bytes(statement, 0);
    if (!bytes && size != 0) {
        auto error = detail::make_sqlite_error(connection,
                                               SQLITE_NOMEM,
                                               "db.fetch_failed",
                                               jb::core::ErrorCategory::ResourceExhausted,
                                               message);
        static_cast<void>(sqlite3_finalize(statement));
        return jb::core::Result<std::string, jb::core::Error>::failure(std::move(error));
    }
    auto const* text  = bytes ? reinterpret_cast<char const*>(bytes) : "";
    auto        value = std::string{text, static_cast<std::size_t>(size)};
    static_cast<void>(sqlite3_finalize(statement));
    return jb::core::Result<std::string, jb::core::Error>::success(std::move(value));
}

template <typename Data>
auto configure_connection(Data& data) -> VoidResult
{
    auto result = sqlite3_extended_result_codes(data.connection, 1);
    if (result != SQLITE_OK) {
        return VoidResult::failure(detail::make_sqlite_error(data.connection,
                                                             result,
                                                             "db.sqlite.configuration_failed",
                                                             jb::core::ErrorCategory::Internal,
                                                             "Unable to enable extended SQLite result codes"));
    }
    result = sqlite3_busy_timeout(data.connection, static_cast<int>(data.options.busy_timeout.count()));
    if (result != SQLITE_OK) {
        return VoidResult::failure(detail::make_sqlite_error(data.connection,
                                                             result,
                                                             "db.sqlite.configuration_failed",
                                                             jb::core::ErrorCategory::Internal,
                                                             "Unable to set the SQLite busy timeout"));
    }

    auto foreign_keys = execute(data.connection, "PRAGMA foreign_keys=ON", "Unable to enable SQLite foreign keys");
    if (!foreign_keys) {
        return foreign_keys;
    }
    auto foreign_keys_value =
        scalar_integer(data.connection, "PRAGMA foreign_keys", "Unable to verify SQLite foreign keys");
    if (!foreign_keys_value) {
        return VoidResult::failure(std::move(foreign_keys_value).error());
    }
    if (foreign_keys_value.value() != 1) {
        return VoidResult::failure(configuration_error("SQLite foreign-key enforcement could not be enabled"));
    }

    auto journal_mode = scalar_text(data.connection, "PRAGMA journal_mode=WAL", "Unable to enable SQLite WAL mode");
    if (!journal_mode) {
        return VoidResult::failure(std::move(journal_mode).error());
    }
    auto mode = std::move(journal_mode).value();
    for (auto& character : mode) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    if (mode != "wal") {
        return VoidResult::failure(configuration_error("SQLite WAL journal mode could not be enabled"));
    }

    auto const synchronous_sql =
        data.options.durability == Durability::Normal ? "PRAGMA synchronous=NORMAL" : "PRAGMA synchronous=FULL";
    auto synchronous = execute(data.connection, synchronous_sql, "Unable to set SQLite durability");
    if (!synchronous) {
        return synchronous;
    }
    auto synchronous_value =
        scalar_integer(data.connection, "PRAGMA synchronous", "Unable to verify SQLite durability");
    if (!synchronous_value) {
        return VoidResult::failure(std::move(synchronous_value).error());
    }
    auto const expected = data.options.durability == Durability::Normal ? 1 : 2;
    if (synchronous_value.value() != expected) {
        return VoidResult::failure(configuration_error("SQLite durability could not be applied"));
    }

    return VoidResult::success();
}

auto execute_transaction(sqlite3* connection, char const* sql, std::string_view message) -> VoidResult
{
    return execute(connection, sql, message);
}

} // anonymous namespace

Driver::Driver(Options options)
    : _data{std::make_unique<Private>(std::move(options))}
{}

Driver::~Driver()
{
    if (_data->connection) {
        static_cast<void>(sqlite3_close_v2(_data->connection));
        _data->connection = nullptr;
    }
    release_lock(*_data);
}

auto Driver::name() const noexcept -> std::string_view
{
    return "sqlite";
}

auto Driver::open() -> VoidResult
{
    if (_data->connection) {
        return VoidResult::success();
    }

    auto normalized_path = normalize_database_path(_data->options);
    if (!normalized_path) {
        return VoidResult::failure(std::move(normalized_path).error());
    }
    _data->database_path = std::move(normalized_path).value();

    auto locked = acquire_lock(*_data);
    if (!locked) {
        return locked;
    }

    auto const database_path = _data->database_path.string();
    auto const flags  = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX | SQLITE_OPEN_PRIVATECACHE;
    auto const opened = sqlite3_open_v2(database_path.c_str(), &_data->connection, flags, nullptr);
    if (opened != SQLITE_OK) {
        auto error = detail::make_sqlite_error(_data->connection,
                                               opened,
                                               "db.sqlite.open_failed",
                                               jb::core::ErrorCategory::Io,
                                               "Unable to open the SQLite database file");
        cleanup_failed_open(*_data);
        return VoidResult::failure(std::move(error));
    }

    auto configured = configure_connection(*_data);
    if (!configured) {
        auto error = std::move(configured).error();
        cleanup_failed_open(*_data);
        return VoidResult::failure(std::move(error));
    }
    return VoidResult::success();
}

auto Driver::close() -> VoidResult
{
    if (!_data->connection) {
        release_lock(*_data);
        return VoidResult::success();
    }

    auto const closed = sqlite3_close(_data->connection);
    if (closed != SQLITE_OK) {
        return VoidResult::failure(detail::make_sqlite_error(_data->connection,
                                                             closed,
                                                             "db.sqlite.close_failed",
                                                             jb::core::ErrorCategory::Internal,
                                                             "Unable to close the SQLite database"));
    }

    _data->connection = nullptr;
    release_lock(*_data);
    return VoidResult::success();
}

auto Driver::is_open() const noexcept -> bool
{
    return _data->connection != nullptr;
}

auto Driver::create_query() -> jb::core::Result<std::unique_ptr<jb::db::DriverQuery>, jb::core::Error>
{
    if (!_data->connection) {
        return jb::core::Result<std::unique_ptr<jb::db::DriverQuery>, jb::core::Error>::failure({
            .category = jb::core::ErrorCategory::Unavailable,
            .code     = "db.database_closed",
            .message  = "The SQLite database is closed",
        });
    }
    return jb::core::Result<std::unique_ptr<jb::db::DriverQuery>, jb::core::Error>::success(
        std::make_unique<Query>(_data->connection));
}

auto Driver::begin(jb::db::TransactionMode mode) -> VoidResult
{
    switch (mode) {
        case jb::db::TransactionMode::Deferred:
            return execute_transaction(_data->connection, "BEGIN DEFERRED", "Unable to begin the SQLite transaction");
        case jb::db::TransactionMode::Immediate:
            return execute_transaction(_data->connection, "BEGIN IMMEDIATE", "Unable to begin the SQLite transaction");
        case jb::db::TransactionMode::Exclusive:
            return execute_transaction(_data->connection, "BEGIN EXCLUSIVE", "Unable to begin the SQLite transaction");
        default:
            return VoidResult::failure({
                .category = jb::core::ErrorCategory::Unsupported,
                .code     = "db.unsupported_transaction_mode",
                .message  = "The SQLite transaction mode is unsupported",
            });
    }
}

auto Driver::commit() -> VoidResult
{
    return execute_transaction(_data->connection, "COMMIT", "Unable to commit the SQLite transaction");
}

auto Driver::rollback() -> VoidResult
{
    return execute_transaction(_data->connection, "ROLLBACK", "Unable to roll back the SQLite transaction");
}

} // namespace jb::db::sqlite
