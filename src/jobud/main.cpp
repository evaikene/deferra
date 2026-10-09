#include "composition_priv.hpp"
#include "daemon_logger_priv.hpp"
#include "jobu_version_priv.hpp"
#include "runtime_priv.hpp"
#include "startup_priv.hpp"

#ifdef __linux__
#  include "endpoint_guard_priv.hpp"
#  include "privileges_priv.hpp"
#  include "runtime_paths_priv.hpp"
#endif

#if defined(__linux__) || defined(__APPLE__)
#  include "shutdown_signal_priv.hpp"
#endif

#include "application.hpp"
#include "attempt_executor_group.hpp"
#include "attribute_registry.hpp"
#include "cron.hpp"
#include "database.hpp"
#include "error.hpp"
#include "http/system_http_client.hpp"
#include "local_server.hpp"
#include "logging.hpp"
#include "sqlite/sqlite_driver.hpp"
#include "sqlite/sqlite_schema.hpp"
#include "time_source.hpp"
#include "uuid.hpp"

#include <fmt/format.h>

#include <array>
#include <cstdint>
#include <cstdio> // IWYU pragma: keep for stderr and stdout
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <sys/stat.h>
#include <utility>

namespace {

void log_startup_failure(std::string_view subsystem, jb::core::Error const& error)
{
    using namespace jb::core;
    auto const fields = std::array{
        LogField{.name = "subsystem", .value = subsystem                     },
        LogField{.name = "code",      .value = std::string_view{error.code}  },
        LogField{.name = "reason",    .value = std::string_view{error.detail}}
    };
    // Only the filesystem helper's two error families carry reviewed fixed reason tokens.
    auto const count = error.code == "jobud.path.unsafe" || error.code == "jobud.path.inspect_failed" ? 3U : 2U;
    log_event(LogLevel::Error, "jobud.failed", std::span{fields}.first(count));
}

void print_help(jb::jobud::detail::CompiledPaths const& paths)
{
    fmt::print(stdout,
               "Usage: jobud [--config PATH | --no-config] [--check-config] [options]\n"
               "       jobud --help | --version\n\n"
               "Config: {} (used if present; --config requires its file).\n"
               "Precedence: compiled defaults < config file < explicit flags.\n"
               "Default database: {}\nDefault socket: {}\n\n"
               "Options:\n"
               "  --config PATH            Require and load this config file.\n"
               "  --socket PATH             Override the socket path.\n"
               "  --database PATH           Override the SQLite database path.\n"
               "  --cli-concurrency N       Positive worker limit (default 4).\n"
               "  --http-concurrency N      Positive worker limit (default 16).\n"
               "  --http-proxy URL          HTTP or HTTPS proxy.\n"
               "  --http-ca-bundle PATH     CA bundle file.\n"
               "  --run-as-user NAME        Requested daemon account.\n"
               "  --run-as-group NAME       Requested group; requires a user.\n"
               "  --allow-root-daemon | --no-allow-root-daemon\n"
               "  --allow-root-cli | --no-allow-root-cli\n"
               "  --check-config            Validate locally without starting the daemon.\n"
               "  --no-config               Skip config loading.\n"
               "  --help, --version         Show local information.\n\n"
               "CLI relative paths use the invocation directory. Config paths must be absolute.\n"
               "Config intervals use seconds or s/m/h/d; byte quantities use bytes or k/m/g (1024 base).\n"
               "The two root switches are independent unsafe overrides.\n",
               paths.config.string(),
               paths.database.string(),
               paths.socket.string());
}

} // anonymous namespace

