#pragma once

#include "attempt.hpp"
#include "job.hpp"
#include "run.hpp"

#include <optional>
#include <string_view>

namespace jb::jobu::detail {

/// Shared wire spellings for history requests/results and later statistics/control codecs.
[[nodiscard]] auto history_wire_text(JobType value) noexcept -> std::optional<std::string_view>;
[[nodiscard]] auto history_wire_text(RunOrigin value) noexcept -> std::optional<std::string_view>;
[[nodiscard]] auto history_wire_text(RunState value) noexcept -> std::optional<std::string_view>;
[[nodiscard]] auto history_wire_text(AttemptState value) noexcept -> std::optional<std::string_view>;
[[nodiscard]] auto history_wire_text(AttemptOutcome value) noexcept -> std::optional<std::string_view>;

[[nodiscard]] auto parse_history_wire_text(std::string_view text, JobType& value) noexcept -> bool;
[[nodiscard]] auto parse_history_wire_text(std::string_view text, RunOrigin& value) noexcept -> bool;
[[nodiscard]] auto parse_history_wire_text(std::string_view text, RunState& value) noexcept -> bool;
[[nodiscard]] auto parse_history_wire_text(std::string_view text, AttemptState& value) noexcept -> bool;
[[nodiscard]] auto parse_history_wire_text(std::string_view text, AttemptOutcome& value) noexcept -> bool;

} // namespace jb::jobu::detail
