#include "attribute_registry.hpp"
#include "byte_buffer.hpp"
#include "command_line_priv.hpp"
#include "command_registry_priv.hpp"
#include "control_json.hpp"
#include "help_priv.hpp"
#include "input_priv.hpp"
#include "json.hpp"
#include "management_json.hpp"
#include "statistics_json.hpp"
#include "support/catch_utils.hpp" // IWYU pragma: keep for Catch::StringMaker specializations
#include "support/temporary_directory.hpp"
#include "utc_timestamp.hpp"
#include "uuid.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <utility>
#include <variant> // IWYU pragma: keep for std::get in Catch assertions
#include <vector>

using namespace jb::core;
using namespace jb::jobu;
using namespace jb::jobuctl::detail;

namespace {

constexpr auto job_id = "00000000-0000-7000-8000-000000000001";

// Destroy the lexer inputs and registry before inspecting the result: parsed commands must own their data.
auto parse_action(std::vector<std::string> arguments) -> ParseResult
{
    arguments.insert(arguments.begin(), "jobuctl");
    auto argv = std::vector<char*>{};
    for (auto& argument : arguments) {
        argv.push_back(argument.data());
    }
    StandardAttributeRegistry registry;
    return parse_command_line(static_cast<int>(argv.size()), argv.data(), registry);
}

// Existing parity cases examine remote requests; local-action cases use parse_action directly.
auto parse(std::vector<std::string> arguments) -> CommandBuildResult
{
    auto result = parse_action(std::move(arguments));
    if (result.action) {
        if (auto* command = std::get_if<Command>(&*result.action)) {
            return {.command = std::move(*command)};
        }
    }
    return {.error = std::move(result.error)};
}

} // namespace

TEST_CASE("jobuctl parser retains socket selection and command family errors", "[jobuctl][parse]")
{
    auto parsed = parse({"--socket", "/tmp/custom.sock", "system", "info"});
    REQUIRE(parsed.command);
    CHECK(parsed.command->socket_path == "/tmp/custom.sock");
    CHECK(parsed.command->method == "system.info");
    CHECK(std::holds_alternative<std::monostate>(parsed.command->request));

    CHECK(parse({"system", "info", "--socket", "/tmp/custom.sock"}).command.has_value());
    CHECK(parse({"--socket", "/tmp/custom.sock", "system", "info", "extra"}).error == "unexpected extra operand");
    CHECK(parse({"--socket", "/tmp/custom.sock", "queue", "unknown"}).error == "unknown command action");
    CHECK(parse({"--socket", "/tmp/custom.sock", "job", "unknown"}).error == "unknown command action");
    CHECK(parse({"--socket", "/tmp/custom.sock", "queue", "list", "--socket", "/tmp/other.sock"}).error ==
          "--socket may be supplied only once");
}

TEST_CASE("jobuctl parses machine options before or after the command", "[jobuctl][parse]")
{
    auto first = parse({"--json", "--timeout", "75", "--socket", "fixture.sock", "queue", "list"});
    auto last  = parse({"--socket", "fixture.sock", "queue", "list", "--timeout=75", "--json"});
    REQUIRE(first.command);
    REQUIRE(last.command);
    CHECK(first.command->json);
    CHECK(last.command->json);
    CHECK(first.command->timeout == std::chrono::milliseconds{75});
    CHECK(last.command->timeout == first.command->timeout);

    auto file = parse({"queue", "create", "--socket", "fixture.sock", "--request-file", "request.json"});
    REQUIRE(file.command);
    CHECK(file.command->request_file == std::filesystem::path{"request.json"});
    CHECK(std::holds_alternative<std::monostate>(file.command->request));
    CHECK_FALSE(
        parse({"queue", "create", "reports", "--request-file", "request.json", "--socket", "fixture.sock"}).command);

    for (auto const* invalid : {"0", "-1", "18446744073709551615", "abc"}) {
        CHECK_FALSE(parse({"--socket", "fixture.sock", "queue", "list", "--timeout", invalid}).command);
    }
    auto syntax = parse_action({"queue", "list", "--unknown", "--json"});
    CHECK_FALSE(syntax.action);
    CHECK(syntax.json_requested);
    CHECK_FALSE(parse_action({"queue", "list", "--arg=--json"}).json_requested);
}

TEST_CASE("jobuctl statistics options preserve typed filters and cursor-only continuation", "[jobuctl][parse]")
{
    auto system = parse({"--socket",       "fixture.sock",
                         "system",         "stats",
                         "--queue-id",     job_id,
                         "--job-id",       job_id,
                         "--type",         "cli",
                         "--origin",       "manual",
                         "--planned-from", "2030-01-01T00:00:00Z",
                         "--planned-to",   "2030-01-02T00:00:00Z",
                         "--group-by",     "state",
                         "--limit",        "2"});
    REQUIRE(system.command);
    CHECK(system.command->method == "system.stats");
    auto const& query = std::get<StatisticsRequest>(std::get<StatisticsListRequest>(system.command->request));
    CHECK(query.queue_id.has_value());
    CHECK(query.job_id.has_value());
    CHECK(query.type == JobType::Cli);
    CHECK(query.origin == RunOrigin::Manual);
    CHECK(query.planned.from.has_value());
    CHECK(query.planned.to.has_value());
    CHECK(query.group_by == StatisticsGroupBy::State);
    CHECK(query.limit == 2);

    auto queue = parse({"--socket", "fixture.sock", "queue", "stats", "--name", "reports", "--group-by", "job"});
    REQUIRE(queue.command);
    CHECK(queue.command->method == "queue.stats");
    auto const& scoped = std::get<QueueStatisticsQuery>(std::get<QueueStatisticsListRequest>(queue.command->request));
    CHECK(std::get<std::string>(scoped.selector) == "reports");
    CHECK(scoped.statistics.group_by == StatisticsGroupBy::Job);

    auto continuation = parse({"--socket", "fixture.sock", "queue", "stats", "--cursor", "opaque-token"});
    REQUIRE(continuation.command);
    CHECK(std::get<CursorRequest>(std::get<QueueStatisticsListRequest>(continuation.command->request)).cursor ==
          "opaque-token");
    CHECK_FALSE(parse({"--socket", "fixture.sock", "queue", "stats"}).command);
    CHECK_FALSE(parse({"--socket", "fixture.sock", "queue", "stats", "--id", job_id, "--name", "reports"}).command);
    CHECK_FALSE(
        parse({"--socket", "fixture.sock", "queue", "stats", "--name", "reports", "--cursor", "token"}).command);
    CHECK_FALSE(parse({"--socket", "fixture.sock", "system", "stats", "--cursor", "token", "--limit", "2"}).command);
    CHECK_FALSE(parse({"--socket",
                       "fixture.sock",
                       "system",
                       "stats",
                       "--planned-from",
                       "2030-01-02T00:00:00Z",
                       "--planned-to",
                       "2030-01-01T00:00:00Z"})
                    .command);
}

