#include "application.hpp"
#include "attribute_registry.hpp"
#include "byte_buffer.hpp"
#include "control_json.hpp"
#include "framing.hpp"
#include "history_json.hpp"
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
#include <fstream>
#include <iterator>
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
    RunCancelPolling,
    RunCancelOtherTerminal,
    RunCancelNeverSettles,
    OutputChunk,
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
                                       "job.suspend", "run.cancel",
                                       "run.get", "attempt.output",
                                       "queue.create", "queue.delete",
                                       "queue.get", "queue.suspend",
                                       "queue.list", "system.info"}
                });
                response.emplace("result", behavior == PeerBehavior::InvalidInfo ? JsonValue{} : std::move(info));
            }
            else if (behavior == PeerBehavior::RemoteError) {
                auto error = parse_json(
                    R"({"code":-32000,"message":"fixture rejection","data":{"category":"conflict","code":"jobu.fixture.conflict"}})");
                REQUIRE(error);
                response.emplace("error", std::move(*error));
            }
            else if (method == "run.cancel" || method == "run.get") {
                StandardAttributeRegistry registry;
                auto                      id         = Uuid::parse("00112233-4455-6677-8899-aabbccddeeff");
                auto                      at         = parse_utc_timestamp("2030-01-01T00:00:00Z");
                auto                      attributes = materialize_attributes(registry, {}, {}, {});
                auto                      payload    = parse_json(R"({"command":"/bin/true"})");
                REQUIRE(id);
                REQUIRE(at);
                REQUIRE(attributes);
                REQUIRE(payload);
                auto run = JobRun{.id          = *id,
                                  .job_id      = *id,
                                  .queue_id    = *id,
                                  .planned_at  = *at,
                                  .runnable_at = *at,
                                  .attributes  = *attributes,
                                  .payload     = *payload,
                                  .state       = RunState::Running};
                if (method == "run.cancel") {
                    auto result =
                        cancel_run_result_to_json({.run = run, .disposition = CancelDisposition::Requested}, registry);
                    REQUIRE(result);
                    response.emplace("result", std::move(*result));
                }
                else {
                    ++poll_count;
                    if (behavior == PeerBehavior::RunCancelOtherTerminal) {
                        run.state        = RunState::Succeeded;
                        run.completed_at = *at;
                    }
                    else if (behavior == PeerBehavior::RunCancelPolling && poll_count == 2) {
                        run.state        = RunState::Cancelled;
                        run.completed_at = *at;
                    }
                    auto result = run_details_to_json(run, registry);
                    REQUIRE(result);
                    response.emplace("result", std::move(*result));
                }
            }
            else if (method == "attempt.output") {
                auto id = Uuid::parse("00112233-4455-6677-8899-aabbccddeeff");
                REQUIRE(id);
                auto chunk = AttemptOutputChunk{
                    .attempt        = {.run_id = *id, .attempt_number = 1},
                    .channel        = OutputChannel::Stdout,
                    .status         = OutputStatus::Available,
                    .bytes_returned = 4,
                    .retained_bytes = 4,
                    .total_bytes    = 10,
                    .omitted_bytes  = 6,
                    .truncated      = true,
                    .encoding       = OutputEncoding::Base64,
                    .data           = {std::byte{'A'}, std::byte{0}, std::byte{0xff}, std::byte{'Z'}},
                };
                auto result = attempt_output_chunk_to_json(chunk);
                REQUIRE(result);
                response.emplace("result", std::move(*result));
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

TEST_CASE("jobuctl cancellation wait submits once and reconciles the observed terminal state", "[jobuctl][session]")
{
    auto const command   = std::vector<std::string>{"run", "cancel", "00112233-4455-6677-8899-aabbccddeeff", "--wait"};
    auto       completed = run_session(PeerBehavior::RunCancelPolling, command, {"--json"});
    CHECK(completed.exit->exit_code == 0);
    CHECK(completed.methods == std::vector<std::string>{"system.info", "run.cancel", "run.get", "run.get"});
    auto value = parse_json(completed.output);
    REQUIRE(value);
    CHECK(value->as_object().at("disposition").as_string() == "completed");
    CHECK(value->as_object().at("run").as_object().at("state").as_string() == "cancelled");

    auto conflict = run_session(PeerBehavior::RunCancelOtherTerminal, command, {"--json"});
    CHECK(conflict.exit->exit_code == 1);
    CHECK(std::count(conflict.methods.begin(), conflict.methods.end(), "run.cancel") == 1);
    CHECK(conflict.output.empty());
    auto error = parse_json(conflict.error);
    REQUIRE(error);
    CHECK(error->as_object().at("error").as_object().at("code").as_string() == "jobuctl.wait.state_changed");

    auto timeout = run_session(PeerBehavior::RunCancelNeverSettles, command, {"--json", "--timeout", "250"});
    CHECK(timeout.exit->exit_code == 3);
    CHECK(std::count(timeout.methods.begin(), timeout.methods.end(), "run.cancel") == 1);
    CHECK(timeout.output.empty());
    auto timeout_error = parse_json(timeout.error);
    REQUIRE(timeout_error);
    CHECK_FALSE(timeout_error->as_object().at("error").as_object().at("outcome_unknown").as_bool());
}

TEST_CASE("jobuctl output modes preserve bytes and never overwrite a file", "[jobuctl][session]")
{
    auto const command = std::vector<std::string>{"attempt",
                                                  "output",
                                                  "00112233-4455-6677-8899-aabbccddeeff",
                                                  "1",
                                                  "--channel",
                                                  "stdout"};
    auto       json    = run_session(PeerBehavior::OutputChunk, command, {"--json"});
    CHECK(json.exit->exit_code == 0);
    auto value = parse_json(json.output);
    REQUIRE(value);
    CHECK(value->as_object().at("truncated").as_bool());
    CHECK(value->as_object().at("omitted_bytes").as_uint() == 6);
    CHECK(value->as_object().at("encoding").as_string() == "base64");

    auto human = run_session(PeerBehavior::OutputChunk, command);
    CHECK(human.exit->exit_code == 0);
    CHECK(human.output.find("truncated=true") != std::string::npos);
    CHECK(human.output.find("omitted_bytes=6") != std::string::npos);
    CHECK(human.output.find("encoding=base64") != std::string::npos);

    auto raw_command = command;
    raw_command.emplace_back("--raw");
    auto raw = run_session(PeerBehavior::OutputChunk, raw_command);
    CHECK(raw.exit->exit_code == 0);
    CHECK(raw.output == std::string{"A\0\xffZ", 4});

    jb::test::TemporaryDirectory directory;
    auto const                   path         = directory.path() / "chunk.bin";
    auto                         file_command = command;
    file_command.insert(file_command.end(), {"--output-file", path.string()});
    auto saved = run_session(PeerBehavior::OutputChunk, file_command);
    CHECK(saved.exit->exit_code == 0);
    CHECK(saved.output.empty());
    auto input = std::ifstream{path, std::ios::binary};
    REQUIRE(input);
    auto bytes = std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    CHECK(bytes == std::string{"A\0\xffZ", 4});

    auto refused = run_session(PeerBehavior::OutputChunk, file_command);
    CHECK(refused.exit->exit_code == 2);
    CHECK(refused.output.empty());
    auto unchanged = std::ifstream{path, std::ios::binary};
    REQUIRE(unchanged);
    CHECK(std::string{std::istreambuf_iterator<char>{unchanged}, std::istreambuf_iterator<char>{}} == bytes);
}
