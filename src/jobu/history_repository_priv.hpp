#pragma once

#include "attempt_executor.hpp"
#include "history.hpp"
#include "result.hpp"

#include <cstddef>
#include <optional>
#include <vector>

namespace jb::db {
class Database;
}

namespace jb::jobu {
class AttributeRegistry;
}

namespace jb::jobu::detail {

struct RunCursorKey;

/// Reads retained history through projections suited to each public view.
/// The list queries never select attributes, payload, result, or output columns.
class HistoryRepository final {
public:
    HistoryRepository(jb::db::Database& database, AttributeRegistry const& attributes) noexcept;

    [[nodiscard]] auto get_run(jb::core::Uuid const& id)
        -> jb::core::Result<std::optional<RunDetails>, jb::core::Error>;
    [[nodiscard]] auto get_attempt(AttemptKey const& key)
        -> jb::core::Result<std::optional<AttemptDetails>, jb::core::Error>;

    /// Returns at most limit summary rows, including the caller's one-row lookahead.
    [[nodiscard]] auto list_runs(RunQuery const& query, std::optional<RunCursorKey> after, std::size_t limit)
        -> jb::core::Result<std::vector<RunSummary>, jb::core::Error>;
    /// Returns attempts below before_number, newest first, with one-row lookahead.
    [[nodiscard]] auto
    list_attempts(AttemptQuery const& query, std::optional<AttemptNumber> before_number, std::size_t limit)
        -> jb::core::Result<std::vector<AttemptSummary>, jb::core::Error>;

private:
    jb::db::Database&        _database;
    AttributeRegistry const& _attributes;
};

} // namespace jb::jobu::detail
