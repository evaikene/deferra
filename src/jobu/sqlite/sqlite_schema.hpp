///
/// @file sqlite/sqlite_schema.hpp
/// @brief Defines creation and validation of the SQLite-specific JobU application schema.
///
/// This is the only public application-schema entry point. It accepts the backend-neutral Database interface while
/// requiring the already-open connection to use the SQLite driver; generic JobU and database APIs remain independent
/// of SQLite schema details.
///
#pragma once

#include "database.hpp"
#include "error.hpp"
#include "result.hpp"

#include <cstdint>

namespace jb::jobu::sqlite {

/// Current durable JobU SQLite format marker; older formats are not upgraded.
inline constexpr std::uint32_t current_schema_version{3};

/// Describes the schema accepted by ensure_schema().
///
/// The returned version is always current_schema_version. Creation initializes an empty database; an existing current
/// schema passes validation without modification.
///
struct SchemaStatus {
    /// Validated durable schema version.
    std::uint32_t version{0};
    /// True when this call initialized an empty database.
    bool          created{false};
};

/// Creates or validates the complete current JobU SQLite application schema.
///
/// @p database must be valid, open, idle, owned by the calling thread, and backed by the SQLite driver. The function
/// borrows it only for the duration of the call, begins one immediate transaction, and leaves no transaction active on
/// success or successful rollback. Failed rollback poisons the connection, which must be closed. A fresh schema is
/// created only in an unmarked database without user-defined schema objects. Existing current-format databases are
/// validated without repair. Older and newer formats are rejected; an operator must use a fresh database for this
/// version rather than changing an existing marker. Any failure prevents startup, including an uncertain commit
/// outcome. Reopen and validate before attempting further use.
///
/// @param database Open SQLite database borrowed for the duration of the operation.
/// @return The current version and creation flag, or a stable `jobu.schema.*` error. Invalid connections return
/// `invalid_database`; unmarked nonempty databases return `database_not_empty`; older and newer formats return
/// `unsupported_version` and `newer_database`; malformed or incompatible current schemas return `invalid`.
/// Creation failures return `create_failed`. Backend messages and SQL are not included.
///
[[nodiscard]] auto ensure_schema(jb::db::Database& database) -> jb::core::Result<SchemaStatus, jb::core::Error>;

} // namespace jb::jobu::sqlite
