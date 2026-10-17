#pragma once

#include "history.hpp"
#include "job.hpp"
#include "json.hpp"
#include "uuid.hpp"

#include <optional>
#include <span>
#include <string>

namespace jb::jobuctl::detail {

/// Owning fixed-field diagnostics; never retains execution content or arbitrary runner text.
struct AttemptDiagnostics {
    std::optional<int>         exit_code;
    std::optional<int>         signal_number;
    std::optional<unsigned>    http_status;
    std::optional<std::string> error_code;
    std::optional<std::string> error_category;
    std::optional<std::string> runner_outcome;
};

/// Projects only top-level fields for the trusted run type. Integers are exact and range-checked.
/// Category/outcome tokens must be known; error codes must be ASCII identifiers of at most 128 bytes.
/// Unknown, malformed, or overlong values are omitted in full. No result traversal or outcome inference occurs.
[[nodiscard]] auto project_attempt_diagnostics(jb::jobu::JobType type, jb::core::JsonValue const& result)
    -> AttemptDiagnostics;

/// Formats a compact Last attempt section without result JSON, suitable for summary-only partial results.
/// Returns no text if the public summary codec rejects the supplied identity, enums, or timestamps.
[[nodiscard]] auto format_last_attempt(jb::jobu::AttemptSummary const& attempt) -> std::optional<std::string>;

/// Adds bounded diagnostics for a completed attempt; the typed state/outcome remain authoritative.
/// Incomplete attempts never gain completion diagnostics from a possibly contradictory result object.
[[nodiscard]] auto format_last_attempt(jb::jobu::JobType type, jb::jobu::AttemptDetails const& attempt)
    -> std::optional<std::string>;

/// Produces a runner-appropriate command hint and explains latest-completed selection without reading output.
/// An unknown runner type has no hint.
[[nodiscard]] auto run_output_hint(jb::core::Uuid const& run_id, jb::jobu::JobType type) -> std::string;

/// Returns an owning copy of the first Completed summary in one newest-first, single-run page.
/// Empty/incomplete-only pages return no value. The caller owns page validation and cursor traversal.
[[nodiscard]] auto latest_completed_attempt(std::span<jb::jobu::AttemptSummary const> attempts)
    -> std::optional<jb::jobu::AttemptSummary>;

} // namespace jb::jobuctl::detail
