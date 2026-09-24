#include "application.hpp"
#include "attribute_registry.hpp"
#include "client.hpp"
#include "control_client.hpp"
#include "local_socket.hpp"
#include "object.hpp"

#include <iostream>
#include <memory>
#include <string>
#include <variant>

// Read-only client flow:
//   Application/event loop -> LocalSocket/bytes -> rpc::Client/JSON-RPC -> ControlClient/JobU values.
//   Connect the socket, install all typed observers, initialize with system.info,
//   then list succeeded runs until the server returns no continuation cursor.
int main(int argc, char const* argv[])
{
    if (argc != 2) {
        std::cerr << "Usage: jobu-client-example SOCKET_PATH\n";
        return 2;
    }

    // Application establishes the owner event loop for every later Object. receiver gives callbacks that
    // capture this function's local state a lifetime context.
    jb::core::Application app{argc, argv};
    jb::core::Object      receiver;
    jb::net::LocalSocket  socket;

    // The registry supplies attribute definitions when typed queue, job, and run replies are decoded.
    jb::jobu::StandardAttributeRegistry      attributes;
    // The raw client borrows socket; the typed client borrows raw and attributes. Reverse declaration-order
    // destruction releases both wrappers before the objects they borrow.
    std::unique_ptr<jb::rpc::Client>         raw;
    std::unique_ptr<jb::jobu::ControlClient> typed;

    auto finish = [&](int code) { static_cast<void>(app.quit(code)); };

    // Install receiver-aware socket handlers before connect_to_server(), which may notify synchronously.
    socket.error_occurred.connect(&receiver, [&](jb::core::IOError, std::string const& message) {
        std::cerr << "Socket error: " << message << '\n';
        finish(1);
    });

    socket.connected.connect(&receiver, [&] {
        // Layer JSON-RPC framing over the connected socket, then typed JobU calls over the raw RPC client.
        raw   = std::make_unique<jb::rpc::Client>(socket);
        typed = std::make_unique<jb::jobu::ControlClient>(*raw, attributes);

        // initialize() requests system.info. ready means its reply passed the API-major check and capabilities
        // are cached, so typed calls may now be submitted.
        typed->ready.connect(&receiver, [&](jb::jobu::SystemInfo const& info) {
            std::cout << "Daemon " << info.daemon_version << " (API " << info.api_version.major << '.'
                      << info.api_version.minor << ")\n";

            // The first page specifies filters and limit; later pages use only the returned cursor.
            auto query          = jb::jobu::RunQuery{};
            query.filters.state = jb::jobu::RunState::Succeeded;
            query.limit         = 20;
            if (auto call = typed->list_runs(query); !call) {
                std::cerr << "Cannot list runs: " << call.error().message << '\n';
                finish(1);
            }
        });

        // The call ID identifies an operation when multiple calls are outstanding. This example issues only
        // run.list, so each successful ControlReply must hold a RunPage.
        typed->reply_received.connect(&receiver, [&](jb::jobu::ControlCallId, jb::jobu::ControlReply const& reply) {
            auto const* page = std::get_if<jb::jobu::RunPage>(&reply);
            if (page == nullptr) {
                std::cerr << "Unexpected reply type\n";
                finish(1);
                return;
            }

            for (auto const& run : page->items) {
                std::cout << run.id.to_string() << '\n';
            }
            if (page->next_cursor) {
                // The server's cursor retains the original query; sending filters alongside it is invalid.
                if (auto call = typed->list_runs(jb::jobu::CursorRequest{*page->next_cursor}); !call) {
                    std::cerr << "Cannot continue run list: " << call.error().message << '\n';
                    finish(1);
                }
            }
            else {
                finish(0);
            }
        });

        // A represented remote error differs from a local timeout or protocol failure. This example only reads;
        // mutation callers must also inspect ControlFailure::outcome_unknown before considering a retry.
        typed->call_failed.connect(&receiver, [&](jb::jobu::ControlCallId, jb::jobu::ControlFailure const& failure) {
            if (failure.kind == jb::jobu::ControlFailureKind::Remote) {
                std::cerr << "Remote application error: " << std::get<jb::rpc::RpcError>(failure.error).message << '\n';
            }
            else {
                auto const& error = std::get<jb::core::Error>(failure.error);
                if (error.code == "jobu.client.timeout") {
                    std::cerr << "Local timeout: " << error.message << '\n';
                }
                else {
                    std::cerr << "Local call error: " << error.message << '\n';
                }
            }
            finish(1);
        });

        // failed is the handshake/terminal-client path, separate from one call's failure.
        typed->failed.connect(&receiver, [&](jb::core::Error const& error) {
            std::cerr << "Client stopped: " << error.message << '\n';
            finish(1);
        });

        // Install every observer before the first request: synchronous raw responses are deferred by ControlClient.
        if (auto started = typed->initialize(); !started) {
            std::cerr << "Cannot start handshake: " << started.error().message << '\n';
            finish(1);
        }
    });

    // The connection may complete here or on a later event-loop turn; exec() drives subsequent I/O and deadlines.
    socket.connect_to_server(argv[1]);
    return app.exec();
}
