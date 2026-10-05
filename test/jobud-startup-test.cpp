#include "composition_priv.hpp"
#include "startup_priv.hpp"

#include "application.hpp"
#include "attempt_executor_group.hpp"
#include "attribute_registry.hpp"
#include "cli/cli_attempt_executor.hpp"
#include "cli/process_adapter_priv.hpp"
#include "cron.hpp"
#include "database.hpp"
#include "http/http_attempt_executor.hpp"
#include "json.hpp"
#include "logging.hpp"
#include "management.hpp"
#include "run_repository_priv.hpp"
#include "sqlite/sqlite_driver.hpp"
#include "sqlite/sqlite_schema.hpp"
#include "support/catch_utils.hpp" // IWYU pragma: keep for Catch::StringMaker specializations
#include "support/fake_http_client.hpp"
#include "support/rejecting_secret_provider.hpp"
#include "support/temporary_directory.hpp"
#include "time_source.hpp"
#include "uuid.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <utility>
#include <vector>

using namespace jb::core;
using namespace jb::jobu;
using namespace jb::jobud::detail;

namespace {

auto parse_arguments(std::vector<std::string> extra) -> Result<StartupArguments, StartupError>
{
    auto arguments = std::vector<std::string>{"jobud"};
    arguments.insert(arguments.end(), extra.begin(), extra.end());
    auto argv = std::vector<char const*>{};
    for (auto const& argument : arguments) {
        argv.push_back(argument.c_str());
    }
    return parse_startup_arguments(static_cast<int>(argv.size()), argv.data());
}

auto parse(std::vector<std::string> extra = {}) -> std::optional<StartupOptions>
{
    auto arguments =
        std::vector<std::string>{"jobud", "--no-config", "--socket", "daemon.sock", "--database", "daemon.sqlite"};
    arguments.insert(arguments.end(), extra.begin(), extra.end());
    auto argv = std::vector<char const*>{};
    for (auto const& argument : arguments) {
        argv.push_back(argument.c_str());
    }
    auto parsed = parse_startup_arguments(static_cast<int>(argv.size()), argv.data());
    if (!parsed) {
        return std::nullopt;
    }
    auto resolved = resolve_startup_options(*parsed, {}, compiled_paths(), "/invocation");
    if (!resolved) {
        return std::nullopt;
    }
    return std::move(resolved).value();
}

struct IdentityState {
    std::uint64_t uid{1000};
    std::size_t   destructions{0};
};

class ObservedIdentity final : public cli::detail::EffectiveIdentityProbe {
public:
    explicit ObservedIdentity(IdentityState& state)
        : _state{state}
    {}

    ~ObservedIdentity() override { ++_state.destructions; }

    auto effective_user_id() const noexcept -> std::uint64_t override { return _state.uid; }

private:
    IdentityState& _state;
};

class WarningLogger final : public Logger {
public:
    void log(LogMessage const& message) override
    {
        auto lock = std::scoped_lock{_mutex};
        if (message.level == LogLevel::Warning) {
            _warnings.emplace_back(message.message);
        }
    }

    auto warnings() -> std::vector<std::string>
    {
        auto lock = std::scoped_lock{_mutex};
        return _warnings;
    }

private:
    std::mutex               _mutex;
    std::vector<std::string> _warnings;
};

struct CaptureWarnings {
    CaptureWarnings() { set_logger(capture); }

    ~CaptureWarnings() { set_logger(previous); }

    std::shared_ptr<Logger>        previous{logger()};
    std::shared_ptr<WarningLogger> capture{std::make_shared<WarningLogger>()};
};

auto http_request(TimeSource& time) -> AttemptStartRequest
{
    UuidV7Generator           generator{time};
    StandardAttributeRegistry registry;
    auto                      attributes = materialize_attributes(registry, {}, {}, {});
    REQUIRE(attributes);
    auto run_id   = generator.generate();
    auto job_id   = generator.generate();
    auto queue_id = generator.generate();
    REQUIRE(run_id);
    REQUIRE(job_id);
    REQUIRE(queue_id);
    return {
        .key        = {.run_id = *run_id, .attempt_number = 1},
        .job_id     = *job_id,
        .queue_id   = *queue_id,
        .type       = JobType::Http,
        .attributes = std::move(*attributes),
        .payload =
            JsonValue{.data = JsonValue::Object{{"url", JsonValue{.data = std::string{"http://example.test/"}}}}},
        .started_at = time.utc_now(),
    };
}

} // anonymous namespace

