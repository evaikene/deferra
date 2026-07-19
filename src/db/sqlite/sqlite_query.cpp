#include "sqlite_query_priv.hpp"

#include "sqlite_error.hpp"

#include <cstring>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace jb::db::sqlite {

namespace {

template <typename T>
auto failure(jb::core::Error error) -> jb::core::Result<T, jb::core::Error>
{
    return jb::core::Result<T, jb::core::Error>::failure(std::move(error));
}

auto query_error(std::string_view code, std::string_view message) -> jb::core::Error
{
    return {
        .category = jb::core::ErrorCategory::InvalidArgument,
        .code     = std::string{code},
        .message  = std::string{message},
    };
}

} // anonymous namespace

Query::Query(sqlite3* connection) noexcept
    : _connection{connection}
{}

Query::~Query()
{
    clear();
}

auto Query::prepare(std::string_view sql) -> jb::core::Result<void, jb::core::Error>
{
    clear();
    if (sql.empty() || sql.find('\0') != std::string_view::npos ||
        sql.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return failure<void>(query_error("db.prepare_failed", "The SQLite statement text is invalid or too large"));
    }

    char const* tail{nullptr};
    auto const  prepared = sqlite3_prepare_v3(_connection,
                                              sql.data(),
                                              static_cast<int>(sql.size()),
                                              SQLITE_PREPARE_PERSISTENT,
                                              &_statement,
                                              &tail);
    if (prepared != SQLITE_OK) {
        auto error = detail::make_sqlite_error(_connection,
                                               prepared,
                                               "db.prepare_failed",
                                               jb::core::ErrorCategory::InvalidArgument,
                                               "Unable to prepare the SQLite statement");
        clear();
        return failure<void>(std::move(error));
    }
    if (!_statement) {
        return failure<void>(query_error("db.prepare_failed", "The SQL text does not contain a SQLite statement"));
    }

    auto const* end = sql.data() + sql.size();
    while (tail && tail < end) {
        sqlite3_stmt* extra_statement{nullptr};
        char const*   next_tail{nullptr};
        auto const    extra =
            sqlite3_prepare_v3(_connection, tail, static_cast<int>(end - tail), 0, &extra_statement, &next_tail);
        if (extra != SQLITE_OK) {
            auto error = detail::make_sqlite_error(_connection,
                                                   extra,
                                                   "db.prepare_failed",
                                                   jb::core::ErrorCategory::InvalidArgument,
                                                   "Unable to prepare the trailing SQLite statement text");
            if (extra_statement) {
                static_cast<void>(sqlite3_finalize(extra_statement));
            }
            clear();
            return failure<void>(std::move(error));
        }
        if (extra_statement) {
            static_cast<void>(sqlite3_finalize(extra_statement));
            clear();
            return failure<void>(
                query_error("db.prepare_failed", "Only one SQLite statement may be prepared at a time"));
        }
        if (!next_tail || next_tail <= tail) {
            break;
        }
        tail = next_tail;
    }

    auto const count = sqlite3_bind_parameter_count(_statement);
    _parameter_names.reserve(static_cast<std::size_t>(count));
    for (int index = 1; index <= count; ++index) {
        auto const* name = sqlite3_bind_parameter_name(_statement, index);
        _parameter_names.emplace_back(name ? name : "");
    }
    return jb::core::Result<void, jb::core::Error>::success();
}

auto Query::parameter_count() const noexcept -> std::size_t
{
    return _parameter_names.size();
}

auto Query::parameter_name(std::size_t index) const -> std::string_view
{
    return _parameter_names.at(index);
}