TEST_CASE("jobuctl schedule preview uses cron-only typed requests", "[jobuctl][parse]")
{
    auto valid = parse({"--socket", "fixture.sock", "schedule", "validate", "0 9 * * FRI-MON"});
    REQUIRE(valid.command);
    CHECK(valid.command->method == "schedule.validate");
    CHECK(std::get<CronSchedule>(valid.command->request).timezone == "UTC");

    auto next = parse({"--socket",
                       "fixture.sock",
                       "schedule",
                       "next",
                       "@daily",
                       "--timezone",
                       "Europe/Tallinn",
                       "--after",
                       "2030-01-01T00:00:00Z",
                       "--count",
                       "2"});
    REQUIRE(next.command);
    CHECK(next.command->method == "schedule.next");
    auto const& request = std::get<ScheduleNextRequest>(next.command->request);
    CHECK(request.schedule.expression == "@daily");
    CHECK(request.schedule.timezone == "Europe/Tallinn");
    CHECK(request.count == 2);

    CHECK_FALSE(parse({"--socket", "fixture.sock", "schedule", "validate"}).command);
    CHECK_FALSE(parse({"--socket", "fixture.sock", "schedule", "next", "@daily"}).command);
    CHECK_FALSE(parse({"--socket", "fixture.sock", "schedule", "next", "@daily", "--after", "2030-01-01"}).command);
    CHECK_FALSE(parse({"--socket",
                       "fixture.sock",
                       "schedule",
                       "next",
                       "@daily",
                       "--after",
                       "2030-01-01T00:00:00Z",
                       "--count",
                       "201"})
                    .command);
}

TEST_CASE("jobuctl statistics and schedule request files use strict public codecs", "[jobuctl][input]")
{
    jb::test::TemporaryDirectory directory;
    StandardAttributeRegistry    registry;
    auto const                   file_path = directory.path() / "params.json";

    struct Case {
        std::vector<std::string> command;
        std::string              document;
    };

    auto const cases = std::vector<Case>{
        {.command = {"system", "stats"},       .document = R"({"group_by":"state","limit":2})"                                },
        {.command = {"queue", "stats"},        .document = R"({"queue_name":"reports","group_by":"job"})"                     },
        {.command  = {"schedule", "validate"},
         .document = R"({"schedule":{"kind":"cron","expression":"@daily","timezone":"UTC"}})"                                 },
        {.command = {"schedule", "next"},
         .document =
             R"({"schedule":{"kind":"cron","expression":"@daily","timezone":"UTC"},"after":"2030-01-01T00:00:00Z","count":2})"},
    };
    for (auto const& item : cases) {
        auto file = std::ofstream{file_path, std::ios::binary | std::ios::trunc};
        REQUIRE(file);
        file << item.document;
        file.close();

        auto arguments = std::vector<std::string>{"--socket", "fixture.sock"};
        arguments.insert(arguments.end(), item.command.begin(), item.command.end());
        arguments.insert(arguments.end(), {"--request-file", file_path.string()});
        auto parsed = parse(std::move(arguments));
        REQUIRE(parsed.command);
        REQUIRE(load_request_file(*parsed.command, registry));

        file.open(file_path, std::ios::binary | std::ios::trunc);
        REQUIRE(file);
        file << R"({"unexpected":true})";
        file.close();
        CHECK_FALSE(load_request_file(*parsed.command, registry));
    }
}

