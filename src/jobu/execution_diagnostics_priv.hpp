#pragma once

#include "job.hpp"
#include "json.hpp"

#include <optional>

namespace jb::jobu::detail {

/// Bounded numeric projection; deliberately cannot retain runner text or execution content.
struct ExecutionDiagnostics {
    std::optional<int>      exit_code;
    std::optional<int>      signal_number;
    std::optional<unsigned> http_status;
};

/// Reads only top-level CLI exit_code/signal or HTTP status from the effective completion result.
/// The caller must supply the cancellation-normalized result, not a contradictory original executor result.
/// Signed/unsigned integers are range-checked without coercion; malformed or unknown diagnostics are omitted.
/// No recursive traversal, result copy, outcome interpretation, storage access or runner dependency is involved.
[[nodiscard]] auto project_execution_diagnostics(JobType type, jb::core::JsonValue const& effective_result)
    -> ExecutionDiagnostics;

} // namespace jb::jobu::detail
