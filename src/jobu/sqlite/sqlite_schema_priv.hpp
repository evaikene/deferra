#pragma once

#include "sqlite_schema.hpp"

#include <cstddef>
#include <span>
#include <string_view>

namespace jb::jobu::sqlite::detail {

enum class SchemaObjectKind : std::uint8_t {
    Table,
    Index,
};

struct SchemaObject {
    SchemaObjectKind kind;
    std::string_view name;
    std::string_view owner;
    std::string_view ddl;
    std::string_view column_probe;
};

using CreationStepObserver = auto (*)(std::size_t completed_statements, std::string_view object_name)
    -> jb::core::Result<void, jb::core::Error>;

[[nodiscard]] auto schema_object_manifest() noexcept -> std::span<SchemaObject const>;

[[nodiscard]] auto ensure_schema_impl(jb::db::Database& database, CreationStepObserver observer)
    -> jb::core::Result<SchemaStatus, jb::core::Error>;

} // namespace jb::jobu::sqlite::detail