TEST_CASE("jobuctl request files are bounded and strictly decoded", "[jobuctl][input]")
{
    jb::test::TemporaryDirectory directory;
    StandardAttributeRegistry    registry;
    auto const                   path = directory.path() / "request.json";
    auto command = parse({"--socket", "fixture.sock", "queue", "list", "--request-file", path.string()});
    REQUIRE(command.command);

    {
        auto file = std::ofstream{path, std::ios::binary};
        REQUIRE(file);
        file << "{\"limit\":2}";
    }
    REQUIRE(load_request_file(*command.command, registry));
    CHECK(std::get<QueueListRequest>(command.command->request).page.limit == 2);

    for (auto const& document : {"[]", R"({"limit":2} trailing)", R"({"limit":2,"unexpected":1})"}) {
        auto file = std::ofstream{path, std::ios::binary | std::ios::trunc};
        REQUIRE(file);
        file << document;
        file.close();

        auto result = load_request_file(*command.command, registry);
        REQUIRE_FALSE(result);
        CHECK(result.error().message.find(document) == std::string::npos);
    }

    {
        auto file = std::ofstream{path, std::ios::binary | std::ios::trunc};
        REQUIRE(file);
        file << std::string((1024U * 1024U) + 1U, 'x');
    }
    auto oversized = load_request_file(*command.command, registry);
    REQUIRE_FALSE(oversized);
    CHECK(oversized.error().code == "jobuctl.input.too_large");

    auto const at = parse_utc_timestamp("2030-01-01T00:00:00Z");
    REQUIRE(at);
    auto payload = parse_json(R"({"command":"/bin/true","arguments":[{"secret":"reports.token"}]})");
    REQUIRE(payload);
    auto create = CreateJobRequest{
        .queue    = std::string{"reports"},
        .type     = JobType::Cli,
        .schedule = OnceSchedule{.planned_at = *at},
        .payload  = *payload,
    };
    auto encoded = create_job_request_to_json(create, registry);
    REQUIRE(encoded);
    auto serialized = serialize_json(*encoded);
    REQUIRE(serialized);
    {
        auto file = std::ofstream{path, std::ios::binary | std::ios::trunc};
        REQUIRE(file);
        file << *serialized;
    }
    auto job = parse({"--socket", "fixture.sock", "job", "create", "--request-file", path.string()});
    REQUIRE(job.command);
    REQUIRE(load_request_file(*job.command, registry));
    CHECK(std::get<CreateJobRequest>(job.command->request).payload == *payload);

    {
        auto file = std::ofstream{path, std::ios::binary | std::ios::trunc};
        REQUIRE(file);
        file
            << R"({"job_id":"00000000-0000-7000-8000-000000000001","expected_revision":7,"name":null,"type":"http","schedule":{"kind":"cron","expression":"0 9 * * *","timezone":"UTC"},"attributes":{"retry.max_attempts":2},"payload":{"url":"https://example.test","headers":[{"name":"Authorization","value":{"secret":"service.token"}}]}})";
    }
    auto update = parse({"--socket", "fixture.sock", "job", "update", "--request-file", path.string()});
    REQUIRE(update.command);
    REQUIRE(load_request_file(*update.command, registry));
    auto const& update_request = std::get<UpdateJobRequest>(update.command->request);
    REQUIRE(update_request.name);
    CHECK_FALSE(*update_request.name);
    CHECK(update_request.expected_revision == 7);
    CHECK(update_request.type == JobType::Http);
    CHECK(std::holds_alternative<CronSchedule>(*update_request.schedule));
    CHECK(update_request.attribute_changes.contains("retry.max_attempts"));
    REQUIRE(update_request.payload);
    CHECK(update_request.payload->as_object()
              .at("headers")
              .as_array()
              .front()
              .as_object()
              .at("value")
              .as_object()
              .at("secret")
              .as_string() == "service.token");

    {
        auto file = std::ofstream{path, std::ios::binary | std::ios::trunc};
        REQUIRE(file);
        file << R"({"queue_name":"reports","defaults":{},"history_retention_seconds":null})";
    }
    auto queue_update = parse({"--socket", "fixture.sock", "queue", "update", "--request-file", path.string()});
    REQUIRE(queue_update.command);
    REQUIRE(load_request_file(*queue_update.command, registry));
    auto const& queue_request = std::get<UpdateQueueRequest>(queue_update.command->request);
    REQUIRE(queue_request.defaults);
    CHECK(queue_request.defaults->empty());
    REQUIRE(queue_request.history_retention);
    CHECK_FALSE(*queue_request.history_retention);
}

TEST_CASE("jobuctl accepts one params object from standard input", "[jobuctl][input]")
{
    auto command = parse({"--socket", "fixture.sock", "queue", "list", "--request-file", "-"});
    REQUIRE(command.command);

    auto                      source   = std::istringstream{"{\"limit\":3}"};
    auto*                     original = std::cin.rdbuf(source.rdbuf());
    StandardAttributeRegistry registry;
    auto                      loaded = load_request_file(*command.command, registry);
    std::cin.rdbuf(original);
    std::cin.clear();

    REQUIRE(loaded);
    CHECK(std::get<QueueListRequest>(command.command->request).page.limit == 3);
}

TEST_CASE("jobuctl secret commands select metadata-only requests and deferred input", "[jobuctl][parse]")
{
    auto set = parse({"--socket", "fixture.sock", "secret", "set", "reports.token", "--stdin"});
    REQUIRE(set.command);
    CHECK(set.command->method == "secret.set");
    REQUIRE(set.command->secret_input);
    CHECK(set.command->secret_input->source == SecretInput::Source::Stdin);
    CHECK(std::get<SetSecretRequest>(set.command->request).name == "reports.token");
    CHECK(std::get<SetSecretRequest>(set.command->request).value.empty());

    auto list = parse({"secret", "list", "--socket", "fixture.sock", "--limit", "1", "--after-name", "reports.a"});
    REQUIRE(list.command);
    CHECK(list.command->method == "secret.list");
    CHECK(std::get<SecretListRequest>(list.command->request).limit == 1);
    CHECK(std::get<SecretListRequest>(list.command->request).after_name == "reports.a");

    auto erase = parse({"--socket", "fixture.sock", "secret", "delete", "reports.token"});
    REQUIRE(erase.command);
    CHECK(erase.command->method == "secret.delete");
    CHECK(std::get<std::string>(erase.command->request) == "reports.token");

    for (auto const& arguments : std::vector<std::vector<std::string>>{
             {"secret", "set", "reports.token"},
             {"secret", "set", "reports.token", "--file", "a", "--stdin"},
             {"secret", "set", "reports.token", "--stdin", "--value", "literal"},
             {"secret", "set", "reports.token", "--stdin", "--request-file", "request.json"},
             {"secret", "list", "--limit", "0"},
             {"secret", "list", "--after-name", ""},
             {"secret", "delete"},
    }) {
        auto full = arguments;
        full.insert(full.begin(), {"--socket", "fixture.sock"});
        CHECK_FALSE(parse(std::move(full)).command);
    }
}

