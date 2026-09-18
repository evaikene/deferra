#include "application.hpp"
#include "byte_buffer.hpp"
#include "json.hpp"
#include "local_server.hpp"
#include "process.hpp"
#include "protocol.hpp"
#include "server.hpp"
#include "support/temporary_directory.hpp"
#include "system_info.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace jb::core;
using namespace jb::jobu;
using namespace jb::net;
using namespace jb::rpc;

namespace {

struct ClientExchange {
    std::size_t                connections{0};
    std::vector<std::string>   methods;
    std::optional<JsonValue>   params;
    std::string                output;
    std::optional<ProcessExit> exit;
};

/// Runs the real client against a local RPC peer; completion is driven by process and socket readiness.
/// @throws Catch::TestFailureException when fixture setup or the bounded child watchdog fails.
auto run_client(std::vector<std::string> arguments,
                SystemInfo               info = {
                    .daemon_version = "fixture",
                    .api_version    = {.major = 1,   .minor = 2   },
                    .capabilities   = {"job.create", "system.info"}
}) -> ClientExchange
{
    ClientExchange               exchange;
    Application                  app{0, nullptr};
    jb::test::TemporaryDirectory directory;
    LocalServer                  listener;
    Server                       server;
    Process                      client;

    auto accepted = listener.new_connection.connect(&server, [&] {
        while (auto socket = listener.take_next_connection()) {
            ++exchange.connections;
            REQUIRE(server.add_connection(std::move(socket)));
        }
    });
    REQUIRE(server.register_method("system.info", [&](RequestContext const&, std::optional<JsonValue> const&) {
        exchange.methods.emplace_back("system.info");
        return MethodResult::success(system_info_to_json(info));
    }));
    REQUIRE(server.register_method("job.create", [&](RequestContext const&, std::optional<JsonValue> const& params) {
        exchange.methods.emplace_back("job.create");
        exchange.params = params;
        // A controlled remote rejection proves the request arrived and the client reports the server's decision.
        return MethodResult::failure(
            {.code = static_cast<std::int64_t>(ErrorCode::InvalidParams), .message = "fixture job.create reached"});
    }));
    auto collect  = [&](ByteBuffer const& bytes) { exchange.output.append(as_string_view(bytes)); };
    auto output   = client.standard_output.connect(&app, collect);
    auto error    = client.standard_error.connect(&app, collect);
    auto finished = client.finished.connect(&app, [&](ProcessExit const& exit) {
        exchange.exit = exit;
        REQUIRE(app.quit(EXIT_SUCCESS));
    });

    auto const socket_path = directory.path() / "jobuctl.sock";
    REQUIRE(listener.listen(socket_path));
    arguments.insert(arguments.begin(), {"--socket", socket_path.string()});
    REQUIRE(client.start({.executable        = JOBUCTL_EXECUTABLE,
                          .arguments         = std::move(arguments),
                          .timeout           = std::chrono::seconds{5},
                          .termination_grace = std::chrono::milliseconds{0}}));
    REQUIRE(app.exec() == EXIT_SUCCESS);
    REQUIRE(exchange.exit);
    INFO(exchange.output);
    REQUIRE(exchange.exit->kind == ProcessExitKind::Exited);
    REQUIRE(exchange.exit->exit_code == EXIT_FAILURE);
    REQUIRE_FALSE(exchange.exit->stdout_lost);
    REQUIRE_FALSE(exchange.exit->stderr_lost);
    return exchange;
}

auto cli_create(std::vector<std::string> options = {}, std::string command = "/bin/true") -> std::vector<std::string>
{
    auto arguments = std::vector<std::string>{"job",
                                              "create",
                                              "--queue-name",
                                              "fixture",
                                              "--type",
                                              "cli",
                                              "--at",
                                              "2030-01-01T00:00:00Z",
                                              "--command",
                                              std::move(command)};
    arguments.insert(arguments.end(), options.begin(), options.end());
    return arguments;
}

/// @throws Catch::TestFailureException when the client does not send exactly the expected request.
void check_request(ClientExchange const& exchange, std::string_view payload)
{
    INFO(exchange.output);
    REQUIRE(exchange.connections == 1);
    REQUIRE(exchange.methods == std::vector<std::string>{"system.info", "job.create"});
    REQUIRE(exchange.params);
    auto expected = parse_json(
        R"({"attributes":{},"payload":)" + std::string{payload} +
        R"(,"priority":0,"queue_name":"fixture","schedule":{"at":"2030-01-01T00:00:00.000000Z","kind":"once"},"type":"cli"})");
    REQUIRE(expected);
    CHECK(*exchange.params == *expected);
    CHECK(exchange.output.find("fixture job.create reached") != std::string::npos);
    CHECK(exchange.output.find("Usage:\n") == std::string::npos);
}

/// @throws Catch::TestFailureException when the client contacts the peer or fails without a local usage error.
void check_local_rejection(ClientExchange const& exchange)
{
    INFO(exchange.output);
    CHECK(exchange.connections == 0);
    CHECK(exchange.methods.empty());
    CHECK_FALSE(exchange.params);
    CHECK(exchange.output.find("Usage:\n") != std::string::npos);
}

} // anonymous namespace

