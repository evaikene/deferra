#pragma once

#include "command_line_parser.hpp"
#include "command_line_priv.hpp"
#include "history.hpp"
#include "job.hpp"
#include "json.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace jb::jobuctl::detail {

auto parse_failure(std::string message) -> CommandBuildResult;
auto option_value(jb::core::CommandLineArgument const& argument) -> std::optional<std::string_view>;
auto parse_unsigned(std::string_view text, std::uint64_t minimum, std::uint64_t maximum)
    -> std::optional<std::uint64_t>;

/// Recognizes explicit-attempt and composed-run commands that deliver one attempt-output chunk.
auto is_output_command(CommandKind kind) -> bool;

/// Checks channel compatibility with a trusted run type, without inspecting output data or availability.
auto output_channel_matches(jb::jobu::JobType type, jb::jobu::OutputChannel channel) -> bool;

/// Appends channel/offset/limit fields from registry-validated output options to request parameters.
/// Returns the existing usage diagnostic on failure; partially appended parameters must then be discarded.
/// Callers supply channel validation and defaults through their public codec or CLI-only request builder.
auto append_output_options(jb::core::JsonValue::Object&                   parameters,
                           std::span<jb::core::CommandLineArgument const> arguments,
                           std::string_view                               command_name) -> std::optional<std::string>;

} // namespace jb::jobuctl::detail
