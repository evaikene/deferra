#include "run_session_priv.hpp"

#include "application.hpp"
#include "error.hpp"
#include "output_priv.hpp"

#include <utility>

namespace jb::jobuctl::detail {

auto run_session(Command command, jb::jobu::StandardAttributeRegistry const& registry, SessionMode mode)
    -> SessionResult
{
    auto const            json = command.json;
    jb::core::Application app{0, nullptr};
    Session               session{std::move(command), registry, mode};

    // Connect before start: even a synchronous failure must stop this Application's loop.
    session.finished.connect(&app, [&app](int code) { static_cast<void>(app.quit(code)); });
    session.start();
    static_cast<void>(app.exec());

    // Returning moves the owning outcome, then destroys Session before Application. No callback owns this scope.
    if (auto result = session.take_result()) {
        return std::move(*result);
    }

    // An event-loop failure can end exec() without a terminal Session callback.
    auto error = local_error({.category = jb::core::ErrorCategory::Internal,
                              .code     = "jobuctl.session.incomplete",
                              .message  = "Command event loop stopped before completion"});
    if (mode == SessionMode::Render) {
        print_error(json, error);
    }
    return {.exit_code = 3, .reply = std::nullopt, .error = std::move(error)};
}

} // namespace jb::jobuctl::detail