TEST_CASE("daemon startup maps default and explicit concurrency to Scheduler options", "[jobud][startup]")
{
    jb::test::TemporaryDirectory directory;
    auto const                   ca_bundle = directory.path() / "ca.pem";
    std::ofstream{ca_bundle} << "test CA bundle";

    auto defaults = parse();
    REQUIRE(defaults);
    CHECK(defaults->socket_path == "/invocation/daemon.sock");
    CHECK(defaults->database_path == "/invocation/daemon.sqlite");
    CHECK_FALSE(defaults->allow_root_cli);
    CHECK_FALSE(defaults->http_proxy);
    CHECK_FALSE(defaults->http_ca_bundle);
    CHECK(scheduler_options(*defaults).cli_concurrency == 4U);
    CHECK(scheduler_options(*defaults).http_concurrency == 16U);

    auto explicit_options = parse({"--cli-concurrency=4294967295",
                                   "--http-concurrency",
                                   "1",
                                   "--allow-root-cli",
                                   "--http-proxy",
                                   "https://proxy.test",
                                   "--http-ca-bundle",
                                   ca_bundle.string()});
    REQUIRE(explicit_options);
    CHECK(scheduler_options(*explicit_options).cli_concurrency == 4294967295U);
    CHECK(scheduler_options(*explicit_options).http_concurrency == 1U);
    CHECK(explicit_options->allow_root_cli);
    CHECK(explicit_options->http_proxy == "https://proxy.test");
    CHECK(explicit_options->http_ca_bundle == ca_bundle);
    auto cli_only = parse({"--cli-concurrency", "1"});
    REQUIRE(cli_only);
    CHECK(scheduler_options(*cli_only).cli_concurrency == 1U);
    CHECK(scheduler_options(*cli_only).http_concurrency == 16U);
}

TEST_CASE("daemon startup rejects malformed values duplicates and values on the unsafe flag", "[jobud][startup]")
{
    for (auto const& name : {"--cli-concurrency", "--http-concurrency"}) {
        for (auto const& value : {"0", "4294967296", "-1", "+1", "1x", "1.0", " 1", "1 ", ""}) {
            CAPTURE(name, value);
            CHECK_FALSE(parse({name, value}));
        }
        CHECK_FALSE(parse({name}));
        CHECK_FALSE(parse({name, "1", name, "2"}));
    }
    for (auto const& arguments : std::vector<std::vector<std::string>>{
             {"--allow-root-cli", "--allow-root-cli"},
             {"--allow-root-cli=true"},
             {"--allow-root-cli=false"},
             {"--allow-root-cli="},
             {"--allow-root-cli", "true"},
             {"--allow-root-cli", ""},
             {"--unknown"},
             {"--"},
             {"unexpected"},
             {"--socket", "other.sock"},
             {"--database", "other.sqlite"},
             {"--http-proxy", ""},
             {"--http-ca-bundle"},
             {"--http-proxy", "a", "--http-proxy", "b"},
             {"--http-ca-bundle", "a", "--http-ca-bundle", "b"}
    }) {
        CAPTURE(arguments);
        CHECK_FALSE(parse(arguments));
    }
}

