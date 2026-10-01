#pragma once

#include "result.hpp"
#include "time_source.hpp"
#include "uuid.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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

/// Owning primary-key continuation for an indexed reference page.
struct IdempotencyKey {
    std::string    method;
    jb::core::Uuid scope_id;
    std::string    key;
};

class IdempotencyRepository final {
public:
    explicit IdempotencyRepository(jb::db::Database& database) noexcept;

    [[nodiscard]] auto find(std::string_view method, jb::core::Uuid const& scope_id, std::string_view key)
        -> jb::core::Result<std::optional<IdempotencyRecord>, jb::core::Error>;
    [[nodiscard]] auto insert(IdempotencyRecord const& record) -> jb::core::Result<void, jb::core::Error>;
    /// Reads at most limit (1..1000) records referring to either owner role.
    /// All queries finish before returning; the caller owns the cleanup transaction.
    [[nodiscard]] auto list_referencing(jb::core::Uuid const&                owner,
                                        std::size_t                          limit,
                                        std::optional<IdempotencyKey> const& after = std::nullopt)
        -> jb::core::Result<std::vector<IdempotencyRecord>, jb::core::Error>;
    /// Deletes only the selected primary key with its checked method/scope/resource relationship.
    /// The caller must validate its documents and require exactly one affected row.
    [[nodiscard]] auto erase_matching(IdempotencyRecord const& record)
        -> jb::core::Result<std::size_t, jb::core::Error>;

private:
    jb::db::Database& _database;
};

} // namespace jb::jobu::detail