TEST_CASE("jobuctl reads secret file and stdin as bounded raw bytes", "[jobuctl][input]")
{
    jb::test::TemporaryDirectory directory;
    auto const                   path = directory.path() / "secret.bin";
    auto command = parse({"--socket", "fixture.sock", "secret", "set", "reports.token", "--file", path.string()});
    REQUIRE(command.command);

    auto const sample = std::string{"\0\xff\n", 3};
    {
        auto file = std::ofstream{path, std::ios::binary};
        REQUIRE(file);
        file.write(sample.data(), static_cast<std::streamsize>(sample.size()));
    }
    REQUIRE(load_secret_input(*command.command));
    CHECK(as_string_view(std::get<SetSecretRequest>(command.command->request).value) == sample);

    for (auto const size : {std::size_t{0}, std::size_t{65536}, std::size_t{65537}}) {
        auto file = std::ofstream{path, std::ios::binary | std::ios::trunc};
        REQUIRE(file);
        file << std::string(size, 'x');
        file.close();

        auto loaded = load_secret_input(*command.command);
        if (size > 65536) {
            REQUIRE_FALSE(loaded);
            CHECK(loaded.error().code == "jobuctl.input.too_large");
        }
        else {
            REQUIRE(loaded);
            CHECK(std::get<SetSecretRequest>(command.command->request).value.size() == size);
        }
    }

    auto stdin_command = parse({"--socket", "fixture.sock", "secret", "set", "reports.token", "--stdin"});
    REQUIRE(stdin_command.command);
    auto  source   = std::istringstream{sample};
    auto* original = std::cin.rdbuf(source.rdbuf());
    auto  loaded   = load_secret_input(*stdin_command.command);
    std::cin.rdbuf(original);
    std::cin.clear();
    REQUIRE(loaded);
    CHECK(as_string_view(std::get<SetSecretRequest>(stdin_command.command->request).value) == sample);
}

TEST_CASE("jobuctl decodes structured secret requests without exposing input", "[jobuctl][input]")
{
    jb::test::TemporaryDirectory directory;
    StandardAttributeRegistry    registry;
    auto const                   path  = directory.path() / "request.json";
    auto                         write = [&](std::string_view document) {
        auto file = std::ofstream{path, std::ios::binary | std::ios::trunc};
        REQUIRE(file);
        file << document;
    };

    auto set = parse({"--socket", "fixture.sock", "secret", "set", "--request-file", path.string()});
    REQUIRE(set.command);
    write(R"({"name":"reports.token","value":{"encoding":"base64","data":"AP8K"}})");
    REQUIRE(load_request_file(*set.command, registry));
    CHECK(as_string_view(std::get<SetSecretRequest>(set.command->request).value) == std::string{"\0\xff\n", 3});

    write(R"({"name":"reports.token","value":{"encoding":"base64","data":"secret-sentinel"}})");
    auto invalid = load_request_file(*set.command, registry);
    REQUIRE_FALSE(invalid);
    CHECK(invalid.error().message.find("secret-sentinel") == std::string::npos);

    auto list = parse({"--socket", "fixture.sock", "secret", "list", "--request-file", path.string()});
    REQUIRE(list.command);
    write(R"({"limit":2,"after_name":"reports.a"})");
    REQUIRE(load_request_file(*list.command, registry));
    CHECK(std::get<SecretListRequest>(list.command->request).limit == 2);

    auto erase = parse({"--socket", "fixture.sock", "secret", "delete", "--request-file", path.string()});
    REQUIRE(erase.command);
    write(R"({"name":"reports.token"})");
    REQUIRE(load_request_file(*erase.command, registry));
    CHECK(std::get<std::string>(erase.command->request) == "reports.token");
}

TEST_CASE("jobuctl parser preserves queue selectors and deletion rendering identity", "[jobuctl][parse]")
{
    for (auto const* action : {"get", "suspend", "resume", "delete"}) {
        CAPTURE(action);
        auto named = parse({"--socket", "fixture.sock", "queue", action, "--name", "queue with spaces"});
        REQUIRE(named.command);
        auto const& selector = std::get<QueueSelector>(named.command->request);
        CHECK(std::get<std::string>(selector) == "queue with spaces");
        CHECK(named.command->method == std::string{"queue."} + action);

        auto identified = parse({"--socket", "fixture.sock", "queue", action, "--id", job_id});
        REQUIRE(identified.command);
        CHECK(std::get<Uuid>(std::get<QueueSelector>(identified.command->request)).to_string() == job_id);
    }

    auto duplicate = parse({"--socket", "fixture.sock", "queue", "get", "--id", job_id, "--name", "queue"});
    CHECK_FALSE(duplicate.command);
    CHECK(duplicate.error == "expected exactly one valid --id or --name selector");
}

