#include "application.hpp"
#include "attribute_registry.hpp"
#include "byte_buffer.hpp"
#include "client.hpp"
#include "control_client.hpp"
#include "event_loop.hpp"
#include "json.hpp"
#include "local_server.hpp"
#include "local_socket.hpp"
#include "object.hpp"
#include "process.hpp"
#include "scoped_event_loop.hpp"
#include "server.hpp"
#include "support/temporary_directory.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <csignal>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <unistd.h>

namespace {

using namespace jb::core;
using namespace jb::jobu;
using namespace jb::net;
using namespace std::chrono_literals;

auto create_scope() -> std::unique_ptr<ScopedEventLoop>
{
    auto result = ScopedEventLoop::create();
    REQUIRE(result);
    return std::move(result).value();
}

auto remaining_options(TimePoint deadline) -> ControlCallOptions
{
    auto const remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now());
    REQUIRE(remaining > 0ms);
    return {.timeout = remaining};
}

/// Fixture-only bridge: one independent socket/raw/typed bundle, with all observers installed before I/O.
/// State outlives the receiver and wrappers. Removing the receiver first fences queued teardown deliveries.
struct ClientSession {
    StandardAttributeRegistry               attributes;
    std::optional<SystemInfo>               info;
    std::optional<Error>                    terminal;
    std::optional<IOError>                  connection_error;
    std::map<ControlCallId, ControlReply>   replies;
    std::map<ControlCallId, ControlFailure> failures;
    std::unique_ptr<Object>                 receiver{std::make_unique<Object>()};
    LocalSocket                             socket;
    std::unique_ptr<jb::rpc::Client>        raw;
    std::unique_ptr<ControlClient>          typed;

    void connect(std::filesystem::path const& endpoint, TimePoint deadline)
    {
        socket.error_occurred.connect(receiver.get(),
                                      [&](IOError error, std::string const&) { connection_error = error; });
        socket.connected.connect(receiver.get(), [this, deadline] {
            raw   = std::make_unique<jb::rpc::Client>(socket);
            typed = std::make_unique<ControlClient>(*raw, attributes);
            typed->ready.connect(receiver.get(), [&](SystemInfo const& value) { info = value; });
            typed->failed.connect(receiver.get(), [&](Error const& error) { terminal = error; });
            typed->reply_received.connect(receiver.get(), [&](ControlCallId id, ControlReply const& reply) {
                CHECK(replies.emplace(id, reply).second);
            });
            typed->call_failed.connect(receiver.get(), [&](ControlCallId id, ControlFailure const& failure) {
                CHECK(failures.emplace(id, failure).second);
            });
            auto const remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now());
            if (remaining <= 0ms) {
                terminal = Error{.category = ErrorCategory::Timeout,
                                 .code     = "embedding.connect_timeout",
                                 .message  = "Connection consumed the operation deadline"};
                return;
            }
            auto accepted = typed->initialize({.timeout = remaining});
            if (!accepted) {
                terminal = accepted.error();
            }
        });
        socket.connect_to_server(endpoint);
    }

    ~ClientSession()
    {
        receiver.reset();
        typed.reset();
        raw.reset();
        socket.abort();
    }

    void await_ready(EventLoop& loop, TimePoint deadline)
    {
        REQUIRE(loop.run_until(deadline, [&] { return info || terminal || connection_error; }));
        REQUIRE_FALSE(terminal);
        REQUIRE_FALSE(connection_error);
        REQUIRE(info);
        CHECK(info->api_version.major == 1);
        CHECK(info->api_version.minor >= 7);
        CHECK(socket.event_loop() == &loop);
        CHECK(raw->event_loop() == &loop);
        CHECK(typed->event_loop() == &loop);
    }

    void read_info(EventLoop& loop, TimePoint deadline)
    {
        auto call = typed->get_system_info(remaining_options(deadline));
        REQUIRE(call);
        REQUIRE(
            loop.run_until(deadline, [&] { return replies.contains(*call) || failures.contains(*call) || terminal; }));
        REQUIRE_FALSE(terminal);
        REQUIRE_FALSE(failures.contains(*call));
        REQUIRE(replies.contains(*call));
        CHECK(std::holds_alternative<SystemInfo>(replies.at(*call)));
    }
};

