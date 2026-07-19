/** @file query.hpp
 * @brief Defines the forward-only generic prepared-query API used by application repositories.
 */
#pragma once

#include "database.hpp"
#include "record.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

namespace jb::db {

/** Owns generic prepared-query state while delegating native work to a DriverQuery.
 *
 * Construct a Query with a Database that will outlive it. Prepare SQL, bind every placeholder explicitly, execute, and
 * advance result rows with next(). The query is forward-only; finish() retains SQL and bindings for reuse, while
 * clear() discards all prepared state.
 */
class Query final {
public:
    /// Creates an unprepared query and immediately registers its lifetime with a database.
    /// @param database Database that must outlive this Query.
    explicit Query(Database& database);

    /// Discards backend query state and unregisters from the Database.
    ~Query();

    /// Prevents sharing a native prepared statement.
    /// @param other Query that would otherwise be copied.
    Query(Query const& other) = delete;

    /// Transfers query state while preserving its Database registration.
    /// @param other Query to transfer from.
    Query(Query&& other) noexcept;

    /// Prevents assigning native query ownership.
    /// @param other Query that would otherwise be copied.
    /// @return This Query, if assignment were supported.
    auto operator=(Query const& other) -> Query& = delete;

    /// Prevents replacing a query because that could hide cursor cleanup failures.
    /// @param other Query that would otherwise be moved.
    /// @return This Query, if assignment were supported.
    auto operator=(Query&& other) -> Query& = delete;

    /// Prepares one non-empty SQL statement and resets previous prepared state.
    /// @param sql SQL text containing backend-supported placeholders.
    /// @return Success or a generic/driver Error.
    [[nodiscard]] auto prepare(std::string_view sql) -> jb::core::Result<void, jb::core::Error>;

    /// Executes the prepared statement after verifying that every slot is bound.
    /// @return Success or a generic/driver Error.
    [[nodiscard]] auto exec() -> jb::core::Result<void, jb::core::Error>;

    /// Prepares and executes one SQL statement without separate binding calls.
    /// @param sql SQL text to prepare and execute.
    /// @return Success or a generic/driver Error.
    [[nodiscard]] auto exec(std::string_view sql) -> jb::core::Result<void, jb::core::Error>;

    /// Binds a value to a zero-based slot using positional binding mode.
    /// @param position Zero-based backend-reported bind slot.
    /// @param value Value to own and bind explicitly, including Null.
    /// @return Success or a generic/driver Error.
    auto bind_value(std::size_t position, Value value) -> jb::core::Result<void, jb::core::Error>;

    /// Binds a value using a complete backend-reported placeholder name.
    /// @param placeholder Placeholder including its prefix, such as `:queue_id`.
    /// @param value Value to own and bind explicitly, including Null.
    /// @return Success or a generic/driver Error.
    auto bind_value(std::string_view placeholder, Value value) -> jb::core::Result<void, jb::core::Error>;

    /// Binds a value to the next unbound slot using positional binding mode.
    /// @param value Value to own and bind explicitly, including Null.
    /// @return Success or a generic/driver Error.
    auto add_bind_value(Value value) -> jb::core::Result<void, jb::core::Error>;

    /// Advances an active result query by one row.
    /// @return True for a new current row, false at end-of-results, or a generic/driver Error.
    [[nodiscard]] auto next() -> jb::core::Result<bool, jb::core::Error>;

    /// Tests whether the last execution remains active.
    /// @return True after successful exec() and before finish(), clear(), or a new preparation.
    [[nodiscard]] auto is_active() const noexcept -> bool;

    /// Tests whether the query is positioned on a current result row.
    /// @return True only after next() returns true.
    [[nodiscard]] auto is_valid() const noexcept -> bool;

    /// Tests whether the active execution produces records.
    /// @return True for an active result query, otherwise false.
    [[nodiscard]] auto is_select() const noexcept -> bool;

    /// Returns the current row or result-field metadata.
    /// @return Current row when valid, otherwise null-valued metadata before the first or after the last row.
    /// @throws std::logic_error when this is not an active result query.
    [[nodiscard]] auto record() const -> Record const&;

    /// Returns an indexed value from the current row without conversion.
    /// @param index Zero-based field index.
    /// @return Current field value.
    /// @throws std::logic_error when no current row exists.
    /// @throws std::out_of_range when index is outside the current record.
    [[nodiscard]] auto value(std::size_t index) const -> Value const&;

    /// Returns a named value from the current row without conversion.
    /// @param name Field name matched using Record semantics.
    /// @return Pointer to the current value, or nullptr when no row or field exists.
    [[nodiscard]] auto value(std::string_view name) const -> Value const*;

    /// Tests whether an indexed current value is absent or SQL NULL.
    /// @param index Zero-based field index.
    /// @return True when there is no current row, index is absent, or the value contains Null.
    [[nodiscard]] auto is_null(std::size_t index) const noexcept -> bool;

    /// Tests whether a named current value is absent or SQL NULL.
    /// @param name Field name matched using Record semantics.
    /// @return True when there is no current row, field is absent, or the value contains Null.
    [[nodiscard]] auto is_null(std::string_view name) const noexcept -> bool;

    /// Returns the affected-row count from the active execution.
    /// @return Backend-reported count, or -1 when unavailable or inactive.
    [[nodiscard]] auto num_rows_affected() const noexcept -> std::int64_t;

    /// Returns the last successfully prepared SQL text with placeholders intact.
    /// @return Query-owned SQL, or an empty view when unprepared.
    [[nodiscard]] auto last_query() const noexcept -> std::string_view;

    /// Returns the last error produced by a Query operation.
    /// @return A copy of the last error, or no value when none is stored.
    [[nodiscard]] auto last_error() const -> std::optional<jb::core::Error>;

    /// Releases an active cursor while retaining prepared SQL and bindings.
    /// @return Success or a generic/driver Error. Calling finish() while inactive succeeds.
    auto finish() -> jb::core::Result<void, jb::core::Error>;

    /// Discards the backend query, SQL, bindings, execution state, and last error.
    void clear() noexcept;

private:
    struct Private;
    std::unique_ptr<Private> _data;
};

} // namespace jb::db
