#pragma once

#include "attribute.hpp"
#include "queue.hpp"
#include "result.hpp"

#include <cstddef>
#include <optional>
#include <string_view>
#include <vector>

namespace jb::db {
class Database;
}

namespace jb::jobu::detail {

class QueueRepository final {
public:
    QueueRepository(jb::db::Database& database, AttributeRegistry const& attributes) noexcept;

    [[nodiscard]] auto insert(Queue const& queue, std::string_view internal_name)
        -> jb::core::Result<void, jb::core::Error>;
    [[nodiscard]] auto find_by_id(jb::core::Uuid const& id, bool include_deleted)
        -> jb::core::Result<std::optional<Queue>, jb::core::Error>;
    [[nodiscard]] auto find_by_name(std::string_view name, bool include_deleted)
        -> jb::core::Result<std::optional<Queue>, jb::core::Error>;
    [[nodiscard]] auto list(bool                          include_deleted,
                            std::optional<QueueState>     state,
                            std::size_t                   limit,
                            std::optional<jb::core::Uuid> after_id)
        -> jb::core::Result<std::vector<Queue>, jb::core::Error>;
    [[nodiscard]] auto replace_mutable_fields(Queue const& queue) -> jb::core::Result<bool, jb::core::Error>;

private:
    jb::db::Database&        _database;
    AttributeRegistry const& _attributes;
};

} // namespace jb::jobu::detail
