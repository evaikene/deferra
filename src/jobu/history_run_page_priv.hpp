#pragma once

#include "error.hpp"
#include "history.hpp"
#include "history_cursor_priv.hpp"
#include "result.hpp"

#include <cstddef>
#include <optional>
#include <span>

namespace jb::jobu::detail {

/// Packs already-filtered rows in descending keyset order, including the repository's lookahead.
/// The query must be validated and match the previous cursor, when present. All inputs are borrowed;
/// the returned page and any new cursor own their data. Continuation starts after the last emitted row.
/// The byte budget includes the page-wrapper reserve; production uses 512 KiB, tests may supply less.
/// Reports jobu.response.too_large if the wrapper or first row cannot fit, without issuing a cursor.
[[nodiscard]] auto build_run_page(std::span<RunSummary const>          rows,
                                  RunQuery const&                      query,
                                  std::optional<RunCursorState> const& previous,
                                  HistoryCursorStore&                  cursors,
                                  std::size_t byte_budget) -> jb::core::Result<RunPage, jb::core::Error>;

} // namespace jb::jobu::detail