TEST_CASE("jobuctl sends CLI creation fields through job.create on older API minors", "[jobuctl][cli]")
{
    for (auto minor : {0U, 1U, 2U}) {
        CAPTURE(minor);
        auto exchange = run_client(cli_create(
                                       {
                                           "--working-directory",
                                           "/tmp",
                                           "--env",
                                           "PATH=/bin:/usr/bin",
                                           "--env",
                                           "EMPTY=",
                                           "--env",
                                           "VALUE=left=right",
                                           "--unset-env",
                                           "OLD",
                                           "--unset-env",
                                           "OTHER",
                                           "--expected-exit-code",
                                           "255",
                                           "--expected-exit-code",
                                           "0"
        },
                                       "true"),
                                   {.daemon_version = "fixture",
                                    .api_version    = {.major = 1, .minor = minor},
                                    .capabilities   = {"job.create", "system.info"}});
        check_request(
            exchange,
            R"({"command":"true","working_directory":"/tmp","environment":{"PATH":"/bin:/usr/bin","EMPTY":"","VALUE":"left=right","OLD":null,"OTHER":null},"expected_exit_codes":[255,0]})");
    }
}

TEST_CASE("jobuctl omits unsupplied CLI fields and preserves explicit defaults", "[jobuctl][cli]")
{
    check_request(run_client(cli_create()), R"({"command":"/bin/true"})");
    check_request(run_client(cli_create({"--working-directory=/"})),
                  R"({"command":"/bin/true","working_directory":"/"})");
    check_request(run_client(cli_create({"--env=EMPTY="})), R"({"command":"/bin/true","environment":{"EMPTY":""}})");
    check_request(run_client(cli_create({"--unset-env=ABSENT"})),
                  R"({"command":"/bin/true","environment":{"ABSENT":null}})");
    check_request(run_client(cli_create({"--expected-exit-code=0"})),
                  R"({"command":"/bin/true","expected_exit_codes":[0]})");
}

TEST_CASE("jobuctl preserves literal CLI arguments after registering new option names", "[jobuctl][cli]")
{
    auto exchange = run_client(cli_create({"--arg",           "",
                                           "--arg",           "-abc",
                                           "--arg",           "--unknown",
                                           "--arg",           "--env",
                                           "--arg",           "--env=NAME=value",
                                           "--arg",           "--unset-env",
                                           "--arg",           "--working-directory",
                                           "--arg",           "--expected-exit-code",
                                           "--arg=--command", "--arg=",
                                           "--env",           "ACTUAL=value"}));
    check_request(
        exchange,
        R"({"command":"/bin/true","arguments":["","-abc","--unknown","--env","--env=NAME=value","--unset-env","--working-directory","--expected-exit-code","--command",""],"environment":{"ACTUAL":"value"}})");
}

