#include "application.hpp"
#include "attribute_registry.hpp"
#include "byte_buffer.hpp"
#include "framing.hpp"
#include "json.hpp"
#include "local_server.hpp"
#include "local_socket.hpp"
#include "management_json.hpp"
#include "process.hpp"
#include "support/temporary_directory.hpp"
#include "system_info.hpp"
#include "utc_timestamp.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <limits>
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
    MissingCapability,
    ControlInfo,
    LargeRevision,
    QueueSuspendPolling,
    JobSuspendPolling,
    SuspendNeverSettles,
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
auto run_session(PeerBehavior             behavior,
                 std::vector<std::string> command        = {"queue", "list"},
                 std::vector<std::string> global_options = {}) -> Exchange
{
    Exchange                     exchange;
    Application                  app{0, nullptr};
    jb::test::TemporaryDirectory directory;
    LocalServer                  listener;
    std::unique_ptr<LocalSocket> peer;
    StreamFramer                 framer;
    Process                      child;
    std::size_t                  poll_count{0};

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
                (behavior == PeerBehavior::SilentCommand && method != "system.info")) {
                continue;
            }

            auto response = JsonValue::Object{
                {"jsonrpc", JsonValue{.data = std::string{"2.0"}}},
                {"id",      fields.at("id")                      }
            };
            if (method == "system.info") {
                auto info = system_info_to_json({
                    .daemon_version = behavior == PeerBehavior::ControlInfo ? "fixture\x1b[31m" : "fixture",
                    .api_version    = {.major = 1, .minor = 2},
                    .capabilities   = behavior == PeerBehavior::MissingCapability
                                        ? std::vector<std::string>{"system.info"}
                                        : std::vector<std::string>{"job.get",
                                       "job.suspend", "queue.create",
                                       "queue.delete", "queue.get",
                                       "queue.suspend", "queue.list",
                                       "system.info"}
                });
                response.emplace("result", behavior == PeerBehavior::InvalidInfo ? JsonValue{} : std::move(info));
            }
            else if (behavior == PeerBehavior::RemoteError) {
                auto error = parse_json(
                    R"({"code":-32000,"message":"fixture rejection","data":{"category":"conflict","code":"jobu.fixture.conflict"}})");
                REQUIRE(error);
                response.emplace("error", std::move(*error));
            }
            else if (method == "queue.delete") {
                response.emplace("result", JsonValue{});
            }
            else if (method == "queue.suspend" || method == "queue.get") {
                auto id = Uuid::parse("00112233-4455-6677-8899-aabbccddeeff");
                auto at = parse_utc_timestamp("2030-01-01T00:00:00Z");
                REQUIRE(id);
                REQUIRE(at);
                auto queue = Queue{.id         = *id,
                                   .name       = "reports",
                                   .state      = QueueState::Suspending,
                                   .created_at = *at,
                                   .updated_at = *at};
                if (method == "queue.get" && behavior != PeerBehavior::SuspendNeverSettles && ++poll_count == 2) {
                    queue.state = QueueState::Suspended;
                }
                auto encoded = queue_to_json(queue, StandardAttributeRegistry{});
                REQUIRE(encoded);
                response.emplace("result", std::move(*encoded));
            }
            else if (method == "job.suspend" || (method == "job.get" && behavior == PeerBehavior::JobSuspendPolling)) {
                StandardAttributeRegistry registry;
                auto                      id         = Uuid::parse("00112233-4455-6677-8899-aabbccddeeff");
                auto                      at         = parse_utc_timestamp("2030-01-01T00:00:00Z");
                auto                      attributes = materialize_attributes(registry, {}, {}, {});
                auto                      payload    = parse_json(R"({"command":"/bin/true"})");
                REQUIRE(id);
                REQUIRE(at);
                REQUIRE(attributes);
                REQUIRE(payload);
                auto job = JobDefinition{.id         = *id,
                                         .queue_id   = *id,
                                         .state      = JobState::Suspending,
                                         .schedule   = OnceSchedule{.planned_at = *at},
                                         .attributes = *attributes,
                                         .payload    = *payload,
                                         .created_at = *at,
                                         .updated_at = *at};
                if (method == "job.get" && ++poll_count == 2) {
                    job.state = JobState::Suspended;
                }
                auto encoded = job_to_json(job, registry);
                REQUIRE(encoded);
                response.emplace("result", std::move(*encoded));
            }
            else if (method == "job.get" && behavior == PeerBehavior::LargeRevision) {
                StandardAttributeRegistry registry;
                auto                      id         = Uuid::parse("00112233-4455-6677-8899-aabbccddeeff");
                auto                      at         = parse_utc_timestamp("2030-01-01T00:00:00Z");
                auto                      attributes = materialize_attributes(registry, {}, {}, {});
                auto                      payload    = parse_json(R"({"command":"/bin/true"})");
                REQUIRE(id);
                REQUIRE(at);
                REQUIRE(attributes);
                REQUIRE(payload);
                auto job = JobDefinition{
                    .id         = *id,
                    .queue_id   = *id,
                    .revision   = std::numeric_limits<std::uint64_t>::max(),
                    .schedule   = OnceSchedule{.planned_at = *at},
                    .attributes = *attributes,
                    .payload    = *payload,
                    .created_at = *at,
                    .updated_at = *at,
                };
                auto encoded_job = job_to_json(job, registry);
                REQUIRE(encoded_job);
                response.emplace("result", std::move(*encoded_job));
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
    auto arguments = std::vector<std::string>{"--socket", socket_path.string()};
    arguments.insert(arguments.end(), global_options.begin(), global_options.end());
    arguments.insert(arguments.end(), command.begin(), command.end());
    REQUIRE(child.start({.executable        = JOBUCTL_EXECUTABLE,
                         .arguments         = std::move(arguments),
                         .timeout           = std::chrono::seconds{15},
                         .termination_grace = std::chrono::milliseconds{0}}));
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

TEST_CASE("jobuctl JSON mode emits one typed result or structured error", "[jobuctl][session]")
{
    SECTION("successful page")
    {
        auto exchange = run_session(PeerBehavior::EmptyPage, {"queue", "list"}, {"--json"});
        CHECK(exchange.exit->exit_code == 0);
        CHECK(exchange.output == "{\"items\":[],\"next_after_id\":null}\n");
        CHECK(exchange.error.empty());
    }
    SECTION("remote application error")
    {
        auto exchange = run_session(PeerBehavior::RemoteError, {"queue", "list"}, {"--json"});
        CHECK(exchange.exit->exit_code == 1);
        CHECK(exchange.output.empty());
        auto value = parse_json(exchange.error);
        REQUIRE(value);
        auto const& error = value->as_object().at("error").as_object();
        CHECK(error.at("kind").as_string() == "remote");
        CHECK(error.at("code").as_string() == "jobu.fixture.conflict");
        CHECK(error.at("rpc_code").as_int() == -32000);
        CHECK(error.at("category").as_string() == "conflict");
        CHECK(error.at("message").as_string() == "fixture rejection");
        CHECK_FALSE(error.at("outcome_unknown").as_bool());
    }
    SECTION("system info reuses the handshake")
    {
        auto exchange = run_session(PeerBehavior::EmptyPage, {"system", "info"}, {"--json"});
        CHECK(exchange.methods == std::vector<std::string>{"system.info"});
        CHECK(exchange.exit->exit_code == 0);
        CHECK(exchange.error.empty());
        auto value = parse_json(exchange.output);
        REQUIRE(value);
        CHECK(value->as_object().at("daemon_version").as_string() == "fixture");
    }
    SECTION("successful null reply")
    {
        auto exchange = run_session(PeerBehavior::EmptyPage, {"queue", "delete", "--name", "reports"}, {"--json"});
        CHECK(exchange.exit->exit_code == 0);
        CHECK(exchange.output == "null\n");
        CHECK(exchange.error.empty());
    }
    SECTION("large unsigned revision remains an integer")
    {
        auto exchange = run_session(PeerBehavior::LargeRevision,
                                    {"job", "get", "00112233-4455-6677-8899-aabbccddeeff"},
                                    {"--json"});
        CHECK(exchange.exit->exit_code == 0);
        CHECK(exchange.error.empty());
        auto value = parse_json(exchange.output);
        REQUIRE(value);
        CHECK(value->as_object().at("revision").as_uint() == std::numeric_limits<std::uint64_t>::max());
    }
}

TEST_CASE("jobuctl handles missing capability before sending the command", "[jobuctl][session]")
{
    auto exchange = run_session(PeerBehavior::MissingCapability, {"queue", "list"}, {"--json"});
    CHECK(exchange.methods == std::vector<std::string>{"system.info"});
    CHECK(exchange.exit->exit_code == 1);
    CHECK(exchange.output.empty());
    auto value = parse_json(exchange.error);
    REQUIRE(value);
    CHECK(value->as_object().at("error").as_object().at("code").as_string() == "jobu.client.unsupported_method");
}

TEST_CASE("jobuctl human output escapes daemon control characters", "[jobuctl][session]")
{
    auto exchange = run_session(PeerBehavior::ControlInfo, {"system", "info"});
    CHECK(exchange.exit->exit_code == 0);
    CHECK(exchange.output.find("Daemon version: fixture\\x1B[31m\n") == 0);
    CHECK(exchange.output.find('\x1b') == std::string::npos);
}

TEST_CASE("jobuctl session rejects malformed handshake and command results", "[jobuctl][session]")
{
    SECTION("handshake rejection prevents command submission")
    {
        auto exchange = run_session(PeerBehavior::InvalidInfo);
        CHECK(exchange.methods == std::vector<std::string>{"system.info"});
        CHECK(exchange.exit->exit_code == 3);
        CHECK(exchange.output.empty());
        CHECK(exchange.error.find("JobU response is invalid") != std::string::npos);
    }
    SECTION("invalid command result is not printed")
    {
        auto exchange = run_session(PeerBehavior::InvalidResult);
        CHECK(exchange.methods == std::vector<std::string>{"system.info", "queue.list"});
        CHECK(exchange.exit->exit_code == 3);
        CHECK(exchange.output.empty());
        CHECK(exchange.error.find("JobU response is invalid") != std::string::npos);
    }
}

TEST_CASE("jobuctl session enforces one overall command deadline", "[jobuctl][session]")
{
    // A silent peer leaves the selected phase pending; the short configured deadline bounds the process.
    SECTION("handshake timeout")
    {
        auto exchange = run_session(PeerBehavior::SilentHandshake, {"queue", "list"}, {"--timeout", "50"});
        CHECK(exchange.methods == std::vector<std::string>{"system.info"});
        CHECK(exchange.exit->exit_code == 3);
        CHECK(exchange.output.empty());
        CHECK(exchange.error == "jobuctl: Overall command deadline expired\n");
    }
    SECTION("command timeout")
    {
        auto exchange = run_session(PeerBehavior::SilentCommand, {"queue", "list"}, {"--timeout", "50"});
        CHECK(exchange.methods == std::vector<std::string>{"system.info", "queue.list"});
        CHECK(exchange.exit->exit_code == 3);
        CHECK(exchange.output.empty());
        CHECK(exchange.error == "jobuctl: Overall command deadline expired\n");
    }
    SECTION("unobserved mutation reports uncertainty")
    {
        auto exchange =
            run_session(PeerBehavior::SilentCommand, {"queue", "create", "reports"}, {"--json", "--timeout", "50"});
        CHECK(exchange.methods == std::vector<std::string>{"system.info", "queue.create"});
        CHECK(exchange.exit->exit_code == 3);
        CHECK(exchange.output.empty());
        auto value = parse_json(exchange.error);
        REQUIRE(value);
        auto const& error = value->as_object().at("error").as_object();
        CHECK(error.at("code").as_string() == "jobu.client.timeout");
        CHECK(error.at("outcome_unknown").as_bool());
    }
}

TEST_CASE("jobuctl suspend wait submits once and reads until suspension completes", "[jobuctl][session]")
{
    auto queue =
        run_session(PeerBehavior::QueueSuspendPolling, {"queue", "suspend", "--name", "reports", "--wait"}, {"--json"});
    CHECK(queue.exit->exit_code == 0);
    CHECK(queue.methods == std::vector<std::string>{"system.info", "queue.suspend", "queue.get", "queue.get"});
    auto queue_result = parse_json(queue.output);
    REQUIRE(queue_result);
    CHECK(queue_result->as_object().at("state").as_string() == "suspended");

    auto job = run_session(PeerBehavior::JobSuspendPolling,
                           {"job", "suspend", "00112233-4455-6677-8899-aabbccddeeff", "--wait"},
                           {"--json"});
    CHECK(job.exit->exit_code == 0);
    CHECK(job.methods == std::vector<std::string>{"system.info", "job.suspend", "job.get", "job.get"});
    auto job_result = parse_json(job.output);
    REQUIRE(job_result);
    CHECK(job_result->as_object().at("state").as_string() == "suspended");
}

TEST_CASE("jobuctl wait deadline reports an unconfirmed state after observed mutation", "[jobuctl][session]")
{
    auto exchange = run_session(PeerBehavior::SuspendNeverSettles,
                                {"queue", "suspend", "--name", "reports", "--wait"},
                                {"--json", "--timeout", "250"});
    CHECK(exchange.exit->exit_code == 3);
    CHECK(exchange.methods.front() == "system.info");
    CHECK(exchange.methods[1] == "queue.suspend");
    CHECK(std::count(exchange.methods.begin(), exchange.methods.end(), "queue.suspend") == 1);
    auto value = parse_json(exchange.error);
    REQUIRE(value);
    auto const& error = value->as_object().at("error").as_object();
    CHECK(error.at("code").as_string() == "jobu.client.timeout");
    CHECK_FALSE(error.at("outcome_unknown").as_bool());
}
