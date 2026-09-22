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

/// Current durable JobU SQLite schema version understood by this binary.
inline constexpr std::uint32_t current_schema_version{2};

/// Describes the schema accepted by ensure_schema().
///
/// The returned version is always current_schema_version. Creation initializes an empty database; upgrade preserves
/// existing version-1 data. Both flags are false when an existing current schema passes validation.
///
struct SchemaStatus {
    /// Validated durable schema version.
    std::uint32_t version{0};
    /// True when this call initialized an empty database.
    bool          created{false};
    /// True when this call committed the version-1-to-version-2 upgrade.
    bool          upgraded{false};
};

/// Creates, upgrades, or validates the complete JobU SQLite application schema.
///
/// @p database must be valid, open, idle, owned by the calling thread, and backed by the SQLite driver. The function
/// borrows it only for the duration of the call, begins one immediate transaction, and leaves no transaction active on
/// success or successful rollback. Failed rollback poisons the connection, which must be closed. A fresh schema is
/// created only in an unmarked database without user-defined schema objects. Valid version-1 databases receive an
/// atomic index-only upgrade; current databases are validated without repair. Any failure prevents startup, including
/// an uncertain commit outcome. Reopen and validate before attempting further use.
///
/// @param database Open SQLite database borrowed for the duration of the operation.
/// @return The current version and creation/upgrade flags, or a stable `jobu.schema.*` error. Upgrade DDL, marker,
/// validation, or commit failures use `jobu.schema.upgrade_failed` (Internal); invalid starting schemas retain
/// `jobu.schema.invalid` (Internal). Backend messages and SQL are not included in these errors.
///
[[nodiscard]] auto ensure_schema(jb::db::Database& database) -> jb::core::Result<SchemaStatus, jb::core::Error>;

} // namespace jb::jobu::sqlite
