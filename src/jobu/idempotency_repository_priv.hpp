#pragma once

#include "result.hpp"
#include "time_source.hpp"
#include "uuid.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace jb::db {
class Database;
}

namespace jb::jobu::detail {

struct IdempotencyRecord {
    std::string                           method;
    jb::core::Uuid                        scope_id;
    std::string                           key;
    std::string                           request_json;
    std::string                           result_json;
    jb::core::Uuid                        resource_id;
    jb::core::UtcTimePoint                created_at;
    std::optional<jb::core::UtcTimePoint> expires_at;
};

class IdempotencyRepository final {
public:
    explicit IdempotencyRepository(jb::db::Database& database) noexcept;

    [[nodiscard]] auto find(std::string_view method, jb::core::Uuid const& scope_id, std::string_view key)
        -> jb::core::Result<std::optional<IdempotencyRecord>, jb::core::Error>;
    [[nodiscard]] auto insert(IdempotencyRecord const& record) -> jb::core::Result<void, jb::core::Error>;
    [[nodiscard]] auto erase_for_resource(jb::core::Uuid const& resource_id)
        -> jb::core::Result<std::size_t, jb::core::Error>;
    [[nodiscard]] auto erase_expired(jb::core::UtcTimePoint cutoff, std::size_t limit)
        -> jb::core::Result<std::size_t, jb::core::Error>;

private:
    jb::db::Database& _database;
};

} // namespace jb::jobu::detail
