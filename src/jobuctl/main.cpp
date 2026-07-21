#include "jobu_version_priv.hpp"

#include "application.hpp"
#include "client.hpp"
#include "command_line_parser.hpp"
#include "local_socket.hpp"
#include "logging.hpp"
#include "protocol.hpp"
#include "system_info.hpp"
#include "timer.hpp"

#include <fmt/format.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace {

auto parse_system_info_socket(int argc, char* argv[]) -> std::optional<std::filesystem::path>
{
    using namespace jb::core;

    constexpr std::array options{
        CommandLineOption{.long_name = "socket", .value_mode = CommandLineValueMode::Required},
    };
    CommandLineParser parser{argc, argv, options};
    if (parser.arguments().size() != 3U) {
        return std::nullopt;
    }

    auto const& socket  = parser.arguments()[0];
    auto const& command = parser.arguments()[1];
    auto const& action  = parser.arguments()[2];
    if (socket.kind() != CommandLineArgumentKind::Option || !socket.known() || socket.missing_value() ||
        !socket.value() || socket.value()->empty() || command.kind() != CommandLineArgumentKind::Positional ||
        command.token() != "system" || action.kind() != CommandLineArgumentKind::Positional ||
        action.token() != "info") {
        return std::nullopt;
    }
    return std::filesystem::path{std::string{*socket.value()}};
}

void print_usage()
{
    fmt::print(stderr, "Usage: jobuctl --socket <filesystem-path> system info\n");
}

void print_system_info(jb::jobu::SystemInfo const& info)
{
    fmt::print(stdout, "Daemon version: {}\n", info.daemon_version);
    fmt::print(stdout, "API version: {}.{}\n", info.api_version.major, info.api_version.minor);
    fmt::print(stdout, "Capabilities:\n");
    for (auto const& capability : info.capabilities) {
        fmt::print(stdout, "  {}\n", capability);
    }
}

} // anonymous namespace

auto main(int argc, char* argv[]) -> int
{
    using namespace jb::core;
    using namespace jb::jobu;
    using namespace jb::net;
    using namespace jb::rpc;
    using namespace std::chrono_literals;

    if (argc == 2 && std::string_view{argv[1]} == "--version") {
        fmt::print(stdout, "jobuctl {}\n", jb::jobu::detail::project_version);
        return EXIT_SUCCESS;
    }

    auto const socket_path = parse_system_info_socket(argc, argv);
    if (!socket_path) {
        print_usage();
        return EXIT_FAILURE;
    }

    Application             app{0, nullptr};
    LocalSocket             socket;
    std::unique_ptr<Client> client;
    Timer                   timeout;
    auto                    finished = false;

    auto finish = [&](int code) -> void {
        if (finished) {
            return;
        }
        finished = true;
        timeout.stop();
        static_cast<void>(app.quit(code));
    };
    auto finish_operator_error = [&finish, &finished](std::string_view message) -> void {
        if (finished) {
            return;
        }
        fmt::print(stderr, "jobuctl: {}\n", message);
        finish(EXIT_FAILURE);
    };

    socket.set_read_buffer_limit(2U * 1024U * 1024U);
    socket.error_occurred.connect(
        [&finish_operator_error](IOError, std::string message) -> void { finish_operator_error(message); });
    socket.connected.connect([&]() -> void {
        client = std::make_unique<Client>(socket);
        client->result_received.connect([&](RequestId, JsonValue value) -> void {
            auto info = system_info_from_json(value);
            if (!info) {
                log_error("Invalid system.info response: {} ({})", info.error().message, info.error().code);
                finish(EXIT_FAILURE);
                return;
            }
            print_system_info(info.value());
            finish(EXIT_SUCCESS);
        });
        client->error_received.connect([&finish, &finished](RequestId, RpcError error) -> void {
            if (finished) {
                return;
            }
            fmt::print(stderr, "jobuctl: remote RPC error {}: {}\n", error.code, error.message);
            finish(EXIT_FAILURE);
        });
        client->request_failed.connect(
            [&finish_operator_error](RequestId, Error error) -> void { finish_operator_error(error.message); });
        client->protocol_error.connect([&finish, &finished](Error error) -> void {
            if (finished) {
                return;
            }
            log_error("RPC protocol error: {} ({})", error.message, error.code);
            finish(EXIT_FAILURE);
        });

        auto call = client->call("system.info");
        if (!call) {
            log_error("Unable to send the system.info request: {} ({})", call.error().message, call.error().code);
            finish(EXIT_FAILURE);
        }
    });
    timeout.timeout.connect(
        [&finish_operator_error]() -> void { finish_operator_error("system.info request timed out"); });

    timeout.start(5s);
    socket.connect_to_server(*socket_path);
    return app.exec();
}
