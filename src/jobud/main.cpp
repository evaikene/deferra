#include "composition_priv.hpp"
#include "jobu_version_priv.hpp"
#include "runtime_priv.hpp"
#include "startup_priv.hpp"

#if defined(__linux__) || defined(__APPLE__)
#  include "shutdown_signal_priv.hpp"
#endif

#include "application.hpp"
#include "attempt_executor_group.hpp"
#include "attribute_registry.hpp"
#include "cron.hpp"
#include "database.hpp"
#include "http/system_http_client.hpp"
#include "logging.hpp"
#include "sqlite/sqlite_driver.hpp"
#include "sqlite/sqlite_schema.hpp"
#include "time_source.hpp"
#include "uuid.hpp"

#include <fmt/format.h>

#include <cstdio> // IWYU pragma: keep for stderr and stdout
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <utility>

namespace {

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

    // The relay precedes Application and every worker-capable dependency. Its checked retirement
    // follows their complete scope teardown, including every early startup return.
#if defined(__linux__) || defined(__APPLE__)
    auto installed = jb::jobud::detail::ShutdownSignalRelay::install();
    if (!installed) {
        log_error("JobU signal setup failed: code={}", installed.error().code);
        return EXIT_FAILURE;
    }
    auto relay = std::move(installed).value();
#endif

    auto run_application = [&]() -> int {
        Application      app{0, nullptr};
        SystemTimeSource time_source;
        jb::db::Database database{
            std::make_unique<jb::db::sqlite::Driver>(jb::db::sqlite::Options{.database_file = startup.database_path})};
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
                                                 startup,
                                                 std::move(should_stop)};
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
        auto schema = jb::jobu::sqlite::ensure_schema(database);
        if (!schema) {
            runtime.fail("schema", schema.error());
            return runtime.exit_code();
        }

        auto make_runners = [&]() -> Result<jb::jobud::detail::RuntimeRunners, Error> {
            using RunnersResult = Result<jb::jobud::detail::RuntimeRunners, Error>;
            auto created        = jb::net::http::SystemHttpClient::create(
                *app.event_loop(),
                {.ca_bundle = startup.http_ca_bundle, .proxy = startup.http_proxy});
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
        log_error("JobU signal cleanup failed: code={}", closed.error().code);
        status = EXIT_FAILURE;
    }
#endif
    return status;
}
