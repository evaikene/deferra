#include "sqlite_error.hpp"

#include <string>

namespace jb::db::sqlite::detail {

namespace {

struct ErrorIdentity {
    jb::core::ErrorCategory category;
    std::string_view        code;
};

auto error_identity(int                     primary,
                    int                     extended,
                    std::string_view        fallback_code,
                    jb::core::ErrorCategory fallback_category) -> ErrorIdentity
{
    switch (extended) {
        case SQLITE_CONSTRAINT_UNIQUE:
        case SQLITE_CONSTRAINT_PRIMARYKEY:
            return {.category = jb::core::ErrorCategory::Conflict, .code = "db.constraint.unique"};
        case SQLITE_CONSTRAINT_FOREIGNKEY:
            return {.category = jb::core::ErrorCategory::Conflict, .code = "db.constraint.foreign_key"};
        default:
            break;
    }

    switch (primary) {
        case SQLITE_BUSY:
            return {.category = jb::core::ErrorCategory::Unavailable, .code = "db.busy"};
        case SQLITE_LOCKED:
            return {.category = jb::core::ErrorCategory::Unavailable, .code = "db.locked"};
        case SQLITE_CONSTRAINT:
            return {.category = jb::core::ErrorCategory::Conflict, .code = "db.constraint"};
        case SQLITE_CORRUPT:
        case SQLITE_NOTADB:
            return {.category = jb::core::ErrorCategory::Internal, .code = "db.corrupt"};
        case SQLITE_IOERR:
        case SQLITE_CANTOPEN:
            return {
                .category = jb::core::ErrorCategory::Io,
                .code     = fallback_code == "db.sqlite.open_failed" ? fallback_code : std::string_view{"db.io"},
            };
        case SQLITE_PERM:
        case SQLITE_AUTH:
        case SQLITE_READONLY:
            return {.category = jb::core::ErrorCategory::PermissionDenied, .code = "db.permission_denied"};
        case SQLITE_NOMEM:
            return {.category = jb::core::ErrorCategory::ResourceExhausted, .code = "db.out_of_memory"};
        case SQLITE_TOOBIG:
        case SQLITE_FULL:
            return {.category = jb::core::ErrorCategory::ResourceExhausted, .code = fallback_code};
        case SQLITE_INTERRUPT:
            return {.category = jb::core::ErrorCategory::Cancelled, .code = fallback_code};
        case SQLITE_ERROR:
        case SQLITE_MISMATCH:
        case SQLITE_MISUSE:
        case SQLITE_RANGE:
            return {.category = jb::core::ErrorCategory::InvalidArgument, .code = fallback_code};
        default:
            return {.category = fallback_category, .code = fallback_code};
    }
}

} // anonymous namespace

auto make_sqlite_error(sqlite3*                connection,
                       int                     result_code,
                       std::string_view        fallback_code,
                       jb::core::ErrorCategory fallback_category,
                       std::string_view        message) -> jb::core::Error
{
    auto extended = result_code;
    if (connection) {
        auto const current = sqlite3_extended_errcode(connection);
        if ((current & 0xff) == (result_code & 0xff)) {
            extended = current;
        }
    }
    auto const  primary        = extended & 0xff;
    auto const  identity       = error_identity(primary, extended, fallback_code, fallback_category);
    auto const* detail_message = connection ? sqlite3_errmsg(connection) : sqlite3_errstr(result_code);

    return {
        .category = identity.category,
        .code     = std::string{identity.code},
        .message  = std::string{message},
        .detail   = "primary=" + std::to_string(primary) + " extended=" + std::to_string(extended) +
                    " message=" + (detail_message ? detail_message : "unknown SQLite error"),
    };
}

} // namespace jb::db::sqlite::detail
