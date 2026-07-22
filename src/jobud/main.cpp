#include "jobu_version_priv.hpp"

#include "application.hpp"
#include "command_line_parser.hpp"
#include "local_server.hpp"
#include "logging.hpp"
#include "protocol.hpp"
#include "server.hpp"
#include "system_info.hpp"
#include "system_info_priv.hpp"

#include <fmt/format.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace {

auto parse_socket_path(int argc, char* argv[]) -> std::optional<std::filesystem::path>
{
    using namespace jb::core;

    constexpr std::array options{
        CommandLineOption{.long_name = "socket", .value_mode = CommandLineValueMode::Required},
    };
    CommandLineParser parser{argc, argv, options};
    if (parser.arguments().size() != 1U) {
        return std::nullopt;
    }

    auto const& socket = parser.arguments().front();
    if (socket.kind() != CommandLineArgumentKind::Option || !socket.known() || socket.missing_value() ||
        !socket.value() || socket.value()->empty()) {
        return std::nullopt;
    }
    return std::filesystem::path{std::string{*socket.value()}};
}

void print_usage()
{
    fmt::print(stderr, "Usage: jobud --socket <filesystem-path>\n");
}

} // anonymous namespace

auto main(int argc, char* argv[]) -> int
{
    using namespace jb::core;
    using namespace jb::jobu;
    using namespace jb::net;
    using namespace jb::rpc;

    if (argc == 2 && std::string_view{argv[1]} == "--version") {
        fmt::print(stdout, "jobud {}\n", jb::jobu::detail::project_version);
        return EXIT_SUCCESS;
    }

    auto const socket_path = parse_socket_path(argc, argv);
    if (!socket_path) {
        print_usage();
        return EXIT_FAILURE;
    }

    Application app{0, nullptr};
    LocalServer local_server;
    Server      rpc_server;

    auto const info = SystemInfo{
        .daemon_version = std::string{jb::jobu::detail::project_version},
        .api_version    = {.major = 1, .minor = 0},
        .capabilities   = {"system.info"},
    };
    if (!rpc_server.register_method(
            "system.info",
            [info](RequestContext const&, std::optional<JsonValue> const& params) -> MethodResult {
                return jb::jobu::detail::handle_system_info(info, params);
            })) {
        log_fatal("Unable to register the jobud system.info handler");
        return EXIT_FAILURE;
    }

    local_server.new_connection.connect([&local_server, &rpc_server]() -> void {
        while (auto socket = local_server.take_next_connection()) {
            auto const credentials    = socket->peer_credentials();
            auto       operation      = OperationContext{};
            operation.peer.process_id = credentials.process_id;
            operation.peer.user_id    = credentials.user_id;
            operation.peer.group_id   = credentials.group_id;

            auto added = rpc_server.add_connection(std::move(socket), std::move(operation));
            if (!added) {
                log_error("Unable to admit a local RPC connection: {} ({})", added.error().message, added.error().code);
            }
        }
    });
    local_server.accept_error.connect(
        [](IOError, std::string const& message) -> void { log_error("Local server accept error: {}", message); });
    rpc_server.connection_error.connect([](ConnectionId, Error const& error) -> void {
        log_error("RPC connection error: {} ({})", error.message, error.code);
    });

    if (!local_server.listen(*socket_path)) {
        log_error("Unable to listen on the local socket: {}", local_server.error_string());
        return EXIT_FAILURE;
    }

    return app.exec();
}
