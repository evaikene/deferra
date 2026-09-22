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
#include "support/fake_http_client.hpp"
#include "support/rejecting_secret_provider.hpp"
#include "support/temporary_directory.hpp"
#include "time_source.hpp"
#include "uuid.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace jb::core;
using namespace jb::jobu;
using namespace jb::jobud::detail;

namespace {

auto parse(std::vector<std::string> extra = {}) -> std::optional<StartupOptions>
{
    auto arguments = std::vector<std::string>{"jobud", "--socket", "daemon.sock", "--database", "daemon.sqlite"};
    arguments.insert(arguments.end(), extra.begin(), extra.end());
    auto argv = std::vector<char const*>{};
    for (auto const& argument : arguments) {
        argv.push_back(argument.c_str());
    }
    return parse_startup_options(static_cast<int>(argv.size()), argv.data());
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
        auto lock = std::lock_guard{_mutex};
        if (message.level == LogLevel::Warning) {
            _warnings.emplace_back(message.message);
        }
    }

    auto warnings() -> std::vector<std::string>
    {
        auto lock = std::lock_guard{_mutex};
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
    auto defaults = parse();
    REQUIRE(defaults);
    CHECK(defaults->socket_path == "daemon.sock");
    CHECK(defaults->database_path == "daemon.sqlite");
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
                                   "/test/ca.pem"});
    REQUIRE(explicit_options);
    CHECK(scheduler_options(*explicit_options).cli_concurrency == 4294967295U);
    CHECK(scheduler_options(*explicit_options).http_concurrency == 1U);
    CHECK(explicit_options->allow_root_cli);
    CHECK(explicit_options->http_proxy == "https://proxy.test");
    CHECK(explicit_options->http_ca_bundle == "/test/ca.pem");
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
                    CHECK(messages.front() == "UNSAFE: --allow-root-cli enables command execution as root");
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
