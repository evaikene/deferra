#pragma once

#include "error.hpp"
#include "result.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace jb::jobuctl::detail {

/// Borrowed configuration for one synchronous resolution call; no ambient environment is read here.
struct EditorCommandInputs {
    std::optional<std::string_view> override_command;
    std::string_view                visual;
    std::string_view                editor;
};

/// Selects explicit override, nonempty VISUAL, nonempty EDITOR, then vi, and tokenizes with parse_command_argv().
/// An empty override or malformed selected value fails without falling back; diagnostics never echo command bytes.
/// Returns owning argv values without a draft path. Graphical editors must be configured to wait (e.g. code --wait).
[[nodiscard]] auto resolve_editor_command(EditorCommandInputs const& inputs)
    -> jb::core::Result<std::vector<std::string>, jb::core::Error>;

} // namespace jb::jobuctl::detail