TEST_CASE("jobuctl rejects invalid CLI creation options before connecting", "[jobuctl][cli]")
{
    auto const invalid_options = std::vector<std::vector<std::string>>{
        {"--working-directory", "/", "--working-directory", "/tmp"},
        {"--working-directory", "relative"},
        {"--working-directory="},
        {"--working-directory"},
        {"--env", "NAME"},
        {"--env", "=value"},
        {"--env", "BAD-NAME=value"},
        {"--env", "9NAME=value"},
        {"--env", "JOBU_JOB_ID=value"},
        {"--env", "JOBU_RUN_ID=value"},
        {"--env", "JOBU_ATTEMPT=value"},
        {"--env", "NAME=one", "--env", "NAME=two"},
        {"--env="},
        {"--env"},
        {"--unset-env", "NAME", "--unset-env", "NAME"},
        {"--unset-env", "BAD-NAME"},
        {"--unset-env", "NAME=value"},
        {"--unset-env", "JOBU_JOB_ID"},
        {"--unset-env", "JOBU_RUN_ID"},
        {"--unset-env", "JOBU_ATTEMPT"},
        {"--unset-env="},
        {"--unset-env"},
        {"--env", "NAME=value", "--unset-env", "NAME"},
        {"--unset-env", "NAME", "--env", "NAME="},
        {"--expected-exit-code", "0", "--expected-exit-code", "00"},
        {"--expected-exit-code", "256"},
        {"--expected-exit-code=-1"},
        {"--expected-exit-code", "+1"},
        {"--expected-exit-code", "1x"},
        {"--expected-exit-code", "1.0"},
        {"--expected-exit-code", "18446744073709551616"},
        {"--expected-exit-code="},
        {"--expected-exit-code"},
        {"--url", "http://example.test"},
        {"--method", "GET"},
        {"--arg"},
        {"--unknown-option", "value"},
        {"--working-directory", "/" + std::string(4096, 'x')},
    };
    for (auto const& options : invalid_options) {
        CAPTURE(options);
        check_local_rejection(run_client(cli_create(options)));
    }
}

TEST_CASE("jobuctl validates commands and explicit PATH before connecting", "[jobuctl][cli]")
{
    for (auto const& command : {"", "./relative", "relative/tool"}) {
        CAPTURE(command);
        check_local_rejection(run_client(cli_create({}, command)));
    }
    for (auto const& options : std::vector<std::vector<std::string>>{
             {},
             {"--unset-env", "PATH"},
             {"--env", "PATH="},
             {"--env", "PATH=relative"},
             {"--env", "PATH=/bin:"},
             {"--env", "PATH=:/bin"},
             {"--env", "PATH=/bin::/usr/bin"}
    }) {
        CAPTURE(options);
        check_local_rejection(run_client(cli_create(options, "true")));
    }
}

TEST_CASE("jobuctl rejects CLI-only options for HTTP creation", "[jobuctl][cli]")
{
    for (auto const& options : std::vector<std::vector<std::string>>{
             {"--working-directory",  "/"        },
             {"--env",                "NAME="    },
             {"--unset-env",          "NAME"     },
             {"--expected-exit-code", "0"        },
             {"--command",            "/bin/true"},
             {"--arg",                ""         }
    }) {
        CAPTURE(options);
        auto arguments = std::vector<std::string>{"job",
                                                  "create",
                                                  "--queue-name",
                                                  "fixture",
                                                  "--type",
                                                  "http",
                                                  "--at",
                                                  "2030-01-01T00:00:00Z",
                                                  "--url",
                                                  "http://example.test"};
        arguments.insert(arguments.end(), options.begin(), options.end());
        check_local_rejection(run_client(std::move(arguments)));
    }
}

TEST_CASE("jobuctl preserves major-version and capability checks for CLI creation", "[jobuctl][cli]")
{
    for (auto const& info : std::vector<SystemInfo>{
             {.daemon_version = "fixture",
              .api_version    = {.major = 2, .minor = 0},
              .capabilities   = {"job.create", "system.info"}                                                      },
             {.daemon_version = "fixture", .api_version = {.major = 1, .minor = 0}, .capabilities = {"system.info"}}
    }) {
        auto exchange = run_client(cli_create({"--env", "NAME=value"}), info);
        CHECK(exchange.methods == std::vector<std::string>{"system.info"});
        CHECK_FALSE(exchange.params);
        CHECK(exchange.output.find("Usage:\n") == std::string::npos);
    }
}
