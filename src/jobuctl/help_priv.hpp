#pragma once

#include "command_line_priv.hpp"

#include <string>

namespace jb::jobuctl::detail {

/// Renders local help from static command metadata without input or runtime effects.
/// The path must be a recognized root/group/leaf path returned by parsing.
auto render_help(HelpCommand const& command) -> std::string;

} // namespace jb::jobuctl::detail
