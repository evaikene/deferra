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

    /// Counts selected runs and their required timing rows, then attempts, without multiplying run samples.
    /// Returns coverage and a Complete-terminal wait duration even when its sample population is empty.
    [[nodiscard]] auto aggregate(StatisticsRequest const& request, StatisticsGroupKey const& key)
        -> jb::core::Result<StatisticsGroup, jb::core::Error>;

private:
    jb::db::Database& _database;
};

} // namespace jb::jobu::detail
