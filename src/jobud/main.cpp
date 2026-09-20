#include "composition_priv.hpp"
#include "jobu_version_priv.hpp"
#include "runtime_priv.hpp"
#include "startup_priv.hpp"

#if defined(__linux__)
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
#include <functional>
#include <memory>
#include <string_view>
#include <utility>

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

    if (argc == 2 && std::string_view{argv[1]} == "--version") {
        fmt::print(stdout, "jobud {}\n", jb::jobu::detail::project_version);
        return EXIT_SUCCESS;
    }

    auto const startup = jb::jobud::detail::parse_startup_options(argc, argv);
    if (!startup) {
        print_usage();
        return EXIT_FAILURE;
    }

    // The relay precedes Application and every worker-capable dependency. Its checked retirement
    // follows their complete scope teardown, including every early startup return.
#if defined(__linux__)
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
            std::make_unique<jb::db::sqlite::Driver>(jb::db::sqlite::Options{.database_file = startup->database_path})};
        UuidV7Generator           uuid_generator{time_source};
        StandardAttributeRegistry attribute_registry;
        SystemCronEngine          cron;

        // Native macOS signal-relay adaptation remains Stage 7.18. The common runtime and recovery
        // ordering compile there without pretending that Linux signal handling supplies coverage.
        std::function<bool()> should_stop;
#if defined(__linux__)
        should_stop = [&relay] { return relay->requested(); };
#endif
        jb::jobud::detail::DaemonRuntime runtime{*app.event_loop(),
                                                 database,
                                                 attribute_registry,
                                                 cron,
                                                 uuid_generator,
                                                 time_source,
                                                 *startup,
                                                 std::move(should_stop)};
#if defined(__linux__)
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
                {.ca_bundle = startup->http_ca_bundle, .proxy = startup->http_proxy});
            if (!created) {
                return RunnersResult::failure(std::move(created).error());
            }
            auto runners    = jb::jobud::detail::RuntimeRunners{.http      = std::move(created).value(),
                                                                .executors = std::make_unique<AttemptExecutorGroup>()};
            auto registered = jb::jobud::detail::register_attempt_executors(*runners.executors,
                                                                            *runners.http,
                                                                            time_source,
                                                                            startup->allow_root_cli);
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
#if defined(__linux__)
    auto closed = relay->close();
    if (!closed) {
        log_error("JobU signal cleanup failed: code={}", closed.error().code);
        status = EXIT_FAILURE;
    }
#endif
    return status;
}
