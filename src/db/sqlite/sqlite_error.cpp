#include "sqlite_error.hpp"

#include <string>
#include <utility>

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
            return {jb::core::ErrorCategory::Conflict, "db.constraint.unique"};
        case SQLITE_CONSTRAINT_FOREIGNKEY:
            return {jb::core::ErrorCategory::Conflict, "db.constraint.foreign_key"};
        default:
            break;
    }

    switch (primary) {
        case SQLITE_BUSY:
            return {jb::core::ErrorCategory::Unavailable, "db.busy"};
        case SQLITE_LOCKED:
            return {jb::core::ErrorCategory::Unavailable, "db.locked"};
        case SQLITE_CONSTRAINT:
            return {jb::core::ErrorCategory::Conflict, "db.constraint"};
        case SQLITE_CORRUPT:
        case SQLITE_NOTADB:
            return {jb::core::ErrorCategory::Internal, "db.corrupt"};
        case SQLITE_IOERR:
        case SQLITE_CANTOPEN:
            return {jb::core::ErrorCategory::Io,
                    fallback_code == "db.sqlite.open_failed" ? fallback_code : std::string_view{"db.io"}};
        case SQLITE_PERM:
        case SQLITE_AUTH:
        case SQLITE_READONLY:
            return {jb::core::ErrorCategory::PermissionDenied, "db.permission_denied"};
        case SQLITE_NOMEM:
            return {jb::core::ErrorCategory::ResourceExhausted, "db.out_of_memory"};
        case SQLITE_TOOBIG:
        case SQLITE_FULL:
            return {jb::core::ErrorCategory::ResourceExhausted, fallback_code};
        case SQLITE_INTERRUPT:
            return {jb::core::ErrorCategory::Cancelled, fallback_code};
        case SQLITE_ERROR:
        case SQLITE_MISMATCH:
        case SQLITE_MISUSE:
        case SQLITE_RANGE:
            return {jb::core::ErrorCategory::InvalidArgument, fallback_code};
        default:
            return {fallback_category, fallback_code};
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