TEST_CASE("jobuctl parser preserves job identity revisions and signed priority", "[jobuctl][parse]")
{
    auto parsed = parse({"--socket",
                         "fixture.sock",
                         "job",
                         "update",
                         job_id,
                         "--revision",
                         "18446744073709551615",
                         "--priority",
                         "-2147483648",
                         "--clear-name"});
    REQUIRE(parsed.command);
    auto const& request = std::get<UpdateJobRequest>(parsed.command->request);
    CHECK(request.job_id.to_string() == job_id);
    CHECK(request.expected_revision == std::numeric_limits<std::uint64_t>::max());
    CHECK(request.priority == std::numeric_limits<std::int32_t>::min());
    REQUIRE(request.name);
    CHECK_FALSE(*request.name);

    for (auto const* revision : {"0", "-1", "18446744073709551616"}) {
        CAPTURE(revision);
        auto invalid = parse({"--socket", "fixture.sock", "job", "delete", job_id, "--revision", revision});
        CHECK_FALSE(invalid.command);
    }
    for (auto const* action : {"get", "suspend", "resume"}) {
        auto selected = parse({"--socket", "fixture.sock", "job", action, job_id});
        REQUIRE(selected.command);
        CHECK(selected.command->method == std::string{"job."} + action);
        CHECK(std::get<Uuid>(selected.command->request).to_string() == job_id);
    }

    auto deleted = parse({"--socket", "fixture.sock", "job", "delete", job_id, "--revision", "7"});
    REQUIRE(deleted.command);
    auto const& deletion = std::get<DeleteJobRequest>(deleted.command->request);
    CHECK(deletion.expected_revision == 7);
    CHECK(deletion.job_id.to_string() == job_id);
}

TEST_CASE("jobuctl parser retains exact repeated subprocess argument bytes", "[jobuctl][parse]")
{
    auto parsed = parse({"--socket",
                         "fixture.sock",
                         "job",
                         "create",
                         "--queue-name",
                         "queue",
                         "--type",
                         "cli",
                         "--at",
                         "2030-01-01T00:00:00Z",
                         "--command",
                         "/bin/true",
                         "--priority",
                         "-12",
                         "--arg",
                         "",
                         "--arg",
                         "-abc",
                         "--arg",
                         "--unknown",
                         "--arg",
                         "--env",
                         "--arg",
                         "--env=NAME=value",
                         "--arg=--command",
                         "--arg=--help",
                         "--arg=",
                         "--env",
                         "ACTUAL=value"});
    REQUIRE(parsed.command);
    auto const& request = std::get<CreateJobRequest>(parsed.command->request);
    CHECK(request.priority == -12);
    auto expected = parse_json(
        R"({"command":"/bin/true","arguments":["","-abc","--unknown","--env","--env=NAME=value","--command","--help",""],"environment":{"ACTUAL":"value"}})");
    REQUIRE(expected);
    CHECK(request.payload == *expected);
}

TEST_CASE("jobuctl help is local at root group leaf and alias paths", "[jobuctl][help]")
{
    for (auto const& args : std::vector<std::vector<std::string>>{{}, {"--help"}, {"-h"}, {"help"}}) {
        auto result = parse_action(args);
        REQUIRE(result.action);
        auto const* help = std::get_if<HelpCommand>(&*result.action);
        REQUIRE(help);
        CHECK(help->group.empty());
        auto text = render_help(*help);
        CHECK(text.find("Groups:") != std::string::npos);
        CHECK(text.find("--socket PATH") != std::string::npos);
        CHECK(text.find("--expected-exit-code") == std::string::npos);
    }
    for (auto const& group : command_groups()) {
        for (auto const& args : std::vector<std::vector<std::string>>{
                 {std::string{group.name}},
                 {std::string{group.name}, "--help"},
                 {"help", std::string{group.name}}
        }) {
            auto result = parse_action(args);
            REQUIRE(result.action);
            auto const& help = std::get<HelpCommand>(*result.action);
            CHECK(help.group == group.name);
            CHECK(help.action.empty());
            CHECK(render_help(help).find("Commands:") != std::string::npos);
        }
    }

    // Every currently supported leaf must explain itself without requiring its operands or a daemon socket.
    for (auto const& spec : command_specs()) {
        for (auto const& args : std::vector<std::vector<std::string>>{
                 {std::string{spec.group}, std::string{spec.name},  "--help"              },
                 {"help",                  std::string{spec.group}, std::string{spec.name}},
                 {"-h",                    std::string{spec.group}, std::string{spec.name}}
        }) {
            CAPTURE(args);
            auto result = parse_action(args);
            REQUIRE(result.action);
            auto text = render_help(std::get<HelpCommand>(*result.action));
            CHECK(text.find(spec.summary) != std::string::npos);
            CHECK(text.find("Example:") != std::string::npos);
            for (auto const& option : spec.options) {
                CHECK(text.find("--" + std::string{option.option.long_name}) != std::string::npos);
            }
        }
        if (!spec.alias.empty()) {
            auto alias = parse_action({std::string{spec.group}, std::string{spec.alias}, "--help"});
            REQUIRE(alias.action);
            auto text = render_help(std::get<HelpCommand>(*alias.action));
            CHECK(text == render_help({std::string{spec.group}, std::string{spec.name}}));
            CHECK(text.find("Alias:") != std::string::npos);
        }
    }
}

TEST_CASE("jobuctl help does not conceal lexical errors or unknown paths", "[jobuctl][help]")
{
    for (auto const& args : std::vector<std::vector<std::string>>{
             {"unknown", "--help"},
             {"queue", "unknown", "--help"},
             {"help", "queue", "unknown"},
             {"queue", "--unknown", "--help"},
             {"queue", "list", "--weight", "2", "--help"},
             {"queue", "create", "--weight", "--help"},
             {"queue", "create", "--weight", "-h"},
             {"queue", "get", "--name", "--help"},
             {"--socket", "--help"},
             {"--socket=", "--help"},
             {"queue", "list", "--include-deleted=true", "--help"},
             {"--help=anything"},
             {"queue", "create", "one", "two", "--help"},
             {"help", "queue", "create", "extra"},
             {"queue", "list", "--", "--help"},
             {"--", "queue", "--help"},
             {"job", "create", "--arg", "--socket", "x", "--help"},
             {"job", "create", "--arg", "--", "--help"},
             {"queue", "--version"},
             {"--version", "--help"},
             {"queue", "list", "--limit", "1", "--limit", "2", "--help"}
    }) {
        CAPTURE(args);
        auto result = parse_action(args);
        CHECK_FALSE(result.action);
        CHECK_FALSE(result.error.empty());
    }
    auto group_error = parse_action({"queue", "unknown", "--help"});
    CHECK(group_error.usage.group == "queue");
    CHECK(group_error.usage.action.empty());
    auto leaf_error = parse_action({"queue", "get", "--name", "--help"});
    CHECK(leaf_error.usage.action == "get");

    // Values need not name a live object or satisfy domain validation merely to request help.
    auto help = parse_action({"queue", "get", "--id", "not-a-uuid", "--help"});
    REQUIRE(help.action);
    CHECK(std::holds_alternative<HelpCommand>(*help.action));
}

