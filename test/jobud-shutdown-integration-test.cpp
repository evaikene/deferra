#include "byte_buffer.hpp"
#include "client.hpp"
#include "json.hpp"
#include "local_socket.hpp"
#include "process.hpp"
#include "protocol.hpp"
#include "support/shutdown_fixture.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <csignal> // IWYU pragma: keep POSIX signal constants.
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <unistd.h>

namespace {
using namespace jb::core;
using namespace jb::jobu;
using namespace jb::test;
using namespace std::chrono_literals;

class SignalFixture final {
public:
    void start()
    {
        REQUIRE(work.storage.database.close());
        socket = work.storage.directory.path() / ("daemon-" + std::to_string(++incarnation) + ".sock");
        exit.reset();
        daemon = std::make_unique<Process>();
        daemon->finished.connect(&work.app, [&](ProcessExit const& value) { exit = value; });
        daemon->standard_error.connect(&work.app, [&](ByteBuffer const& bytes) { log.append(as_string_view(bytes)); });
        auto arguments = std::vector<std::string>{"--database",
                                                  work.storage.database_file.string(),
                                                  "--socket",
                                                  socket.string(),
                                                  "--cli-concurrency",
                                                  "1",
                                                  "--http-concurrency",
                                                  "1"};
        if (::geteuid() == 0) {
            arguments.emplace_back("--allow-root-cli");
        }
        REQUIRE(daemon->start(
            {.executable = JOBUD_EXECUTABLE, .arguments = std::move(arguments), .termination_grace = 0ms}));
        work.until([&] {
            INFO(log);
            REQUIRE_FALSE(exit);
            std::error_code error;
            return std::filesystem::is_socket(socket, error);
        });
        // A completed RPC round trip proves scheduler startup and recovery finished.
        rpc("system.info", {.data = JsonValue::Object{}});
    }

    void rpc(std::string const& method, JsonValue parameters)
    {
        jb::net::LocalSocket connection;
        bool                 connected{false};
        connection.connected.connect(&work.app, [&] { connected = true; });
        connection.connect_to_server(socket);
        work.until([&] { return connected; });
        jb::rpc::Client                  client{connection};
        bool                             received{false};
        std::optional<jb::rpc::RpcError> error;
        client.result_received.connect(&work.app,
                                       [&](jb::rpc::RequestId const&, JsonValue const&) { received = true; });
        client.error_received.connect(&work.app, [&](jb::rpc::RequestId const&, jb::rpc::RpcError const& value) {
            error = value;
        });
        REQUIRE(client.call(method, std::move(parameters)));
        work.until([&] { return received || error.has_value(); });
        INFO((error ? error->message : ""));
        REQUIRE_FALSE(error);
    }

    void stop(int signal)
    {
        auto pid = daemon->process_id();
        REQUIRE(pid);
        REQUIRE(::kill(static_cast<pid_t>(*pid), signal) == 0);
        work.until([&] { return exit.has_value(); });
        INFO(log);
        REQUIRE(exit->kind == ProcessExitKind::Exited);
        REQUIRE(exit->exit_code == 0);
        // Inspect before destroying Process: its destructor must not make the test pass.
        REQUIRE_FALSE(std::filesystem::exists(socket));
        REQUIRE(work.storage.database.open());
    }

    ShutdownWork               work;
    std::filesystem::path      socket;
    unsigned                   incarnation{0};
    std::optional<ProcessExit> exit;
    std::string                log;
    std::unique_ptr<Process>   daemon;
};
} // namespace

TEST_CASE("SIGTERM and SIGINT stop mixed work without durable finalization", "[jobud][shutdown][integration]")
{
    require_shutdown_execution_environment();
    auto const signal = GENERATE(SIGTERM, SIGINT);
    auto const retry  = GENERATE(false, true);
    CAPTURE(signal, retry);
    SignalFixture fixture;
    fixture.work.seed(retry ? RecoveryPolicy::RetryInterrupted : RecoveryPolicy::FailInterrupted);
    fixture.start();
    fixture.work.await_work();

    auto const running = fixture.work.snapshot();
    fixture.stop(signal);
    fixture.work.require_cleanup();
    REQUIRE(fixture.work.snapshot() == running);
    fixture.work.hold_recovery();

    fixture.start();
    fixture.stop(signal);
    fixture.work.require_recovery(retry);
    auto const recovered = fixture.work.snapshot();
    fixture.start();
    fixture.stop(signal);
    CHECK(fixture.work.snapshot() == recovered);
    CHECK(fixture.work.server.requests().size() == 1);
}

TEST_CASE("isolated runtime proves fatal cleanup and direct-child reaping", "[jobud][shutdown][integration]")
{
    require_shutdown_execution_environment();
    auto const* const scenario =
        GENERATE("normal stop", "completion failure", "management failure", "fatal after stop");
    CAPTURE(scenario);
    Application                app{0, nullptr};
    Process                    helper;
    std::optional<ProcessExit> exit;
    std::string                output;
    helper.standard_output.connect(&app, [&](ByteBuffer const& bytes) { output.append(as_string_view(bytes)); });
    helper.standard_error.connect(&app, [&](ByteBuffer const& bytes) { output.append(as_string_view(bytes)); });
    helper.finished.connect(&app, [&](ProcessExit const& value) { exit = value; });

    // Process uses an exact environment. Preserve the caller's explicit opt-in for the helper's own root guard.
    ProcessEnvironment environment;
    if (auto const* opt_in = std::getenv("JOBU_TEST_ALLOW_ROOT_CLI")) {
        environment.emplace("JOBU_TEST_ALLOW_ROOT_CLI", opt_in);
    }
    REQUIRE(helper.start({.executable        = JOBUD_SHUTDOWN_TEST_HELPER,
                          .arguments         = {scenario},
                          .environment       = std::move(environment),
                          .timeout           = 12s,
                          .termination_grace = 0ms}));
    auto const deadline = Clock::now() + 14s;
    while (!exit && Clock::now() < deadline) {
        REQUIRE(app.process_events(EventFlag::All, 20) != ProcessEventsResult::Failed);
    }
    INFO(output);
    REQUIRE(exit);
    REQUIRE(exit->kind == ProcessExitKind::Exited);
    REQUIRE(exit->exit_code == 0);
    REQUIRE(output.find("All tests passed") != std::string::npos);
}