TEST_CASE("daemon startup keeps flag absence distinct from explicit negative overrides", "[jobud][startup]")
{
    auto absent = parse_arguments({"--check-config"});
    REQUIRE(absent);
    CHECK(absent->action == StartupAction::CheckConfig);
    CHECK_FALSE(absent->allow_root_cli);
    CHECK_FALSE(absent->allow_root_daemon);

    auto negative = parse_arguments({"--no-allow-root-cli", "--no-allow-root-daemon"});
    REQUIRE(negative);
    CHECK(negative->allow_root_cli == false);
    CHECK(negative->allow_root_daemon == false);

    for (auto const& flags : std::vector<std::vector<std::string>>{
             {"--config", "/file", "--no-config"},
             {"--no-config", "--config", "/file"},
             {"--allow-root-cli", "--no-allow-root-cli"},
             {"--no-allow-root-daemon", "--allow-root-daemon"},
             {"--run-as-user", "first", "--run-as-user", "second"},
             {"--help", "--version"},
             {"--check-config", "--check-config"},
    }) {
        CAPTURE(flags);
        CHECK_FALSE(parse_arguments(flags));
    }
    CHECK(parse_arguments({"--help", "--config", "/missing/jobud.ini"}));
    CHECK(parse_arguments({"--version", "--run-as-user", "no-such-account"}));
}

TEST_CASE("daemon startup resolves compiled config and flag precedence", "[jobud][startup]")
{
    auto const paths = CompiledPaths{.config   = "/compiled/jobud.ini",
                                     .database = "/compiled/jobu.sqlite3",
                                     .socket   = "/compiled/jobud.sock"};
    auto       empty = resolve_startup_options({}, {}, paths, "/invocation");
    REQUIRE(empty);
    CHECK(empty->socket_path == paths.socket);
    CHECK(empty->database_path == paths.database);
    CHECK_FALSE(empty->allow_root_cli);
    CHECK(empty->default_retention == std::chrono::seconds{2'592'000});

    auto config = parse_configuration_text("socket.path = /configured/jobud.sock\n"
                                           "database.path = /configured/jobu.sqlite3\n"
                                           "cli.concurrency = 7\n"
                                           "cli.allow_root = true\n"
                                           "daemon.allow_root = true\n"
                                           "daemon.run_as_group = operators\n"
                                           "history.default_retention = 0\n");
    REQUIRE(config);

    auto flags = parse_arguments({"--socket",
                                  "relative.sock",
                                  "--cli-concurrency",
                                  "3",
                                  "--no-allow-root-cli",
                                  "--no-allow-root-daemon",
                                  "--run-as-user",
                                  "daemon"});
    REQUIRE(flags);
    auto resolved = resolve_startup_options(*flags, *config, paths, "/invocation");
    REQUIRE(resolved);
    CHECK(resolved->socket_path == "/invocation/relative.sock");
    CHECK(resolved->database_path == "/configured/jobu.sqlite3");
    CHECK(resolved->cli_concurrency == 3U);
    CHECK_FALSE(resolved->allow_root_cli);
    CHECK_FALSE(resolved->allow_root_daemon);
    CHECK(resolved->run_as_user == "daemon");
    CHECK(resolved->run_as_group == "operators");
    CHECK(resolved->default_retention == std::chrono::seconds::zero());
    CHECK(resolved->rpc_read_buffer_capacity == 1'064'960U);

    CHECK_FALSE(resolve_startup_options({}, *config, paths, "/invocation"));

    jb::test::TemporaryDirectory directory;
    auto const                   ca_bundle = directory.path() / "ca.pem";
    std::ofstream{ca_bundle} << "test CA bundle";
    auto relative = parse_arguments({"--database", "state/daemon.sqlite", "--http-ca-bundle", "ca.pem"});
    REQUIRE(relative);
    auto relative_options = resolve_startup_options(*relative, {}, paths, directory.path());
    REQUIRE(relative_options);
    CHECK(relative_options->database_path == directory.path() / "state/daemon.sqlite");
    CHECK(relative_options->http_ca_bundle == ca_bundle);

    CHECK_FALSE(resolve_startup_options(
        {},
        {},
        {.config = paths.config, .database = paths.database, .socket = "/" + std::string(200, 'x')},
        "/invocation"));
}