TEST_CASE("jobuctl globals and aliases preserve canonical remote requests", "[jobuctl][parse]")
{
    auto expected = parse({"--socket", "fixture.sock", "queue", "create", "reports", "--weight", "2"});
    REQUIRE(expected.command);
    for (auto const& args : std::vector<std::vector<std::string>>{
             {"queue", "add", "reports", "--weight", "2", "--socket", "fixture.sock"},
             {"queue", "--socket", "fixture.sock", "create", "reports", "--weight", "2"},
             {"--socket=fixture.sock", "queue", "add", "reports", "--weight=2"}
    }) {
        auto result = parse(args);
        REQUIRE(result.command);
        CHECK(result.command->method == "queue.create");
        CHECK(result.command->kind == expected.command->kind);
        CHECK(result.command->socket_path == expected.command->socket_path);
        auto const& actual    = std::get<CreateQueueRequest>(result.command->request);
        auto const& reference = std::get<CreateQueueRequest>(expected.command->request);
        CHECK(actual.name == reference.name);
        CHECK(actual.weight == reference.weight);
    }
    auto canonical = parse({"--socket",
                            "fixture.sock",
                            "job",
                            "create",
                            "--queue-name",
                            "reports",
                            "--type",
                            "http",
                            "--at",
                            "2030-01-01T00:00:00Z",
                            "--url",
                            "https://example.test"});
    auto alias     = parse({"job",
                            "add",
                            "--queue-name",
                            "reports",
                            "--type",
                            "http",
                            "--at",
                            "2030-01-01T00:00:00Z",
                            "--url",
                            "https://example.test",
                            "--socket",
                            "fixture.sock"});
    REQUIRE(canonical.command);
    REQUIRE(alias.command);
    CHECK(alias.command->method == "job.create");
    CHECK(alias.command->kind == canonical.command->kind);
    auto const& alias_request     = std::get<CreateJobRequest>(alias.command->request);
    auto const& canonical_request = std::get<CreateJobRequest>(canonical.command->request);
    CHECK(alias_request.queue == canonical_request.queue);
    CHECK(alias_request.type == canonical_request.type);
    CHECK(alias_request.payload == canonical_request.payload);

    CHECK_FALSE(parse_action({"queue", "list"}).action);
    CHECK_FALSE(parse_action({"--socket", "one", "queue", "list", "--socket", "two"}).action);
    auto version = parse_action({"--version"});
    REQUIRE(version.action);
    CHECK(std::holds_alternative<VersionCommand>(*version.action));
}

TEST_CASE("jobuctl help-looking option values and terminator operands remain data", "[jobuctl][parse]")
{
    for (auto const& suffix : std::vector<std::vector<std::string>>{
             {"--arg=--help"},
             {"--arg", "--help"},
             {"--arg", "-h"},
             {"--arg", "-ahb"},
             {"--arg", "--version"},
             {"--arg", "--json"},
             {"--arg", "--timeout"},
             {"--arg", "--request-file"},
             {"--arg", "--unknown"},
             {"--arg", "--env"}
    }) {
        CAPTURE(suffix);
        auto args = std::vector<std::string>{"job",
                                             "create",
                                             "--socket",
                                             "fixture.sock",
                                             "--queue-name",
                                             "reports",
                                             "--type",
                                             "cli",
                                             "--at",
                                             "2030-01-01T00:00:00Z",
                                             "--command",
                                             "/bin/true"};
        args.insert(args.end(), suffix.begin(), suffix.end());
        auto result = parse(args);
        REQUIRE(result.command);
        auto const& values =
            std::get<CreateJobRequest>(result.command->request).payload.as_object().at("arguments").as_array();
        REQUIRE(values.size() == 1);
        CHECK(values.front().as_string() == (suffix.size() == 1 ? "--help" : suffix.back()));
    }
    for (auto const& args : std::vector<std::vector<std::string>>{
             {"queue", "create", "--socket", "fixture.sock", "--", "--help"},
             {"queue", "create", "--socket", "fixture.sock", "--", "-h"    }
    }) {
        auto result = parse(args);
        REQUIRE(result.command);
        CHECK(std::get<CreateQueueRequest>(result.command->request).name == args.back());
    }
    auto name = parse({"--socket", "fixture.sock", "queue", "get", "--name=--help"});
    REQUIRE(name.command);
    CHECK(std::get<std::string>(std::get<QueueSelector>(name.command->request)) == "--help");
}

