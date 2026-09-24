#include "jobu_version_priv.hpp"

#include "application.hpp"
#include "attribute_registry.hpp"
#include "command_line_priv.hpp"
#include "help_priv.hpp"
#include "input_priv.hpp"
#include "output_priv.hpp"
#include "session_priv.hpp"

#include <fmt/format.h>

#include <cstdio> // IWYU pragma: keep for stdout/stderr macros
#include <cstdlib>
#include <utility>
#include <variant>

auto main(int argc, char* argv[]) -> int
{
    using namespace jb::jobuctl::detail;

    // Local actions finish before constructing event-loop or socket infrastructure.
    jb::jobu::StandardAttributeRegistry registry;
    auto                                parsed = parse_command_line(argc, argv, registry);
    if (!parsed.action) {
        print_error(parsed.json_requested,
                    local_error({.category = jb::core::ErrorCategory::InvalidArgument,
                                 .code     = "jobuctl.syntax",
                                 .message  = parsed.error}));
        if (!parsed.json_requested) {
            fmt::print(stderr, "{}", render_help(parsed.usage));
        }
        return 2;
    }
    if (auto const* help = std::get_if<HelpCommand>(&*parsed.action)) {
        fmt::print(stdout, "{}", render_help(*help));
        return EXIT_SUCCESS;
    }
    if (std::holds_alternative<VersionCommand>(*parsed.action)) {
        fmt::print(stdout, "jobuctl {}\n", jb::jobu::detail::project_version);
        return EXIT_SUCCESS;
    }

    auto command = std::move(std::get<Command>(*parsed.action));
    auto loaded  = load_request_file(command, registry);
    if (!loaded) {
        print_error(command.json, local_error(loaded.error()));
        return 2;
    }
    auto secret_loaded = load_secret_input(command);
    if (!secret_loaded) {
        print_error(command.json, local_error(secret_loaded.error()));
        return 2;
    }

    jb::core::Application app{0, nullptr};
    Session               session{std::move(command), registry};
    session.finished.connect(&app, [&app](int code) { static_cast<void>(app.quit(code)); });
    session.start();
    return app.exec();
}