/// Process owns and reaps the child even when a test assertion aborts. No Application or helper thread is used.
struct Daemon {
    Object                     receiver;
    std::string                log;
    std::optional<ProcessExit> exit;
    Process                    process;

    void start(EventLoop& loop, std::filesystem::path const& database, std::filesystem::path const& endpoint)
    {
        process.standard_error.connect(&receiver, [&](ByteBuffer const& bytes) { log.append(as_string_view(bytes)); });
        process.finished.connect(&receiver, [&](ProcessExit const& value) { exit = value; });
        auto arguments =
            std::vector<std::string>{"--no-config", "--database", database.string(), "--socket", endpoint.string()};
        if (::geteuid() == 0) {
            arguments.emplace_back("--allow-root-daemon");
        }
        auto started = process.start({.executable = JOBUD_EXECUTABLE, .arguments = std::move(arguments)});
        INFO((started ? "Process accepted" : started.error().code));
        REQUIRE(started);
        REQUIRE(
            loop.run_until(Clock::now() + 8s, [&] { return exit || log.find("jobud.ready") != std::string::npos; }));
        INFO(log);
        REQUIRE_FALSE(exit);
    }

    void stop(EventLoop& loop)
    {
        REQUIRE(process.process_id());
        REQUIRE(::kill(static_cast<pid_t>(*process.process_id()), SIGTERM) == 0);
        REQUIRE(loop.run_until(Clock::now() + 8s, [&] { return exit.has_value(); }));
        INFO(log);
        CHECK(exit->kind == ProcessExitKind::Exited);
        CHECK(exit->exit_code == 0);
    }
};

TEST_CASE("Host repeatedly creates and destroys complete loop and daemon-client sessions",
          "[jobu][embedding][integration]")
{
    jb::test::TemporaryDirectory directory{std::filesystem::perms::owner_all};
    auto const                   database = directory.path() / "daemon.sqlite";
    auto const                   endpoint = directory.path() / "daemon.sock";
    for (int iteration = 0; iteration < 3; ++iteration) {
        auto  scope = create_scope();
        auto& loop  = scope->loop();
        CHECK(Application::instance() == nullptr);
        Daemon daemon;
        daemon.start(loop, database, endpoint);
        for (int client = 0; client < 2; ++client) {
            auto const    deadline = Clock::now() + 3s;
            ClientSession session;
            session.connect(endpoint, deadline);
            session.await_ready(loop, deadline);
            session.read_info(loop, deadline);
        }
        daemon.stop(loop);
    }
    CHECK(EventLoop::current() == nullptr);
    CHECK(Application::instance() == nullptr);
}

TEST_CASE("Two embedded client bundles share a loop while keeping independent calls and teardown",
          "[jobu][embedding][integration]")
{
    auto                         scope = create_scope();
    auto&                        loop  = scope->loop();
    jb::test::TemporaryDirectory directory{std::filesystem::perms::owner_all};
    auto const                   endpoint = directory.path() / "daemon.sock";
    Daemon                       daemon;
    daemon.start(loop, directory.path() / "daemon.sqlite", endpoint);

    auto const    deadline = Clock::now() + 3s;
    auto          first    = std::make_unique<ClientSession>();
    ClientSession second;
    first->connect(endpoint, deadline);
    second.connect(endpoint, deadline);
    first->await_ready(loop, deadline);
    second.await_ready(loop, deadline);

    auto first_call  = first->typed->get_system_info(remaining_options(deadline));
    auto second_call = second.typed->list_queues({}, remaining_options(deadline));
    REQUIRE(first_call);
    REQUIRE(second_call);
    REQUIRE(loop.run_until(deadline, [&] {
        return first->replies.contains(*first_call) && second.replies.contains(*second_call);
    }));
    CHECK(std::holds_alternative<SystemInfo>(first->replies.at(*first_call)));
    CHECK(std::holds_alternative<QueuePage>(second.replies.at(*second_call)));
    CHECK(first->replies.size() == 1);
    CHECK(second.replies.size() == 1);
    CHECK(first->failures.empty());
    CHECK(second.failures.empty());

    // Leave an accepted read outstanding when closing the first bundle; its queued work must be harmless.
    REQUIRE(first->typed->get_system_info(remaining_options(deadline)));
    first.reset();
    second.read_info(loop, deadline);
    daemon.stop(loop);
}

