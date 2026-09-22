#include "attribute_registry.hpp"
#include "command_line_priv.hpp"
#include "command_registry_priv.hpp"
#include "help_priv.hpp"
#include "json.hpp"
#include "management_json.hpp"
#include "uuid.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>
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
    CHECK_FALSE(parsed.command->params);

    CHECK(parse({"system", "info", "--socket", "/tmp/custom.sock"}).command.has_value());
    CHECK(parse({"--socket", "/tmp/custom.sock", "system", "info", "extra"}).error == "unexpected extra operand");
    CHECK(parse({"--socket", "/tmp/custom.sock", "queue", "unknown"}).error == "unknown command action");
    CHECK(parse({"--socket", "/tmp/custom.sock", "job", "unknown"}).error == "unknown command action");
    CHECK(parse({"--socket", "/tmp/custom.sock", "queue", "list", "--socket", "/tmp/other.sock"}).error ==
          "--socket may be supplied only once");
}

TEST_CASE("jobuctl parser preserves queue selectors and deletion rendering identity", "[jobuctl][parse]")
{
    for (auto const* action : {"get", "suspend", "resume", "delete"}) {
        CAPTURE(action);
        auto named = parse({"--socket", "fixture.sock", "queue", action, "--name", "queue with spaces"});
        REQUIRE(named.command);
        REQUIRE(named.command->params);
        auto selector = queue_selector_from_json(*named.command->params);
        REQUIRE(selector);
        CHECK(std::get<std::string>(*selector) == "queue with spaces");
        CHECK(named.command->selector == *selector);
        CHECK(named.command->method == std::string{"queue."} + action);

        auto identified = parse({"--socket", "fixture.sock", "queue", action, "--id", job_id});
        REQUIRE(identified.command);
        REQUIRE(identified.command->selector);
        CHECK(std::get<Uuid>(*identified.command->selector).to_string() == job_id);
    }

    auto duplicate = parse({"--socket", "fixture.sock", "queue", "get", "--id", job_id, "--name", "queue"});
    CHECK_FALSE(duplicate.command);
    CHECK(duplicate.error == "expected exactly one valid --id or --name selector");
}

TEST_CASE("jobuctl parser preserves job identity revisions and signed priority", "[jobuctl][parse]")
{
    StandardAttributeRegistry registry;
    auto                      parsed = parse({"--socket",
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
    REQUIRE(parsed.command->params);
    auto request = update_job_request_from_json(*parsed.command->params, registry);
    REQUIRE(request);
    CHECK(request->job_id.to_string() == job_id);
    CHECK(request->expected_revision == std::numeric_limits<std::uint64_t>::max());
    CHECK(request->priority == std::numeric_limits<std::int32_t>::min());
    REQUIRE(request->name);
    CHECK_FALSE(*request->name);

    for (auto const* revision : {"0", "-1", "18446744073709551616"}) {
        CAPTURE(revision);
        auto invalid = parse({"--socket", "fixture.sock", "job", "delete", job_id, "--revision", revision});
        CHECK_FALSE(invalid.command);
    }
    for (auto const* action : {"get", "suspend", "resume"}) {
        auto selected = parse({"--socket", "fixture.sock", "job", action, job_id});
        REQUIRE(selected.command);
        CHECK(selected.command->method == std::string{"job."} + action);
        REQUIRE(selected.command->job_id);
        CHECK(selected.command->job_id->to_string() == job_id);
    }

    auto deleted = parse({"--socket", "fixture.sock", "job", "delete", job_id, "--revision", "7"});
    REQUIRE(deleted.command);
    REQUIRE(deleted.command->params);
    auto deletion = delete_job_request_from_json(*deleted.command->params);
    REQUIRE(deletion);
    CHECK(deletion->expected_revision == 7);
    REQUIRE(deleted.command->job_id);
    CHECK(deleted.command->job_id->to_string() == job_id);
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
    REQUIRE(parsed.command->params);
    StandardAttributeRegistry registry;
    auto                      request = create_job_request_from_json(*parsed.command->params, registry);
    REQUIRE(request);
    CHECK(request->priority == -12);
    auto expected = parse_json(
        R"({"command":"/bin/true","arguments":["","-abc","--unknown","--env","--env=NAME=value","--command","--help",""],"environment":{"ACTUAL":"value"}})");
    REQUIRE(expected);
    CHECK(request->payload == *expected);
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
        CHECK(result.command->params == expected.command->params);
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
    CHECK(alias.command->params == canonical.command->params);

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
        auto const& values = result.command->params->as_object().at("payload").as_object().at("arguments").as_array();
        REQUIRE(values.size() == 1);
        CHECK(values.front().as_string() == (suffix.size() == 1 ? "--help" : suffix.back()));
    }
    for (auto const& args : std::vector<std::vector<std::string>>{
             {"queue", "create", "--socket", "fixture.sock", "--", "--help"},
             {"queue", "create", "--socket", "fixture.sock", "--", "-h"    }
    }) {
        auto result = parse(args);
        REQUIRE(result.command);
        CHECK(result.command->params->as_object().at("name").as_string() == args.back());
    }
    auto name = parse({"--socket", "fixture.sock", "queue", "get", "--name=--help"});
    REQUIRE(name.command);
    CHECK(std::get<std::string>(*name.command->selector) == "--help");
}
