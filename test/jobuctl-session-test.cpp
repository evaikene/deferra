#include "application.hpp"
#include "byte_buffer.hpp"
#include "framing.hpp"
#include "json.hpp"
#include "local_server.hpp"
#include "local_socket.hpp"
#include "process.hpp"
#include "support/temporary_directory.hpp"
#include "system_info.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace jb::core;
using namespace jb::jobu;
using namespace jb::net;
using namespace jb::rpc;

namespace {

enum class PeerBehavior : std::uint8_t {
    EmptyPage,
    RemoteError,
    InvalidInfo,
    InvalidResult,
    SilentHandshake,
    SilentCommand
};

struct Exchange {
    std::vector<std::string>   methods;
    std::string                output;
    std::string                error;
    std::optional<ProcessExit> exit;
};

/// Runs the executable against a scripted peer, driven entirely by socket/process readiness.
/// @throws Catch::TestFailureException when setup, framing, or the child watchdog fails.
auto run_session(PeerBehavior behavior) -> Exchange
{
    Exchange                     exchange;
    Application                  app{0, nullptr};
    jb::test::TemporaryDirectory directory;
    LocalServer                  listener;
    std::unique_ptr<LocalSocket> peer;
    StreamFramer                 framer;
    Process                      child;

    // The peer can deliberately withhold either response without blocking the owner event loop.
    auto read_requests = [&] {
        auto bodies = framer.append(peer->read_all());
        REQUIRE(bodies);
        for (auto const& body : *bodies) {
            auto request = parse_json(body);
            REQUIRE(request);
            auto const& fields = request->as_object();
            auto const& method = fields.at("method").as_string();
            exchange.methods.push_back(method);
            if (behavior == PeerBehavior::SilentHandshake ||
                (behavior == PeerBehavior::SilentCommand && method == "queue.list")) {
                continue;
            }

            auto response = JsonValue::Object{
                {"jsonrpc", JsonValue{.data = std::string{"2.0"}}},
                {"id",      fields.at("id")                      }
            };
            if (method == "system.info") {
                auto info = system_info_to_json({
                    .daemon_version = "fixture",
                    .api_version    = {.major = 1,   .minor = 2   },
                    .capabilities   = {"queue.list", "system.info"}
                });
                response.emplace("result", behavior == PeerBehavior::InvalidInfo ? JsonValue{} : std::move(info));
            }
            else if (behavior == PeerBehavior::RemoteError) {
                auto error = parse_json(
                    R"({"code":-32000,"message":"fixture rejection","data":{"category":"conflict","code":"jobu.fixture.conflict"}})");
                REQUIRE(error);
                response.emplace("error", std::move(*error));
            }
            else {
                auto page = parse_json(R"({"items":[],"next_after_id":null})");
                REQUIRE(page);
                response.emplace("result", behavior == PeerBehavior::InvalidResult ? JsonValue{} : std::move(*page));
            }

            auto encoded = serialize_json(JsonValue{.data = std::move(response)});
            REQUIRE(encoded);
            auto frame = frame_message(*encoded);
            REQUIRE(frame);
            REQUIRE(peer->write(*frame) == frame->size());
        }
    };
    listener.new_connection.connect(&app, [&] {
        REQUIRE_FALSE(peer);
        peer = listener.take_next_connection();
        REQUIRE(peer);
        peer->ready_read.connect(&app, read_requests);
        read_requests();
    });
    child.standard_output.connect(&app,
                                  [&](ByteBuffer const& bytes) { exchange.output.append(as_string_view(bytes)); });
    child.standard_error.connect(&app, [&](ByteBuffer const& bytes) { exchange.error.append(as_string_view(bytes)); });
    child.finished.connect(&app, [&](ProcessExit const& exit) {
        exchange.exit = exit;
        REQUIRE(app.quit(EXIT_SUCCESS));
    });

    auto const socket_path = directory.path() / "custom.sock";
    REQUIRE(listener.listen(socket_path));
    REQUIRE(child.start({
        .executable        = JOBUCTL_EXECUTABLE,
        .arguments         = {"--socket", socket_path.string(), "queue", "list"},
        .timeout           = std::chrono::seconds{15},
        .termination_grace = std::chrono::milliseconds{0}
    }));
    REQUIRE(app.exec() == EXIT_SUCCESS);
    REQUIRE(exchange.exit);
    INFO(exchange.error);
    REQUIRE(exchange.exit->kind == ProcessExitKind::Exited);
    REQUIRE_FALSE(exchange.exit->stdout_lost);
    REQUIRE_FALSE(exchange.exit->stderr_lost);
    return exchange;
}

} // namespace

TEST_CASE("jobuctl session preserves handshake ordering and human output", "[jobuctl][session]")
{
    auto exchange = run_session(PeerBehavior::EmptyPage);
    CHECK(exchange.methods == std::vector<std::string>{"system.info", "queue.list"});
    CHECK(exchange.exit->exit_code == EXIT_SUCCESS);
    CHECK(exchange.output == "No queues\n");
    CHECK(exchange.error.empty());
}

TEST_CASE("jobuctl session preserves represented remote errors", "[jobuctl][session]")
{
    auto exchange = run_session(PeerBehavior::RemoteError);
    CHECK(exchange.methods == std::vector<std::string>{"system.info", "queue.list"});
    CHECK(exchange.exit->exit_code == EXIT_FAILURE);
    CHECK(exchange.output.empty());
    CHECK(exchange.error == "jobuctl: fixture rejection (jobu.fixture.conflict)\n");
}

TEST_CASE("jobuctl session rejects malformed handshake and command results", "[jobuctl][session]")
{
    SECTION("handshake rejection prevents command submission")
    {
        auto exchange = run_session(PeerBehavior::InvalidInfo);
        CHECK(exchange.methods == std::vector<std::string>{"system.info"});
        CHECK(exchange.exit->exit_code == EXIT_FAILURE);
        CHECK(exchange.output.empty());
        CHECK(exchange.error.find("Invalid system.info response") != std::string::npos);
    }
    SECTION("invalid command result is not printed")
    {
        auto exchange = run_session(PeerBehavior::InvalidResult);
        CHECK(exchange.methods == std::vector<std::string>{"system.info", "queue.list"});
        CHECK(exchange.exit->exit_code == EXIT_FAILURE);
        CHECK(exchange.output.empty());
        CHECK(exchange.error.find("Invalid queue.list response") != std::string::npos);
    }
}

TEST_CASE("jobuctl session deadline identifies the pending operation", "[jobuctl][session]")
{
    // Exercise the real five-second deadline, with a longer child watchdog and no timing-window assertions.
    SECTION("handshake timeout")
    {
        auto exchange = run_session(PeerBehavior::SilentHandshake);
        CHECK(exchange.methods == std::vector<std::string>{"system.info"});
        CHECK(exchange.exit->exit_code == EXIT_FAILURE);
        CHECK(exchange.output.empty());
        CHECK(exchange.error == "jobuctl: system.info request timed out\n");
    }
    SECTION("command timeout")
    {
        auto exchange = run_session(PeerBehavior::SilentCommand);
        CHECK(exchange.methods == std::vector<std::string>{"system.info", "queue.list"});
        CHECK(exchange.exit->exit_code == EXIT_FAILURE);
        CHECK(exchange.output.empty());
        CHECK(exchange.error == "jobuctl: queue.list request timed out\n");
    }
}