TEST_CASE("Daemon restart requires replacing the complete terminal client bundle", "[jobu][embedding][integration]")
{
    auto                         scope = create_scope();
    auto&                        loop  = scope->loop();
    jb::test::TemporaryDirectory directory{std::filesystem::perms::owner_all};
    auto const                   database = directory.path() / "daemon.sqlite";
    auto const                   endpoint = directory.path() / "daemon.sock";
    Daemon                       first;
    first.start(loop, database, endpoint);
    auto       old      = std::make_unique<ClientSession>();
    auto const deadline = Clock::now() + 3s;
    old->connect(endpoint, deadline);
    old->await_ready(loop, deadline);
    old->read_info(loop, deadline);

    first.stop(loop);
    REQUIRE(loop.run_until(Clock::now() + 3s, [&] { return old->terminal.has_value(); }));
    CHECK_FALSE(old->typed->get_system_info());
    old.reset();

    Daemon restarted;
    restarted.start(loop, database, endpoint);
    ClientSession fresh;
    auto const    fresh_deadline = Clock::now() + 3s;
    fresh.connect(endpoint, fresh_deadline);
    fresh.await_ready(loop, fresh_deadline);
    fresh.read_info(loop, fresh_deadline);
    restarted.stop(loop);
}

TEST_CASE("Failed connection and handshake timeout allow clean embedded recreation", "[jobu][embedding][integration]")
{
    auto                         scope = create_scope();
    auto&                        loop  = scope->loop();
    jb::test::TemporaryDirectory directory{std::filesystem::perms::owner_all};
    {
        ClientSession failed;
        failed.connect(directory.path() / "absent.sock", Clock::now() + 1s);
        REQUIRE(loop.run_until(Clock::now() + 1s, [&] { return failed.connection_error.has_value(); }));
        CHECK_FALSE(failed.info);
    }

    auto const endpoint = directory.path() / "daemon.sock";
    {
        LocalServer     incompatible;
        jb::rpc::Server rpc;
        REQUIRE(rpc.register_method("system.info", [](jb::rpc::RequestContext const&, std::optional<JsonValue> const&) {
            // A framed successful RPC reply can still violate the typed handshake contract.
            return jb::rpc::MethodResult::success(JsonValue{.data = JsonValue::Object{}});
        }));
        incompatible.new_connection.connect(&rpc, [&] {
            while (auto socket = incompatible.take_next_connection()) {
                REQUIRE(rpc.add_connection(std::move(socket)));
            }
        });
        REQUIRE(incompatible.listen(endpoint));
        ClientSession rejected;
        auto const    deadline = Clock::now() + 1s;
        rejected.connect(endpoint, deadline);
        REQUIRE(loop.run_until(deadline, [&] { return rejected.terminal.has_value(); }));
        CHECK_FALSE(rejected.info);
        CHECK(rejected.terminal->code == "jobu.client.invalid_response");
    }
    {
        // A real accepted socket that never answers system.info proves the wait's shared deadline is effective.
        LocalServer silent;
        REQUIRE(silent.listen(endpoint));
        ClientSession stalled;
        auto const    deadline = Clock::now() + 30ms;
        stalled.connect(endpoint, deadline);
        auto result = loop.run_until(deadline, [&] { return stalled.info || stalled.terminal; });
        CHECK_FALSE(stalled.info);
        if (result) {
            REQUIRE(stalled.terminal);
            CHECK(stalled.terminal->category == ErrorCategory::Timeout);
        }
        else {
            CHECK(result.error().code == "core.event_loop.timeout");
        }
    }

    Daemon daemon;
    daemon.start(loop, directory.path() / "daemon.sqlite", endpoint);
    ClientSession recovered;
    auto const    deadline = Clock::now() + 3s;
    recovered.connect(endpoint, deadline);
    recovered.await_ready(loop, deadline);
    recovered.read_info(loop, deadline);
    daemon.stop(loop);
}

} // namespace
