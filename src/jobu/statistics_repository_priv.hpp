#pragma once

#include "result.hpp"
#include "statistics.hpp"

#include <optional>
#include <vector>

namespace jb::db {
class Database;
}

namespace jb::jobu::detail {

/// Scalar-only retained-history projections. No payload, result, or output BLOB is selected.
class StatisticsRepository final {
public:
    explicit StatisticsRepository(jb::db::Database& database) noexcept;

    /// Returns at most limit distinct keys after the boundary, including one-row lookahead.
    [[nodiscard]] auto
    list_groups(StatisticsRequest const& request, std::optional<StatisticsGroupKey> after, std::size_t limit)
        -> jb::core::Result<std::vector<StatisticsGroupKey>, jb::core::Error>;

    /// Counts the selected runs, then their attempts, without multiplying runs through joins.
    [[nodiscard]] auto aggregate(StatisticsRequest const& request, StatisticsGroupKey const& key)
        -> jb::core::Result<StatisticsGroup, jb::core::Error>;

private:
    jb::db::Database& _database;
};

} // namespace jb::jobu::detail