auto main(int argc, char* argv[]) -> int
{
    using namespace jb::core;
    using namespace jb::jobu;

    auto const paths  = jb::jobud::detail::compiled_paths();
    auto       parsed = jb::jobud::detail::parse_startup_arguments(argc, argv);
    if (!parsed) {
        fmt::print(stderr, "jobud: {} ({}); use --help\n", parsed.error().message, parsed.error().code);
        return 2;
    }
    auto const& arguments = parsed.value();
    if (arguments.action == jb::jobud::detail::StartupAction::Help) {
        print_help(paths);
        return EXIT_SUCCESS;
    }
    if (arguments.action == jb::jobud::detail::StartupAction::Version) {
        fmt::print(stdout, "jobud {}\n", jb::jobu::detail::project_version);
        return EXIT_SUCCESS;
    }

    std::error_code cwd_error;
    auto const      invocation_directory = std::filesystem::current_path(cwd_error);
    if (cwd_error) {
        fmt::print(stderr, "jobud: invocation directory is unavailable\n");
        return EXIT_FAILURE;
    }

    auto loaded = jb::jobud::detail::load_configuration(arguments, paths, invocation_directory);
    if (!loaded) {
        fmt::print(stderr, "jobud: {} ({})\n", loaded.error().message, loaded.error().code);
        return loaded.error().code == "jobud.config.read_failed" ? EXIT_FAILURE : 2;
    }
    auto resolved = jb::jobud::detail::resolve_startup_options(arguments, loaded->input, paths, invocation_directory);
    if (!resolved) {
        fmt::print(stderr, "jobud: {} ({})\n", resolved.error().message, resolved.error().code);
        return 2;
    }
    auto accounts = jb::jobud::detail::validate_readonly_accounts(*resolved);
    if (!accounts) {
        fmt::print(stderr, "jobud: {} ({})\n", accounts.error().message, accounts.error().code);
        return 2;
    }
    if (arguments.action == jb::jobud::detail::StartupAction::CheckConfig) {
        if (loaded->source_path) {
            fmt::print(stdout, "Configuration valid: {}\n", loaded->source_path->string());
        }
        else {
            fmt::print(stdout, "Configuration valid (compiled defaults)\n");
        }
        return EXIT_SUCCESS;
    }
    auto const startup = std::move(resolved).value();

    // The installed sink outlives every worker and the relay itself, including destructor fallback logs.
    jb::jobud::detail::DaemonLogScope logging{startup.logging_format, startup.logging_level};
    log_event(LogLevel::Info, "jobud.starting");

    // The relay precedes Application and every worker-capable dependency. Its checked retirement
    // follows their complete scope teardown, including every early startup return.
#if defined(__linux__) || defined(__APPLE__)
    auto installed = jb::jobud::detail::ShutdownSignalRelay::install();
    if (!installed) {
        log_startup_failure("signal_setup", installed.error());
        auto const stopped = std::array{
            LogField{.name = "exit_status", .value = std::int64_t{EXIT_FAILURE}}
        };
        log_event(LogLevel::Info, "jobud.stopped", stopped);
        return EXIT_FAILURE;
    }
    auto relay = std::move(installed).value();
#endif

    auto run_application = [&]() -> int {
#if defined(__linux__) || defined(__APPLE__)
        if (relay->requested()) {
            return EXIT_SUCCESS;
        }
#endif
#ifdef __linux__
        // Resolve authorization before any filesystem mutation. Only explicit directory leaves
        // may be prepared with privilege; all state files are opened after the verified permanent drop.
        auto operations = jb::jobud::detail::make_system_privilege_operations();
        auto identity   = jb::jobud::detail::resolve_final_identity(startup, *operations);
        if (!identity) {
            log_startup_failure("identity", identity.error());
            return EXIT_FAILURE;
        }
        if (relay->requested()) {
            return EXIT_SUCCESS;
        }
        ::umask(0077);
        auto prepared = jb::jobud::detail::prepare_runtime_paths(startup, *identity, *operations);
        if (!prepared) {
            log_startup_failure("paths", prepared.error());
            return EXIT_FAILURE;
        }
        if (relay->requested()) {
            return EXIT_SUCCESS;
        }
        auto applied = jb::jobud::detail::apply_final_identity(*identity, *operations);
        if (!applied) {
            log_startup_failure("identity", applied.error());
            return EXIT_FAILURE;
        }
        if (identity->user == 0) {
            log_event(LogLevel::Warning, "jobud.unsafe.root_daemon");
        }
        if (relay->requested()) {
            return EXIT_SUCCESS;
        }
        auto verified = jb::jobud::detail::verify_runtime_paths(*prepared, identity->user);
        if (verified) {
            verified = jb::jobud::detail::validate_database_artifacts(*prepared, identity->user);
        }
        if (!verified) {
            log_startup_failure("paths", verified.error());
            return EXIT_FAILURE;
        }
        auto effective_startup          = startup;
        effective_startup.database_path = prepared->database_path();
        effective_startup.socket_path   = prepared->socket_path();
        if (effective_startup.http_ca_bundle) {
            std::error_code error;
            auto            canonical = std::filesystem::canonical(*effective_startup.http_ca_bundle, error);
            if (error) {
                auto const fields = std::array{
                    LogField{.name = "subsystem", .value = std::string_view{"http_setup"}               },
                    LogField{.name = "code",      .value = std::string_view{"jobud.path.inspect_failed"}}
                };
                log_event(LogLevel::Error, "jobud.failed", fields);
                return EXIT_FAILURE;
            }
            effective_startup.http_ca_bundle = std::move(canonical);
        }
        if (relay->requested()) {
            return EXIT_SUCCESS;
        }
        // Own the endpoint before database creation. The guard outlives runtime and database,
        // including every partial-startup return; no lock pathname is removed during teardown.
        auto endpoint = jb::jobud::detail::EndpointGuard::acquire(prepared->runtime, identity->user);
        if (!endpoint) {
            log_startup_failure("endpoint", endpoint.error());
            return EXIT_FAILURE;
        }
        if (relay->requested()) {
            return EXIT_SUCCESS;
        }
#else
        auto const& effective_startup = startup;
#endif
        auto                                    listener_options = jb::net::LocalServerOptions{};
        jb::jobud::detail::EndpointGuard const* endpoint_guard{};
        listener_options.permissions = static_cast<std::filesystem::perms>(effective_startup.socket_mode);
#ifdef __linux__
        listener_options.group_id = prepared->runtime_group;
        endpoint_guard            = &*endpoint;
#endif
        Application               app{0, nullptr};
        SystemTimeSource          time_source;
        jb::db::Database          database{std::make_unique<jb::db::sqlite::Driver>(
            jb::db::sqlite::Options{.database_file = effective_startup.database_path})};
        UuidV7Generator           uuid_generator{time_source};
        StandardAttributeRegistry attribute_registry;
        SystemCronEngine          cron;

        std::function<bool()> should_stop;
#if defined(__linux__) || defined(__APPLE__)
        should_stop = [&relay] { return relay->requested(); };
#endif
        jb::jobud::detail::DaemonRuntime runtime{*app.event_loop(),
                                                 database,
                                                 attribute_registry,
                                                 cron,
                                                 uuid_generator,
                                                 time_source,
                                                 effective_startup,
                                                 std::move(should_stop),
                                                 listener_options,
                                                 endpoint_guard};
#if defined(__linux__) || defined(__APPLE__)
        auto attached = relay->attach(*app.event_loop(), [&runtime] { runtime.request_stop(); });
        if (!attached) {
            runtime.fail("signal_watch", attached.error());
            return runtime.exit_code();
        }
        // This watch dies before both its callback target and its EventLoop.
        auto watch = std::move(attached).value();
        if (relay->requested()) {
            runtime.request_stop();
            return runtime.exit_code();
        }
#endif
        auto opened = database.open();
        if (!opened) {
            runtime.fail("database_open", opened.error());
            return runtime.exit_code();
        }
#ifdef __linux__
        auto artifacts = jb::jobud::detail::validate_database_artifacts(*prepared, identity->user);
        if (!artifacts) {
            runtime.fail("database_paths", artifacts.error());
            return runtime.exit_code();
        }
        if (relay->requested()) {
            runtime.request_stop();
            return runtime.exit_code();
        }
        // Database ownership must succeed before a stale endpoint may be removed. Recovery and
        // runner construction are still dormant, so a collision cannot dispatch external work.
        auto admitted = endpoint->prepare_socket();
        if (!admitted) {
            runtime.fail("endpoint", admitted.error());
            return runtime.exit_code();
        }
#endif
        auto schema = jb::jobu::sqlite::ensure_schema(database);
        if (!schema) {
            runtime.fail("schema", schema.error());
            return runtime.exit_code();
        }
#ifdef __linux__
        artifacts = jb::jobud::detail::validate_database_artifacts(*prepared, identity->user);
        if (!artifacts) {
            runtime.fail("database_paths", artifacts.error());
            return runtime.exit_code();
        }
#endif

        auto make_runners = [&]() -> Result<jb::jobud::detail::RuntimeRunners, Error> {
            using RunnersResult = Result<jb::jobud::detail::RuntimeRunners, Error>;
            auto created        = jb::net::http::SystemHttpClient::create(
                *app.event_loop(),
                {.ca_bundle = effective_startup.http_ca_bundle, .proxy = effective_startup.http_proxy});
            if (!created) {
                return RunnersResult::failure(std::move(created).error());
            }
            auto runners    = jb::jobud::detail::RuntimeRunners{.http      = std::move(created).value(),
                                                                .executors = std::make_unique<AttemptExecutorGroup>()};
            auto registered = jb::jobud::detail::register_attempt_executors(*runners.executors,
                                                                            *runners.http,
                                                                            time_source,
                                                                            startup.allow_root_cli);
            if (!registered) {
                return RunnersResult::failure(std::move(registered).error());
            }
            return RunnersResult::success(std::move(runners));
        };
#ifdef __linux__
        auto endpoint_verified = endpoint->verify();
        if (!endpoint_verified) {
            runtime.fail("endpoint", endpoint_verified.error());
            return runtime.exit_code();
        }
#endif
        static_cast<void>(runtime.run(make_runners, [&app] { return app.exec(); }));

        // run() has destroyed service queries and transactions before releasing database ownership.
        auto closed = database.close();
        if (!closed) {
            runtime.fail("database_close", closed.error());
        }
        return runtime.exit_code();
    };

    auto status = run_application();
#if defined(__linux__) || defined(__APPLE__)
    auto closed = relay->close();
    if (!closed) {
        auto const fields = std::array{
            LogField{.name = "subsystem", .value = std::string_view{"signal_cleanup"}   },
            LogField{.name = "code",      .value = std::string_view{closed.error().code}}
        };
        log_event(LogLevel::Error, "jobud.signal.cleanup_failed", fields);
        status = EXIT_FAILURE;
    }
    // The relay destructor can retry cleanup and emit a safe diagnostic before the final record.
    relay.reset();
#endif
    auto const stopped = std::array{
        LogField{.name = "exit_status", .value = static_cast<std::int64_t>(status)}
    };
    log_event(LogLevel::Info, "jobud.stopped", stopped);
    return status;
}