auto Query::bind(std::size_t index, Value const& value) -> jb::core::Result<void, jb::core::Error>
{
    if (!_statement || index >= _parameter_names.size()) {
        return failure<void>(query_error("db.invalid_parameter", "The SQLite bind position is out of range"));
    }

    auto const sqlite_index = static_cast<int>(index + 1);
    auto const result       = std::visit(
        [this, sqlite_index](auto const& stored) {
            using Stored = std::decay_t<decltype(stored)>;
            if constexpr (std::is_same_v<Stored, Null>) {
                return sqlite3_bind_null(_statement, sqlite_index);
            }
            else if constexpr (std::is_same_v<Stored, std::int64_t>) {
                return sqlite3_bind_int64(_statement, sqlite_index, stored);
            }
            else if constexpr (std::is_same_v<Stored, double>) {
                return sqlite3_bind_double(_statement, sqlite_index, stored);
            }
            else if constexpr (std::is_same_v<Stored, std::string>) {
                auto const* bytes = stored.empty() ? "" : stored.data();
                return sqlite3_bind_text64(_statement,
                                           sqlite_index,
                                           bytes,
                                           static_cast<sqlite3_uint64>(stored.size()),
                                           SQLITE_TRANSIENT,
                                           SQLITE_UTF8);
            }
            else {
                static constexpr std::byte empty_blob{0};
                auto const*                bytes =
                    stored.empty() ? static_cast<void const*>(&empty_blob) : static_cast<void const*>(stored.data());
                return sqlite3_bind_blob64(_statement,
                                           sqlite_index,
                                           bytes,
                                           static_cast<sqlite3_uint64>(stored.size()),
                                           SQLITE_TRANSIENT);
            }
        },
        value);
    if (result != SQLITE_OK) {
        return failure<void>(detail::make_sqlite_error(_connection,
                                                       result,
                                                       "db.bind_failed",
                                                       jb::core::ErrorCategory::InvalidArgument,
                                                       "Unable to bind the SQLite parameter"));
    }
    return jb::core::Result<void, jb::core::Error>::success();
}

auto Query::exec() -> jb::core::Result<ExecutionInfo, jb::core::Error>
{
    if (!_statement) {
        return failure<ExecutionInfo>(query_error("db.invalid_query", "The SQLite query is not prepared"));
    }

    _produces_records = sqlite3_column_count(_statement) != 0;
    if (_produces_records) {
        auto record_metadata = metadata();
        if (!record_metadata) {
            return failure<ExecutionInfo>(std::move(record_metadata).error());
        }
        return jb::core::Result<ExecutionInfo, jb::core::Error>::success({
            .produces_records = true,
            .rows_affected    = -1,
            .record_metadata  = std::move(record_metadata).value(),
        });
    }

    auto const stepped = sqlite3_step(_statement);
    if (stepped != SQLITE_DONE) {
        auto error = detail::make_sqlite_error(_connection,
                                               stepped,
                                               "db.exec_failed",
                                               jb::core::ErrorCategory::Internal,
                                               "Unable to execute the SQLite statement");
        static_cast<void>(sqlite3_reset(_statement));
        return failure<ExecutionInfo>(std::move(error));
    }
    return jb::core::Result<ExecutionInfo, jb::core::Error>::success({
        .produces_records = false,
        .rows_affected    = sqlite3_changes64(_connection),
        .record_metadata  = {},
    });
}

auto Query::next() -> jb::core::Result<std::optional<Record>, jb::core::Error>
{
    if (!_statement || !_produces_records) {
        return failure<std::optional<Record>>(query_error("db.invalid_query", "The SQLite query has no record cursor"));
    }

    auto const stepped = sqlite3_step(_statement);
    if (stepped == SQLITE_DONE) {
        return jb::core::Result<std::optional<Record>, jb::core::Error>::success(std::nullopt);
    }
    if (stepped != SQLITE_ROW) {
        return failure<std::optional<Record>>(detail::make_sqlite_error(_connection,
                                                                        stepped,
                                                                        "db.fetch_failed",
                                                                        jb::core::ErrorCategory::Internal,
                                                                        "Unable to fetch the next SQLite record"));
    }

    auto record = current_record();
    if (!record) {
        return failure<std::optional<Record>>(std::move(record).error());
    }
    return jb::core::Result<std::optional<Record>, jb::core::Error>::success(
        std::optional<Record>{std::move(record).value()});
}

