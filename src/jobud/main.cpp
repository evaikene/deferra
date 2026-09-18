#include "composition_priv.hpp"
#include "jobu_version_priv.hpp"
#include "startup_priv.hpp"

#include "application.hpp"
#include "attempt_executor_group.hpp"
#include "attribute_registry.hpp"
#include "cron.hpp"
#include "database.hpp"
#include "http/system_http_client.hpp"
#include "local_server.hpp"
#include "logging.hpp"
#include "management.hpp"
#include "management_rpc.hpp"
#include "protocol.hpp"
#include "scheduler.hpp"
#include "server.hpp"
#include "sqlite/sqlite_driver.hpp"
#include "sqlite/sqlite_schema.hpp"
#include "system_info.hpp"
#include "system_info_rpc.hpp"
#include "time_source.hpp"
#include "uuid.hpp"

#include <fmt/format.h>

#include <cstdio> // IWYU pragma: keep for stderr and stdout
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

void print_usage()
{
    fmt::print(stderr,
               "Usage: jobud --socket <filesystem-path> --database <sqlite-file> "
               "[--cli-concurrency <positive-integer>] [--allow-root-cli] "
               "[--http-concurrency <positive-integer>] [--http-proxy <http-or-https-url>] "
               "[--http-ca-bundle <filesystem-path>]\n");
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

    auto const startup = jb::jobud::detail::parse_startup_options(argc, argv);
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

    auto created_http_client = jb::net::http::SystemHttpClient::create(*app.event_loop(),
                                                                       {
                                                                           .ca_bundle = startup->http_ca_bundle,
                                                                           .proxy     = startup->http_proxy,
                                                                       });
    if (!created_http_client) {
        log_error("Unable to initialize the JobU HTTP client: {} ({})",
                  created_http_client.error().message,
                  created_http_client.error().code);
        return EXIT_FAILURE;
    }

    // Reverse destruction stops Scheduler first, then destroys group-owned runners while the HTTP client is alive.
    // This scope cleanup does not install Phase 7's daemon signal handling or rewrite durable running attempts.
    auto                 http_client = std::move(created_http_client).value();
    AttemptExecutorGroup executors;
    auto                 registered =
        jb::jobud::detail::register_attempt_executors(executors, *http_client, time_source, startup->allow_root_cli);
    if (!registered) {
        log_error("Unable to register the JobU attempt executors: {} ({})",
                  registered.error().message,
                  registered.error().code);
        return EXIT_FAILURE;
    }

    Scheduler         scheduler{database,
                                attribute_registry,
                                cron,
                                uuid_generator,
                                time_source,
                                executors,
                                jb::jobud::detail::scheduler_options(*startup)};
    ManagementService management_service{database, attribute_registry, cron, uuid_generator, time_source};
    LocalServer       local_server;
    Server            rpc_server;

    http_client->failed.connect(
        [](Error const& error) -> void { log_error("System HTTP client failed: {} ({})", error.message, error.code); });
    scheduler.failed.connect(
        [](Error const& error) -> void { log_error("Scheduler failed: {} ({})", error.message, error.code); });

    auto capabilities = std::vector<std::string>{std::string{system_info_rpc_method_name()}};
    capabilities.reserve(capabilities.size() + management_rpc_method_names().size());
    for (auto const method : management_rpc_method_names()) {
        capabilities.emplace_back(method);
    }

    auto info = SystemInfo{
        .daemon_version = std::string{jb::jobu::detail::project_version},
        .api_version    = {.major = 1, .minor = 2},
        .capabilities   = std::move(capabilities),
    };
    if (!register_system_info_method(rpc_server, std::move(info))) {
        log_fatal("Unable to register the jobud system.info handler");
        return EXIT_FAILURE;
    }
    // Receiver tracking deactivates this Object-capturing slot if the scheduler is destroyed before the service.
    management_service.mutation_committed.connect(&scheduler, [&scheduler]() -> void { scheduler.request_rescan(); });
    if (!register_management_methods(rpc_server, management_service, attribute_registry)) {
        log_fatal("Unable to register the jobud management handlers");
        return EXIT_FAILURE;
    }

    // rpc_server is destroyed before local_server, so receiver tracking deactivates its admission slot first.
    local_server.new_connection.connect(&rpc_server, [&local_server, &rpc_server]() -> void {
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

    auto started = scheduler.start();
    if (!started) {
        log_error("Unable to start the JobU scheduler: {} ({})", started.error().message, started.error().code);
        return EXIT_FAILURE;
    }

    return app.exec();
}