TEST_CASE("jobuctl queue configuration keeps inheritance and clear semantics", "[jobuctl][parse]")
{
    jb::test::TemporaryDirectory directory;
    auto const                   path = directory.path() / "defaults.json";
    {
        auto file = std::ofstream{path};
        REQUIRE(file);
        file << R"({"retry.max_attempts":2})";
    }

    auto created = parse({"--socket",
                          "fixture.sock",
                          "queue",
                          "create",
                          "reports",
                          "--defaults-file",
                          path.string(),
                          "--history-retention-seconds",
                          "0",
                          "--runnable-wait-warning-ms",
                          "250"});
    REQUIRE(created.command);
    auto const& create_request = std::get<CreateQueueRequest>(created.command->request);
    CHECK(create_request.defaults.contains("retry.max_attempts"));
    CHECK(create_request.history_retention == std::chrono::seconds{0});
    CHECK(create_request.runnable_wait_warning == std::chrono::milliseconds{250});

    {
        auto file = std::ofstream{path, std::ios::trunc};
        REQUIRE(file);
        file << "{}";
    }
    auto updated = parse({"--socket",
                          "fixture.sock",
                          "queue",
                          "update",
                          "--name",
                          "reports",
                          "--defaults-file",
                          path.string(),
                          "--inherit-history-retention",
                          "--recovery-policy",
                          "retry_interrupted"});
    REQUIRE(updated.command);
    auto const& update_request = std::get<UpdateQueueRequest>(updated.command->request);
    REQUIRE(update_request.defaults);
    CHECK(update_request.defaults->empty());
    REQUIRE(update_request.history_retention);
    CHECK_FALSE(*update_request.history_retention);
    CHECK(update_request.recovery_policy == RecoveryPolicy::RetryInterrupted);

    CHECK_FALSE(parse({"--socket",
                       "fixture.sock",
                       "queue",
                       "update",
                       "--name",
                       "reports",
                       "--history-retention-seconds",
                       "0",
                       "--inherit-history-retention"})
                    .command);
    CHECK_FALSE(
        parse({"--socket", "fixture.sock", "queue", "create", "reports", "--history-retention-seconds", "-1"}).command);
}

TEST_CASE("jobuctl job schedules and attribute patches use typed request semantics", "[jobuctl][parse]")
{
    auto const base = std::vector<std::string>{"--socket",
                                               "fixture.sock",
                                               "job",
                                               "create",
                                               "--queue-name",
                                               "reports",
                                               "--type",
                                               "cli",
                                               "--command",
                                               "/bin/true"};
    auto       with = [&](std::vector<std::string> suffix) {
        auto args = base;
        args.insert(args.end(), suffix.begin(), suffix.end());
        return parse(std::move(args));
    };

    auto immediate = with({"--now", "--attribute", "retry.max_attempts=2"});
    REQUIRE(immediate.command);
    auto const& create_request = std::get<CreateJobRequest>(immediate.command->request);
    CHECK(std::holds_alternative<ImmediateSchedule>(create_request.schedule));
    CHECK(create_request.attributes.contains("retry.max_attempts"));

    auto recurring = with({"--cron", "0 9 * * 1-5", "--timezone", "Europe/Tallinn"});
    REQUIRE(recurring.command);
    auto const& cron = std::get<CronSchedule>(std::get<CreateJobRequest>(recurring.command->request).schedule);
    CHECK(cron.expression == "0 9 * * 1-5");
    CHECK(cron.timezone == "Europe/Tallinn");

    auto updated = parse({"--socket",
                          "fixture.sock",
                          "job",
                          "update",
                          job_id,
                          "--revision",
                          "1",
                          "--cron",
                          "0 9 * * *",
                          "--attribute",
                          "retry.max_attempts=3"});
    REQUIRE(updated.command);
    auto const& update_request = std::get<UpdateJobRequest>(updated.command->request);
    CHECK(update_request.expected_revision == 1);
    CHECK(update_request.attribute_changes.contains("retry.max_attempts"));
    CHECK(std::holds_alternative<CronSchedule>(*update_request.schedule));

    for (auto const& suffix : std::vector<std::vector<std::string>>{
             {"--now", "--at", "2030-01-01T00:00:00Z"},
             {"--now", "--cron", "0 9 * * *"},
             {"--at", "2030-01-01T00:00:00Z", "--timezone", "UTC"},
             {"--now", "--attribute", "retry.max_attempts=null"},
             {"--now", "--attribute", "retry.max_attempts=2", "--attribute", "retry.max_attempts=3"}
    }) {
        CAPTURE(suffix);
        CHECK_FALSE(with(suffix).command);
    }
    CHECK_FALSE(parse({"--socket", "fixture.sock", "job", "update", job_id, "--revision", "1", "--now"}).command);
}

TEST_CASE("jobuctl wait is local to suspend and compatible with request files", "[jobuctl][parse]")
{
    auto queue = parse({"--socket", "fixture.sock", "queue", "suspend", "--name", "reports", "--wait"});
    REQUIRE(queue.command);
    CHECK(queue.command->wait);
    CHECK(std::holds_alternative<QueueSelector>(queue.command->request));

    auto file = parse({"--socket", "fixture.sock", "job", "suspend", "--request-file", "job.json", "--wait"});
    REQUIRE(file.command);
    CHECK(file.command->wait);
    CHECK(file.command->request_file == std::filesystem::path{"job.json"});
    CHECK_FALSE(parse({"--socket", "fixture.sock", "queue", "resume", "--name", "reports", "--wait"}).command);
}