TEST_CASE("daemon local validation checks supplied account names", "[jobud][startup]")
{
    StartupOptions options;
    options.run_as_user = "jobud-stage93-no-such-account";
    auto invalid_user   = validate_readonly_accounts(options);
    REQUIRE_FALSE(invalid_user);
    CHECK(invalid_user.error().key == "daemon.run_as_user");

    options.run_as_user.reset();
    options.socket_group = "jobud-stage93-no-such-group";
    auto invalid_group   = validate_readonly_accounts(options);
    REQUIRE_FALSE(invalid_group);
    CHECK(invalid_group.error().key == "socket.group");

    options.socket_group.reset();
    CHECK(validate_readonly_accounts(options));
}

TEST_CASE("daemon configuration file selection is bounded and read only", "[jobud][startup]")
{
    jb::test::TemporaryDirectory directory;
    auto const                   config_path = directory.path() / "jobud.ini";
    auto const                   paths =
        CompiledPaths{.config = config_path, .database = "/compiled/jobu.sqlite3", .socket = "/compiled/jobud.sock"};

    auto absent = load_configuration({}, paths, directory.path());
    REQUIRE(absent);
    CHECK_FALSE(absent->source_path);

    auto explicit_missing = parse_arguments({"--config", "missing.ini"});
    REQUIRE(explicit_missing);
    auto missing = load_configuration(*explicit_missing, paths, directory.path());
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code == "jobud.config.read_failed");
    CHECK(missing.error().category == ErrorCategory::Io);

    std::ofstream{config_path} << "cli.concurrency = 8\n";
    auto loaded = load_configuration({}, paths, directory.path());
    REQUIRE(loaded);
    CHECK(loaded->source_path == config_path);
    CHECK(loaded->input.cli_concurrency == 8U);

    auto explicit_relative = parse_arguments({"--config", "jobud.ini"});
    REQUIRE(explicit_relative);
    auto selected = load_configuration(*explicit_relative, paths, directory.path());
    REQUIRE(selected);
    CHECK(selected->source_path == config_path);

    auto skipped = parse_arguments({"--no-config"});
    REQUIRE(skipped);
    std::ofstream{config_path} << "unknown = secret-value\n";
    CHECK(load_configuration(*skipped, paths, directory.path()));
    auto invalid = load_configuration({}, paths, directory.path());
    REQUIRE_FALSE(invalid);
    CHECK(invalid.error().code == "jobud.config.unknown_key");
    CHECK(invalid.error().message.find("secret-value") == std::string::npos);

    std::ofstream{config_path} << std::string(65'537U, 'x');
    auto oversized = load_configuration({}, paths, directory.path());
    REQUIRE_FALSE(oversized);
    CHECK(oversized.error().code == "jobud.config.invalid");

    auto const link_path = directory.path() / "linked.ini";
    std::filesystem::create_symlink(config_path, link_path);
    auto linked = load_configuration({},
                                     {.config = link_path, .database = paths.database, .socket = paths.socket},
                                     directory.path());
    REQUIRE_FALSE(linked);
    CHECK(linked.error().code == "jobud.config.read_failed");
    CHECK(linked.error().category == ErrorCategory::PermissionDenied);
}

TEST_CASE("daemon configuration rejects a FIFO without waiting for a writer", "[jobud][startup]")
{
    jb::test::TemporaryDirectory directory;
    auto const                   fifo_path = directory.path() / "jobud.ini";
    REQUIRE(::mkfifo(fifo_path.c_str(), 0600) == 0);

    auto arguments = parse_arguments({"--config", fifo_path.string()});
    REQUIRE(arguments);
    auto loaded = load_configuration(*arguments, compiled_paths(), directory.path());
    REQUIRE_FALSE(loaded);
    CHECK(loaded.error().code == "jobud.config.read_failed");
    CHECK(loaded.error().category == ErrorCategory::PermissionDenied);
}

