#include "editor_command_priv.hpp"

#include "command_argv_priv.hpp"

#include <fmt/format.h>

#include <utility>

namespace jb::jobuctl::detail {

auto resolve_editor_command(EditorCommandInputs const& inputs)
    -> jb::core::Result<std::vector<std::string>, jb::core::Error>
{
    auto command = std::string_view{"vi"};
    auto source  = std::string_view{"default"};
    if (inputs.override_command) {
        command = *inputs.override_command;
        source  = "explicit";
    }
    else if (!inputs.visual.empty()) {
        command = inputs.visual;
        source  = "VISUAL";
    }
    else if (!inputs.editor.empty()) {
        command = inputs.editor;
        source  = "EDITOR";
    }

    auto arguments = parse_command_argv(command);
    if (!arguments) {
        auto error    = std::move(arguments).error();
        error.code    = "jobuctl.editor.invalid_command";
        error.message = fmt::format("Invalid {} editor command: {}", source, error.message);
        return jb::core::Result<std::vector<std::string>, jb::core::Error>::failure(std::move(error));
    }
    return arguments;
}

} // namespace jb::jobuctl::detail
