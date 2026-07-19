/** @file driver_query.hpp
 * @brief Defines the backend query interface consumed by the generic Query API.
 */
#pragma once

#include "error.hpp"
#include "record.hpp"
#include "result.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace jb::db {

class Query;

/** Describes the cursor and mutation outcome produced by query execution.
 *
 * DriverQuery implementations return this owning metadata so generic Query state never references backend buffers.
 */
struct ExecutionInfo {
    /// Whether execution produced a record cursor.
    bool         produces_records{false};
    /// Number of changed rows, or -1 when the backend cannot provide it.
    std::int64_t rows_affected{-1};
    /// Field names and null values describing result columns before the first row.
    Record       record_metadata;
};

/** Performs backend-specific prepared-query operations for generic Query.
 *
 * Backend authors derive from DriverQuery and expose instances only through Driver::create_query(). Application code
 * uses the higher-level Query API rather than calling this interface directly.
 */
class DriverQuery {
public:
    /// Destroys a backend query and its native statement through the interface.
    virtual ~DriverQuery() = default;

    /// Prevents sharing native statement ownership.
    /// @param other DriverQuery that would otherwise be copied.
    DriverQuery(DriverQuery const& other) = delete;

    /// Prevents moving a native statement independently of its generic Query.
    /// @param other DriverQuery that would otherwise be moved.
    DriverQuery(DriverQuery&& other) = delete;

    /// Prevents assigning native statement ownership.
    /// @param other DriverQuery that would otherwise be copied.
    /// @return This DriverQuery, if assignment were supported.
    auto operator=(DriverQuery const& other) -> DriverQuery& = delete;

    /// Prevents move-assigning native statements.
    /// @param other DriverQuery that would otherwise be moved.
    /// @return This DriverQuery, if assignment were supported.
    auto operator=(DriverQuery&& other) -> DriverQuery& = delete;

protected:
    /// Constructs the base portion of a concrete backend query.
    DriverQuery() = default;

private:
    friend class Query;

    /// Prepares exactly one backend statement.
    /// @param sql SQL text containing backend-supported placeholders.
    /// @return Success or a stable project-owned Error.
    [[nodiscard]] virtual auto prepare(std::string_view sql) -> jb::core::Result<void, jb::core::Error> = 0;

    /// Returns the number of distinct bind slots in the prepared statement.
    /// @return Number of bind slots reported by the backend.
    [[nodiscard]] virtual auto parameter_count() const noexcept -> std::size_t = 0;

    /// Returns normalized metadata for one bind slot.
    /// @param index Zero-based bind-slot index.
    /// @return Complete named placeholder or an empty view for an anonymous positional slot.
    [[nodiscard]] virtual auto parameter_name(std::size_t index) const -> std::string_view = 0;

    /// Binds one backend-independent value to a prepared slot.
    /// @param index Zero-based bind-slot index.
    /// @param value Value whose bytes remain owned by generic Query.
    /// @return Success or a stable project-owned Error.
    [[nodiscard]] virtual auto bind(std::size_t index, Value const& value)
        -> jb::core::Result<void, jb::core::Error> = 0;

    /// Executes the prepared statement.
    /// @return Owning execution metadata or a stable project-owned Error.
    [[nodiscard]] virtual auto exec() -> jb::core::Result<ExecutionInfo, jb::core::Error> = 0;

    /// Advances a forward-only cursor.
    /// @return The next owning Record, no record at end-of-results, or a stable project-owned Error.
    [[nodiscard]] virtual auto next() -> jb::core::Result<std::optional<Record>, jb::core::Error> = 0;

    /// Releases active backend cursor resources while retaining the prepared statement.
    /// @return Success or a stable project-owned Error.
    [[nodiscard]] virtual auto finish() -> jb::core::Result<void, jb::core::Error> = 0;

    /// Discards all native prepared-query state without reporting cleanup errors.
    virtual void clear() noexcept = 0;
};

} // namespace jb::db
