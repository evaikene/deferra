#pragma once

#include "idempotency_repository_priv.hpp"
#include "result.hpp"
#include "run_repository_priv.hpp"

#include <cstddef>

namespace jb::db {
class Database;
}

namespace jb::jobu::detail {

struct RetentionPurgeCounts {
    std::size_t runs{0};
    std::size_t idempotency_records{0};
    std::size_t jobs{0};
    std::size_t queues{0};
};

class RetentionRepository final {
public:
    RetentionRepository(jb::db::Database& database, AttributeRegistry const& attributes) noexcept;

    [[nodiscard]] auto purge_batch(jb::core::UtcTimePoint cutoff, std::size_t limit)
        -> jb::core::Result<RetentionPurgeCounts, jb::core::Error>;

private:
    jb::db::Database&     _database;
    RunRepository         _runs;
    IdempotencyRepository _idempotency;
};

} // namespace jb::jobu::detail
