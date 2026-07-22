#pragma once

#include "byte_buffer.hpp"
#include "result.hpp"
#include "secret.hpp"
#include "uuid.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace jb::db {
class Database;
}

namespace jb::jobu::detail {

struct SecretReference {
    std::string secret_name;
    std::string field_path;
};

class SecretRepository final {
public:
    explicit SecretRepository(jb::db::Database& database) noexcept;

    [[nodiscard]] auto set(std::string_view name, jb::core::ByteView value, jb::core::UtcTimePoint updated_at)
        -> jb::core::Result<SecretMetadata, jb::core::Error>;
    [[nodiscard]] auto list_metadata(std::size_t limit, std::optional<std::string_view> after_name = std::nullopt)
        -> jb::core::Result<std::vector<SecretMetadata>, jb::core::Error>;
    [[nodiscard]] auto erase(std::string_view name) -> jb::core::Result<void, jb::core::Error>;
    [[nodiscard]] auto replace_references_for_job(jb::core::Uuid const&            job_id,
                                                  std::span<SecretReference const> references)
        -> jb::core::Result<std::size_t, jb::core::Error>;
    [[nodiscard]] auto erase_references_for_queue(jb::core::Uuid const& queue_id)
        -> jb::core::Result<std::size_t, jb::core::Error>;
    [[nodiscard]] auto reference_count(std::string_view name) -> jb::core::Result<std::uint64_t, jb::core::Error>;

private:
    jb::db::Database& _database;
};

} // namespace jb::jobu::detail
