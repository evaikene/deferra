#include "jobu_version_priv.hpp"

#include "application.hpp"
#include "attribute_registry.hpp"
#include "command_line_parser.hpp"
#include "cron.hpp"
#include "database.hpp"
#include "local_server.hpp"
#include "logging.hpp"
#include "management.hpp"
#include "management_rpc.hpp"
#include "protocol.hpp"
#include "server.hpp"
#include "sqlite/sqlite_driver.hpp"
#include "sqlite/sqlite_schema.hpp"
#include "system_info.hpp"
#include "system_info_rpc.hpp"
#include "time_source.hpp"
#include "uuid.hpp"

#include <fmt/format.h>

#include <array>
#include <cstdio> // IWYU pragma: keep for stderr and stdout
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

struct StartupOptions {
    std::filesystem::path socket_path;
    std::filesystem::path database_path;
};

auto parse_startup_options(int argc, char* argv[]) -> std::optional<StartupOptions>
{
    using namespace jb::core;

    constexpr std::array options{
        CommandLineOption{.long_name = "socket",   .value_mode = CommandLineValueMode::Required},
        CommandLineOption{.long_name = "database", .value_mode = CommandLineValueMode::Required},
    };
    CommandLineParser parser{argc, argv, options};
    if (parser.arguments().size() != options.size()) {
        return std::nullopt;
    }

    auto socket_path   = std::optional<std::filesystem::path>{};
    auto database_path = std::optional<std::filesystem::path>{};
    for (auto const& argument : parser) {
        if (argument.kind() != CommandLineArgumentKind::Option || !argument.known() || argument.missing_value() ||
            !argument.value() || argument.value()->empty()) {
            return std::nullopt;
        }

        auto path = std::filesystem::path{std::string{*argument.value()}};
        if (argument.name() == "socket" && !socket_path) {
            socket_path = std::move(path);
        }
        else if (argument.name() == "database" && !database_path) {
            database_path = std::move(path);
        }
        else {
            return std::nullopt;
        }
    }

    if (!socket_path || !database_path) {
        return std::nullopt;
    }
    return StartupOptions{.socket_path = std::move(*socket_path), .database_path = std::move(*database_path)};
}

void print_usage()
{
    fmt::print(stderr, "Usage: jobud --socket <filesystem-path> --database <sqlite-file>\n");
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

    auto const startup = parse_startup_options(argc, argv);
    if (!startup) {
        print_usage();
        return EXIT_FAILURE;
    }

    Application      app{0, nullptr};
    SystemTimeSource time_source;
    jb::db::Database database{
        std::make_unique<jb::db::sqlite::Driver>(jb::db::sqlite::Options{.database_file = startup->database_path})};

    auto opened = database.open();
    if (!opened) {
        log_error("Unable to open the JobU database: {} ({})", opened.error().message, opened.error().code);
        return EXIT_FAILURE;
    }

    auto schema = jb::jobu::sqlite::ensure_schema(database);
    if (!schema) {
        log_error("Unable to prepare the JobU database schema: {} ({})", schema.error().message, schema.error().code);
        return EXIT_FAILURE;
    }

    UuidV7Generator           uuid_generator{time_source};
    StandardAttributeRegistry attribute_registry;
    SystemCronEngine          cron;
    ManagementService         management_service{database, attribute_registry, cron, uuid_generator, time_source};
    LocalServer               local_server;
    Server                    rpc_server;

    auto capabilities = std::vector<std::string>{std::string{system_info_rpc_method_name()}};
    capabilities.reserve(capabilities.size() + management_rpc_method_names().size());
    for (auto const method : management_rpc_method_names()) {
        capabilities.emplace_back(method);
    }

    auto info = SystemInfo{
        .daemon_version = std::string{jb::jobu::detail::project_version},
        .api_version    = {.major = 1, .minor = 1},
        .capabilities   = std::move(capabilities),
    };
    if (!register_system_info_method(rpc_server, std::move(info))) {
        log_fatal("Unable to register the jobud system.info handler");
        return EXIT_FAILURE;
    }
    if (!register_management_methods(rpc_server, management_service, attribute_registry)) {
        log_fatal("Unable to register the jobud management handlers");
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

    if (!local_server.listen(startup->socket_path)) {
        log_error("Unable to listen on the local socket: {}", local_server.error_string());
        return EXIT_FAILURE;
    }

    return app.exec();
}
