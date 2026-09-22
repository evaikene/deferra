#include "jobu_version_priv.hpp"

#include "application.hpp"
#include "attribute_registry.hpp"
#include "command_line_priv.hpp"
#include "output_priv.hpp"
#include "session_priv.hpp"

#include <fmt/format.h>

#include <cstdio> // IWYU pragma: keep for stdout/stderr macros
#include <cstdlib>
#include <string_view>
#include <utility>

auto main(int argc, char* argv[]) -> int
{
    using namespace jb::jobuctl::detail;

    if (argc == 2 && std::string_view{argv[1]} == "--version") {
        fmt::print(stdout, "jobuctl {}\n", jb::jobu::detail::project_version);
        return EXIT_SUCCESS;
    }

    // Validate the command before constructing event-loop or socket infrastructure.
    jb::jobu::StandardAttributeRegistry registry;
    auto                                parsed = parse_command_line(argc, argv, registry);
    if (!parsed.command) {
        print_operator_error(parsed.error);
        print_usage();
        return EXIT_FAILURE;
    }

    jb::core::Application app{0, nullptr};
    Session               session{std::move(*parsed.command), registry};
    session.finished.connect(&app, [&app](int code) { static_cast<void>(app.quit(code)); });
    session.start();
    return app.exec();
}
