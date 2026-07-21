/**
 * @file sqlite/sqlite_schema.hpp
 * @brief Defines creation and validation of the SQLite-specific JobU application schema.
 *
 * This is the only public application-schema entry point. It accepts the backend-neutral Database interface while
 * requiring the already-open connection to use the SQLite driver; generic JobU and database APIs remain independent
 * of SQLite schema details.
 */
#pragma once

#include "database.hpp"
#include "error.hpp"
#include "result.hpp"

#include <cstdint>

namespace jb::jobu::sqlite {

/// Current durable JobU SQLite schema version understood by this binary.
inline constexpr std::uint32_t current_schema_version{1};

/** Describes the schema accepted by ensure_schema().
 *
 * The returned version is always current_schema_version. The creation flag distinguishes a newly initialized empty
 * database from a previously marked database that passed validation.
 */
struct SchemaStatus {
    /// Validated durable schema version.
    std::uint32_t version{0};
    /// True when this call created the version-1 schema.
    bool          created{false};
};

/** Creates or validates the complete JobU SQLite application schema.
 *
 * @p database must be valid, open, idle, owned by the calling thread, and backed by the SQLite driver. The function
 * borrows it only for the duration of the call, begins one immediate transaction, and leaves no transaction active on
 * success or represented failure. A fresh schema is created only in an unmarked database without user-defined schema
 * objects. Existing version-1 databases are validated without destructive repair.
 *
 * @param database Open SQLite database borrowed for the duration of the operation.
 * @return The current version and whether it was created, or an error with a stable `jobu.schema.*` code.
 */
[[nodiscard]] auto ensure_schema(jb::db::Database& database) -> jb::core::Result<SchemaStatus, jb::core::Error>;

} // namespace jb::jobu::sqlite
