#include "attribute_registry.hpp"
#include "command_line_priv.hpp"
#include "json.hpp"
#include "management_json.hpp"
#include "uuid.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>
#include <string>
#include <variant> // IWYU pragma: keep for std::get in Catch assertions
#include <vector>

using namespace jb::core;
using namespace jb::jobu;
using namespace jb::jobuctl::detail;

namespace {

constexpr auto job_id = "00000000-0000-7000-8000-000000000001";

// Destroy the lexer inputs and registry before inspecting the result: parsed commands must own their data.
auto parse(std::vector<std::string> arguments) -> ParseResult
{
    arguments.insert(arguments.begin(), "jobuctl");
    auto argv = std::vector<char*>{};
    for (auto& argument : arguments) {
        argv.push_back(argument.data());
    }
    StandardAttributeRegistry registry;
    return parse_command_line(static_cast<int>(argv.size()), argv.data(), registry);
}

} // namespace

TEST_CASE("jobuctl parser retains socket selection and command family errors", "[jobuctl][parse]")
{
    auto parsed = parse({"--socket", "/tmp/custom.sock", "system", "info"});
    REQUIRE(parsed.command);
    CHECK(parsed.command->socket_path == "/tmp/custom.sock");
    CHECK(parsed.command->method == "system.info");
    CHECK_FALSE(parsed.command->params);

    CHECK(parse({"system", "info", "--socket", "/tmp/custom.sock"}).error ==
          "--socket PATH must be the first argument");
    CHECK(parse({"--socket", "/tmp/custom.sock", "system", "info", "extra"}).error == "unknown command");
    CHECK(parse({"--socket", "/tmp/custom.sock", "queue", "unknown"}).error == "unknown queue action");
    CHECK(parse({"--socket", "/tmp/custom.sock", "job", "unknown"}).error == "unknown job action");
    CHECK(parse({"--socket", "/tmp/custom.sock", "queue", "list", "--socket", "/tmp/other.sock"}).error ==
          "queue list has an unknown or duplicate option");
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