TEST_CASE("jobuctl run commands preserve history filters and cursor-only continuations", "[jobuctl][parse]")
{
    auto now = parse({"--socket", "fixture.sock", "job", "run-now", job_id, "--idempotency-key", "manual-1"});
    REQUIRE(now.command);
    CHECK(now.command->method == "job.run_now");
    CHECK(std::get<RunNowRequest>(now.command->request).idempotency_key == "manual-1");

    auto listed = parse({"--socket",
                         "fixture.sock",
                         "run",
                         "list",
                         "--queue-id",
                         job_id,
                         "--state",
                         "failed",
                         "--origin",
                         "manual",
                         "--planned-from",
                         "2030-01-01T00:00:00Z",
                         "--planned-to",
                         "2030-02-01T00:00:00Z",
                         "--limit",
                         "20"});
    REQUIRE(listed.command);
    auto const& query = std::get<RunQuery>(std::get<RunListRequest>(listed.command->request));
    CHECK(query.limit == 20);
    CHECK(query.filters.queue_id == *Uuid::parse(job_id));
    CHECK(query.filters.state == RunState::Failed);
    CHECK(query.filters.origin == RunOrigin::Manual);
    CHECK(query.filters.planned.from.has_value());
    CHECK(query.filters.planned.to.has_value());

    auto cursor = parse({"--socket", "fixture.sock", "run", "list", "--cursor", "token"});
    REQUIRE(cursor.command);
    CHECK(std::get<CursorRequest>(std::get<RunListRequest>(cursor.command->request)).cursor == "token");
    CHECK_FALSE(parse({"--socket", "fixture.sock", "run", "list", "--cursor", "token", "--limit", "2"}).command);
    CHECK_FALSE(parse({"--socket", "fixture.sock", "run", "list", "--origin", "submitted"}).command);
    CHECK_FALSE(parse({"--socket",
                       "fixture.sock",
                       "run",
                       "list",
                       "--planned-from",
                       "2031-01-01T00:00:00Z",
                       "--planned-to",
                       "2030-01-01T00:00:00Z"})
                    .command);

    auto cancel = parse({"--socket", "fixture.sock", "run", "cancel", job_id, "--wait"});
    REQUIRE(cancel.command);
    CHECK(cancel.command->wait);
    CHECK(cancel.command->method == "run.cancel");
    CHECK_FALSE(parse({"--socket", "fixture.sock", "run", "get", job_id, "--wait"}).command);
}

TEST_CASE("jobuctl attempt commands keep output delivery separate from request params", "[jobuctl][parse]")
{
    auto listed = parse({"--socket", "fixture.sock", "attempt", "list", job_id, "--limit", "3"});
    REQUIRE(listed.command);
    auto const& query = std::get<AttemptQuery>(std::get<AttemptListRequest>(listed.command->request));
    CHECK(query.run_id == *Uuid::parse(job_id));
    CHECK(query.limit == 3);

    auto cursor = parse({"--socket", "fixture.sock", "attempt", "list", "--cursor", "next"});
    REQUIRE(cursor.command);
    CHECK(std::get<CursorRequest>(std::get<AttemptListRequest>(cursor.command->request)).cursor == "next");
    CHECK_FALSE(parse({"--socket", "fixture.sock", "attempt", "list", job_id, "--cursor", "next"}).command);

    auto output = parse({"--socket",
                         "fixture.sock",
                         "attempt",
                         "output",
                         job_id,
                         "2",
                         "--channel",
                         "stderr",
                         "--offset",
                         "4",
                         "--limit",
                         "16",
                         "--raw"});
    REQUIRE(output.command);
    CHECK(output.command->raw);
    auto const& request = std::get<AttemptOutputRequest>(output.command->request);
    CHECK(request.attempt.attempt_number == 2);
    CHECK(request.channel == OutputChannel::Stderr);
    CHECK(request.offset == 4);
    CHECK(request.limit == 16);

    CHECK_FALSE(parse({"--socket", "fixture.sock", "attempt", "output", job_id, "0", "--channel", "stdout"}).command);
    CHECK_FALSE(
        parse({"--socket", "fixture.sock", "attempt", "output", job_id, "1", "--channel", "stdout", "--raw", "--json"})
            .command);
    CHECK_FALSE(parse({"--socket",
                       "fixture.sock",
                       "attempt",
                       "output",
                       job_id,
                       "1",
                       "--channel",
                       "stdout",
                       "--raw",
                       "--output-file",
                       "out.bin"})
                    .command);

    auto file = parse(
        {"--socket", "fixture.sock", "attempt", "output", "--request-file", "params.json", "--output-file", "out.bin"});
    REQUIRE(file.command);
    CHECK(file.command->output_file == std::filesystem::path{"out.bin"});
}

TEST_CASE("jobuctl request files decode run and output methods with strict public codecs", "[jobuctl][input]")
{
    jb::test::TemporaryDirectory directory;
    StandardAttributeRegistry    registry;
    auto const                   path = directory.path() / "history.json";
    {
        auto file = std::ofstream{path, std::ios::binary};
        REQUIRE(file);
        file << R"({"job_id":"00000000-0000-7000-8000-000000000001","limit":2})";
    }
    auto runs = parse({"--socket", "fixture.sock", "run", "list", "--request-file", path.string()});
    REQUIRE(runs.command);
    REQUIRE(load_request_file(*runs.command, registry));
    auto const& query = std::get<RunQuery>(std::get<RunListRequest>(runs.command->request));
    CHECK(query.limit == 2);
    CHECK(query.filters.job_id == *Uuid::parse(job_id));

    {
        auto file = std::ofstream{path, std::ios::binary | std::ios::trunc};
        REQUIRE(file);
        file
            << R"({"run_id":"00000000-0000-7000-8000-000000000001","attempt_number":1,"channel":"body","offset":4,"limit":8})";
    }
    auto output = parse({"--socket", "fixture.sock", "attempt", "output", "--request-file", path.string(), "--raw"});
    REQUIRE(output.command);
    REQUIRE(load_request_file(*output.command, registry));
    CHECK(output.command->raw);
    auto const& request = std::get<AttemptOutputRequest>(output.command->request);
    CHECK(request.channel == OutputChannel::Body);
    CHECK(request.offset == 4);
    CHECK(request.limit == 8);

    {
        auto file = std::ofstream{path, std::ios::binary | std::ios::trunc};
        REQUIRE(file);
        file << R"({"cursor":"token","limit":2})";
    }
    auto invalid = load_request_file(*runs.command, registry);
    REQUIRE_FALSE(invalid);
    CHECK(invalid.error().code == "jobuctl.input.invalid_params");
}