auto Query::finish() -> jb::core::Result<void, jb::core::Error>
{
    if (!_statement) {
        return jb::core::Result<void, jb::core::Error>::success();
    }

    auto const reset  = sqlite3_reset(_statement);
    _produces_records = false;
    if (reset != SQLITE_OK) {
        return failure<void>(detail::make_sqlite_error(_connection,
                                                       reset,
                                                       "db.exec_failed",
                                                       jb::core::ErrorCategory::Internal,
                                                       "Unable to finish the SQLite statement"));
    }
    return jb::core::Result<void, jb::core::Error>::success();
}

void Query::clear() noexcept
{
    if (_statement) {
        static_cast<void>(sqlite3_finalize(_statement));
        _statement = nullptr;
    }
    _parameter_names.clear();
    _produces_records = false;
}

auto Query::metadata() const -> jb::core::Result<Record, jb::core::Error>
{
    auto const         count = sqlite3_column_count(_statement);
    std::vector<Field> fields;
    fields.reserve(static_cast<std::size_t>(count));
    for (int index = 0; index < count; ++index) {
        auto const* name = sqlite3_column_name(_statement, index);
        if (!name) {
            return failure<Record>(detail::make_sqlite_error(_connection,
                                                             SQLITE_NOMEM,
                                                             "db.exec_failed",
                                                             jb::core::ErrorCategory::ResourceExhausted,
                                                             "Unable to read SQLite result metadata"));
        }
        fields.emplace_back(name, Null{});
    }
    return jb::core::Result<Record, jb::core::Error>::success(Record{std::move(fields)});
}

auto Query::current_record() const -> jb::core::Result<Record, jb::core::Error>
{
    auto const         count = sqlite3_column_count(_statement);
    std::vector<Field> fields;
    fields.reserve(static_cast<std::size_t>(count));
    for (int index = 0; index < count; ++index) {
        auto const* name = sqlite3_column_name(_statement, index);
        if (!name) {
            return failure<Record>(detail::make_sqlite_error(_connection,
                                                             SQLITE_NOMEM,
                                                             "db.fetch_failed",
                                                             jb::core::ErrorCategory::ResourceExhausted,
                                                             "Unable to read the SQLite field name"));
        }

        Value value;
        switch (sqlite3_column_type(_statement, index)) {
            case SQLITE_NULL:
                value = Null{};
                break;
            case SQLITE_INTEGER:
                value = static_cast<std::int64_t>(sqlite3_column_int64(_statement, index));
                break;
            case SQLITE_FLOAT:
                value = sqlite3_column_double(_statement, index);
                break;
            case SQLITE_TEXT: {
                auto const* bytes = sqlite3_column_text(_statement, index);
                auto const  size  = sqlite3_column_bytes(_statement, index);
                if (!bytes && size != 0) {
                    return failure<Record>(detail::make_sqlite_error(_connection,
                                                                     SQLITE_NOMEM,
                                                                     "db.fetch_failed",
                                                                     jb::core::ErrorCategory::ResourceExhausted,
                                                                     "Unable to read the SQLite text value"));
                }
                auto const* text = bytes ? reinterpret_cast<char const*>(bytes) : "";
                value            = std::string{text, static_cast<std::size_t>(size)};
                break;
            }
            case SQLITE_BLOB: {
                auto const* bytes = sqlite3_column_blob(_statement, index);
                auto const  size  = sqlite3_column_bytes(_statement, index);
                if (!bytes && size != 0) {
                    return failure<Record>(detail::make_sqlite_error(_connection,
                                                                     SQLITE_NOMEM,
                                                                     "db.fetch_failed",
                                                                     jb::core::ErrorCategory::ResourceExhausted,
                                                                     "Unable to read the SQLite blob value"));
                }
                jb::core::ByteBuffer blob;
                blob.resize(static_cast<std::size_t>(size));
                if (size != 0) {
                    std::memcpy(blob.data(), bytes, static_cast<std::size_t>(size));
                }
                value = std::move(blob);
                break;
            }
            default:
                return failure<Record>(query_error("db.fetch_failed", "SQLite returned an unsupported field type"));
        }
        fields.emplace_back(name, std::move(value));
    }
    return jb::core::Result<Record, jb::core::Error>::success(Record{std::move(fields)});
}

} // namespace jb::db::sqlite
