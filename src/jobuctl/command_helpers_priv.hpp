#pragma once

#include "command_line_parser.hpp"
#include "command_line_priv.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace jb::jobuctl::detail {

auto parse_failure(std::string message) -> ParseResult;
auto option_value(jb::core::CommandLineArgument const& argument) -> std::optional<std::string_view>;
auto parse_unsigned(std::string_view text, std::uint64_t minimum, std::uint64_t maximum)
    -> std::optional<std::uint64_t>;

} // namespace jb::jobuctl::detail