TEST_CASE("daemon CLI paths preserve symlink traversal through dot-dot", "[jobud][startup]")
{
    jb::test::TemporaryDirectory directory;
    auto const                   target = directory.path() / "target";
    std::filesystem::create_directories(target / "child");
    std::filesystem::create_directory_symlink(target / "child", directory.path() / "link");
    std::ofstream{target / "jobud.ini"} << "cli.concurrency = 9\n";
    std::ofstream{directory.path() / "jobud.ini"} << "unknown = wrong-file\n";
    std::ofstream{target / "ca.pem"} << "test CA bundle";

    auto arguments = parse_arguments({"--config",
                                      "link/../jobud.ini",
                                      "--database",
                                      "link/../state.sqlite",
                                      "--socket",
                                      "/tmp/jobu-link/../daemon.sock",
                                      "--http-ca-bundle",
                                      "link/../ca.pem"});
    REQUIRE(arguments);

    auto loaded = load_configuration(*arguments, compiled_paths(), directory.path());
    REQUIRE(loaded);
    CHECK(loaded->input.cli_concurrency == 9U);
    CHECK(loaded->source_path == directory.path() / "link/../jobud.ini");

    auto resolved = resolve_startup_options(*arguments, loaded->input, compiled_paths(), directory.path());
    REQUIRE(resolved);
    CHECK(resolved->database_path == directory.path() / "link/../state.sqlite");
    CHECK(resolved->socket_path == "/tmp/jobu-link/../daemon.sock");
    CHECK(resolved->http_ca_bundle == directory.path() / "link/../ca.pem");
}

TEST_CASE("daemon composition applies live identity policy and warns once only for unsafe root", "[jobud][startup]")
{
    Application              app{0, nullptr};
    SystemTimeSource         time;
    jb::test::FakeHttpClient client;

    for (auto uid : {std::uint64_t{0}, std::uint64_t{1000}}) {
        for (auto allow_root : {false, true}) {
            CAPTURE(uid, allow_root);
            IdentityState   identity{.uid = uid};
            CaptureWarnings warnings;
            {
                AttemptExecutorGroup group;
                REQUIRE(register_attempt_executors(group,
                                                   client,
                                                   time,
                                                   allow_root,
                                                   std::make_unique<ObservedIdentity>(identity)));
                CHECK(group.is_available(JobType::Http));
                CHECK(group.is_available(JobType::Cli) == (uid != 0 || allow_root));

                // Changing the probe after composition proves availability is rechecked, not cached at startup.
                identity.uid = uid == 0 ? 1000 : 0;
                CHECK(group.is_available(JobType::Cli) == (identity.uid != 0 || allow_root));
                CHECK(identity.destructions == 0U);
                auto messages = warnings.capture->warnings();
                REQUIRE(messages.size() == (uid == 0 && allow_root ? 1U : 0U));
                if (!messages.empty()) {
                    CHECK(messages.front() == "jobud.unsafe.root_cli");
                }
            }
            CHECK(identity.destructions == 1U);
        }
    }
}

TEST_CASE("daemon composition cleans up owned runners before their borrowed HTTP client", "[jobud][lifetime]")
{
    Application              app{0, nullptr};
    SystemTimeSource         time;
    jb::test::FakeHttpClient client;
    IdentityState            identity;
    auto                     completions = std::size_t{0};
    {
        AttemptExecutorGroup group;
        REQUIRE(register_attempt_executors(group, client, time, false, std::make_unique<ObservedIdentity>(identity)));
        auto started = group.start(http_request(time), [&completions](AttemptCompletion const&) { ++completions; });
        REQUIRE(started);
        REQUIRE(client.active_request_count() == 1U);
    }
    CHECK(identity.destructions == 1U);
    REQUIRE(client.cancel_calls().size() == 1U);
    // The client can deliver its owed cancellation after both group and executor have gone away.
    REQUIRE(client.complete_cancelled(client.cancel_calls().front()));
    CHECK(completions == 0U);
}

