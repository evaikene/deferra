#pragma once

#include "result.hpp"
#include "uuid.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace jb::db {
class Database;
}

namespace jb::jobu::detail {

enum class RetentionOwnerKind : std::uint8_t {
    FinishedOnce,
    DeletedJob,
    DeletedQueue
};

/// Manual replay ownership is checked against this projection before deleting the run.
struct RetentionRunOwner {
    jb::core::Uuid job_id;
    bool           manual;
};

/// Payload-free cleanup queries. Every operation requires the caller's transaction
/// and finishes its query before returning; limits are 1..1000 candidate owners.
class RetentionOwnerRepository final {
public:
    explicit RetentionOwnerRepository(jb::db::Database& database) noexcept;

    [[nodiscard]] auto list_candidates(RetentionOwnerKind kind, std::size_t limit, std::optional<jb::core::Uuid> after)
        -> jb::core::Result<std::vector<jb::core::Uuid>, jb::core::Error>;
    [[nodiscard]] auto has_job(jb::core::Uuid const& id) -> jb::core::Result<bool, jb::core::Error>;
    [[nodiscard]] auto find_run(jb::core::Uuid const& id)
        -> jb::core::Result<std::optional<RetentionRunOwner>, jb::core::Error>;
    [[nodiscard]] auto has_queue(jb::core::Uuid const& id) -> jb::core::Result<bool, jb::core::Error>;
    [[nodiscard]] auto has_secret_references(jb::core::Uuid const& job) -> jb::core::Result<bool, jb::core::Error>;

    /// Rechecks soft deletion, domain dependents and both replay owner roles.
    /// Returns the checked affected count; secret references must be checked before retiring any keys.
    [[nodiscard]] auto delete_owner(RetentionOwnerKind kind, jb::core::Uuid const& id)
        -> jb::core::Result<std::size_t, jb::core::Error>;

private:
    jb::db::Database& _database;
};

} // namespace jb::jobu::detail
