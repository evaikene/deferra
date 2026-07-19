/** @file record.hpp
 * @brief Defines owning database fields and forward-query records.
 */
#pragma once

#include "value.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace jb::db {

/** Stores one database field name and its backend-independent value.
 *
 * Query backends construct Fields while application code reads their immutable name and value.
 */
class Field {
public:
    /// Creates a field by taking ownership of its name and value.
    /// @param name Backend-reported field name.
    /// @param value Field value, including Null for SQL NULL.
    Field(std::string name, Value value);

    /// Returns the field name.
    /// @return A reference valid for the lifetime of this Field.
    [[nodiscard]] auto name() const noexcept -> std::string const&;

    /// Returns the field value without conversion.
    /// @return A reference valid for the lifetime of this Field.
    [[nodiscard]] auto value() const noexcept -> Value const&;

    /// Tests whether the field represents SQL NULL.
    /// @return True when the stored Value contains Null.
    [[nodiscard]] auto is_null() const noexcept -> bool;

private:
    std::string _name;
    Value       _value;
};

/** Owns the ordered fields for one query row or a query's field metadata.
 *
 * Use indexed access when column order is known or case-insensitive named access for stable column aliases. Records are
 * read-only values and do not generate SQL.
 */
class Record {
public:
    /// Creates an empty record.
    Record() = default;

    /// Creates a record by taking ownership of ordered fields.
    /// @param fields Fields in backend-reported column order.
    explicit Record(std::vector<Field> fields);

    /// Returns the number of fields.
    /// @return Number of ordered fields in this record.
    [[nodiscard]] auto count() const noexcept -> std::size_t;

    /// Tests whether the record has no fields.
    /// @return True when count() is zero.
    [[nodiscard]] auto is_empty() const noexcept -> bool;

    /// Tests whether a field name is present using ASCII case-insensitive matching.
    /// @param name Field name to find.
    /// @return True when at least one matching field exists.
    [[nodiscard]] auto contains(std::string_view name) const noexcept -> bool;

    /// Finds the first field using ASCII case-insensitive name matching.
    /// @param name Field name to find.
    /// @return Zero-based field index, or -1 when absent.
    [[nodiscard]] auto index_of(std::string_view name) const noexcept -> int;

    /// Returns a field by position.
    /// @param index Zero-based field index.
    /// @return The selected field.
    /// @throws std::out_of_range when index is outside the record.
    [[nodiscard]] auto field(std::size_t index) const -> Field const&;

    /// Returns a field name by position.
    /// @param index Zero-based field index.
    /// @return The selected field name.
    /// @throws std::out_of_range when index is outside the record.
    [[nodiscard]] auto field_name(std::size_t index) const -> std::string const&;

    /// Returns a field value by position without conversion.
    /// @param index Zero-based field index.
    /// @return The selected field value.
    /// @throws std::out_of_range when index is outside the record.
    [[nodiscard]] auto value(std::size_t index) const -> Value const&;

    /// Returns the first field value matching a name.
    /// @param name Field name matched with ASCII case-insensitive semantics.
    /// @return Pointer to the value, or nullptr when absent.
    [[nodiscard]] auto value(std::string_view name) const -> Value const*;

    /// Tests whether an indexed field is absent or SQL NULL.
    /// @param index Zero-based field index.
    /// @return True when index is out of range or its Value contains Null.
    [[nodiscard]] auto is_null(std::size_t index) const noexcept -> bool;

    /// Tests whether a named field is absent or SQL NULL.
    /// @param name Field name matched with ASCII case-insensitive semantics.
    /// @return True when the field is absent or its Value contains Null.
    [[nodiscard]] auto is_null(std::string_view name) const noexcept -> bool;

private:
    std::vector<Field> _fields;
};

} // namespace jb::db