TEST_CASE("daemon composition registration failures retain only previously owned runners", "[jobud][lifetime]")
{
    Application              app{0, nullptr};
    SystemTimeSource         time;
    jb::test::FakeHttpClient client;
    IdentityState            identity;
    IdentityState            existing_identity;
    CaptureWarnings          warnings;
    {
        AttemptExecutorGroup group;
        SECTION("HTTP registration fails before CLI construction")
        {
            REQUIRE(group.add(JobType::Http, std::make_unique<http::HttpAttemptExecutor>(client, time)));
        }
        SECTION("CLI registration fails after HTTP ownership transfer")
        {
            REQUIRE(group.add(
                JobType::Cli,
                cli::detail::CliAttemptExecutorFactory::create({},
                                                               std::make_unique<ObservedIdentity>(existing_identity))));
        }
        identity.uid = 0;
        auto registered =
            register_attempt_executors(group, client, time, true, std::make_unique<ObservedIdentity>(identity));
        REQUIRE_FALSE(registered);
        CHECK(registered.error().code == "jobu.executor.duplicate_type");
        CHECK(identity.destructions == 1U);
        CHECK(group.is_available(JobType::Http));
        CHECK(warnings.capture->warnings().empty());
    }
    CHECK(identity.destructions == 1U);
}

TEST_CASE("root daemon composition leaves CLI pending while HTTP and management work", "[jobud][startup]")
{
    Application                  app{0, nullptr};
    jb::test::TemporaryDirectory directory;
    SystemTimeSource             time;
    UuidV7Generator              generator{time};
    StandardAttributeRegistry    registry;
    SystemCronEngine             cron;
    jb::db::Database             database{std::make_unique<jb::db::sqlite::Driver>(
        jb::db::sqlite::Options{.database_file = directory.path() / "jobu.sqlite"})};
    REQUIRE(database.open());
    REQUIRE(jb::jobu::sqlite::ensure_schema(database));
    jb::test::FakeHttpClient client;
    IdentityState            identity{.uid = 0};
    AttemptExecutorGroup     group;
    REQUIRE(register_attempt_executors(group, client, time, false, std::make_unique<ObservedIdentity>(identity)));
    auto startup = parse({"--http-concurrency", "1"});
    REQUIRE(startup);
    jb::test::RejectingSecretProvider secrets;
    Scheduler         scheduler{database, registry, cron, generator, time, group, secrets, scheduler_options(*startup)};
    ManagementService management{database, registry, cron, generator, time};
    jb::jobu::detail::RunRepository runs{database, registry};

    auto queue = management.create_queue({.name = "root-policy", .concurrency_limit = 2});
    REQUIRE(queue);
    auto cli_job = management.create_job({
        .queue    = queue->id,
        .type     = JobType::Cli,
        .schedule = OnceSchedule{.planned_at = UtcTimePoint{}},
        .payload = JsonValue{.data = JsonValue::Object{{"command", JsonValue{.data = std::string{"/never-executed"}}}}},
    });
    REQUIRE(cli_job);
    auto http_job = management.create_job({
        .queue    = queue->id,
        .type     = JobType::Http,
        .schedule = OnceSchedule{.planned_at = UtcTimePoint{}},
        .payload  = http_request(time).payload,
    });
    REQUIRE(http_job);
    auto http_run = runs.find_schedule_owned(http_job->id);
    REQUIRE(http_run);
    REQUIRE(http_run->has_value());

    REQUIRE(scheduler.start());
    REQUIRE(client.start_records().size() == 1U);
    auto pending = runs.find_schedule_owned(cli_job->id);
    REQUIRE(pending);
    REQUIRE(pending->has_value());
    CHECK(pending->value().state == RunState::Scheduled);
    CHECK_FALSE(pending->value().started_at);

    // Discharge HTTP while Scheduler is alive; root-denied CLI has never acquired a completion obligation.
    scheduler.stop();
    REQUIRE(client.complete_success(client.start_records().front().id, {.status_code = 204}));
    auto completed = runs.find_by_id(http_run->value().id);
    REQUIRE(completed);
    REQUIRE(completed->has_value());
    CHECK(completed->value().state == RunState::Succeeded);
}
