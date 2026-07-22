#pragma once

#include "attribute.hpp"
#include "job.hpp"
#include "result.hpp"

#include <cstddef>
#include <optional>
#include <vector>

namespace jb::db {
class Database;
}

namespace jb::jobu::detail {

class JobRepository final {
public:
    JobRepository(jb::db::Database& database, AttributeRegistry const& attributes) noexcept;

    [[nodiscard]] auto insert(JobDefinition const& job) -> jb::core::Result<void, jb::core::Error>;
    [[nodiscard]] auto find_by_id(jb::core::Uuid const& id, bool include_deleted)
        -> jb::core::Result<std::optional<JobDefinition>, jb::core::Error>;
    [[nodiscard]] auto list(std::optional<jb::core::Uuid> queue_id,
                            bool                          include_deleted,
                            std::optional<JobState>       state,
                            std::optional<JobType>        type,
                            std::size_t                   limit,
                            std::optional<jb::core::Uuid> after_id)
        -> jb::core::Result<std::vector<JobDefinition>, jb::core::Error>;

private:
    jb::db::Database&        _database;
    AttributeRegistry const& _attributes;
};

} // namespace jb::jobu::detail
