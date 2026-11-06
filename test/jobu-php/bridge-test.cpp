#include "client.hpp"
#include "request.hpp"
#include "support/memory_io_device.hpp"
#include "support/temporary_directory.hpp"

#include <jb/core/event_loop.hpp>
#include <jb/core/event_loop_types.hpp>
#include <jb/core/json.hpp>
#include <jb/core/object.hpp>
#include <jb/core/scoped_event_loop.hpp>
#include <jb/core/signal.hpp>
#include <jb/jobu/attribute_registry.hpp>
#include <jb/jobu/client/control_client.hpp>
#include <jb/jobu/system_info.hpp>
#include <jb/net/local_server.hpp>
#include <jb/net/local_socket.hpp>
#include <jb/rpc/client.hpp>
#include <jb/rpc/framing.hpp>
#include <jb/rpc/protocol.hpp>
#include <jb/rpc/server.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant> // IWYU pragma: keep std::get in Catch2 assertions
#include <vector>

namespace {
using namespace std::chrono_literals;
using jb::core::Clock;
using jb::core::JsonValue;
using jb::jobu::ControlCallOptions;
using jb::jobu::SystemInfo;

auto peer_info() -> SystemInfo
{
    return {
        .daemon_version = "controlled-peer",
        .api_version    = {.major = 1, .minor = 7},
        .capabilities   = {"system.info",  "system.stats",  "queue.create",  "queue.get",         "queue.list",
                           "queue.update", "queue.suspend", "queue.resume",  "queue.delete",      "queue.stats",
                           "job.create",   "job.get",       "job.list",      "job.update",        "job.suspend",
                           "job.resume",   "job.move",      "job.delete",    "job.run_now",       "run.get",
                           "run.list",     "run.cancel",    "attempt.get",   "attempt.list",      "attempt.output",
                           "secret.set",   "secret.list",   "secret.delete", "schedule.validate", "schedule.next"}
    };
}

auto reply_frame(std::uint64_t id, JsonValue value, bool error = false) -> std::string
{
    JsonValue envelope{
        .data = JsonValue::Object{{"jsonrpc", JsonValue{.data = std::string{"2.0"}}},
                                  {"id", JsonValue{.data = id}},
                                  {error ? "error" : "result", std::move(value)}}
    };
    auto json = jb::core::serialize_json(envelope);
    REQUIRE(json);
    auto frame = jb::rpc::frame_message(json.value());
    REQUIRE(frame);
    return std::move(frame).value();
}

struct TypedFixture {
    std::unique_ptr<jb::core::ScopedEventLoop> scope;
    jb::jobu::StandardAttributeRegistry        attributes;
    std::unique_ptr<jb::test::MemoryIODevice>  device;
    std::unique_ptr<jb::rpc::Client>           raw;
    std::unique_ptr<jb::jobu::ControlClient>   typed;

    TypedFixture()
    {
        auto created = jb::core::ScopedEventLoop::create();
        REQUIRE(created);
        scope  = std::move(created).value();
        device = std::make_unique<jb::test::MemoryIODevice>();
        device->open();
        raw                    = std::make_unique<jb::rpc::Client>(*device);
        typed                  = std::make_unique<jb::jobu::ControlClient>(*raw, attributes);
        bool             ready = false;
        jb::core::Object receiver;
        typed->ready.connect(&receiver, [&](SystemInfo const&) { ready = true; });
        REQUIRE(typed->initialize());
        device->inject_input(reply_frame(1, jb::jobu::system_info_to_json(peer_info())));
        REQUIRE(scope->loop().run_until(Clock::now() + 1s, [&] { return ready; }));
        (void)device->take_written_data();
    }

    auto read(jb::core::TimePoint deadline) const -> jb::php::Reply
    {
        return jb::php::call_synchronously(*typed, scope->loop(), deadline, [&](ControlCallOptions options) {
            return typed->get_system_info(options);
        });
    }
};

} // namespace

TEST_CASE("Bridge captures immediate raw and deferred replies after call acceptance", "[php][bridge]")
{
    for (bool immediate : {true, false}) {
        TypedFixture     fixture;
        jb::core::Object receiver;
        auto             frame = reply_frame(2, jb::jobu::system_info_to_json(peer_info()));
        fixture.device->bytes_written.connect(&receiver, [&](std::size_t) {
            if (immediate) {
                fixture.device->inject_input(frame);
            }
            else {
                REQUIRE(fixture.scope->loop().post([&] { fixture.device->inject_input(frame); }));
            }
        });
        auto result = fixture.read(Clock::now() + 1s);
        REQUIRE(result);
        CHECK(std::get<SystemInfo>(result.value()).daemon_version == "controlled-peer");
    }
}

TEST_CASE("Bridge preserves pre-acceptance and remote failure identity", "[php][bridge]")
{
    TypedFixture fixture;
    SECTION("pre-acceptance")
    {
        fixture.typed->close();
        auto result = fixture.read(Clock::now() + 1s);
        REQUIRE_FALSE(result);
        CHECK(std::get<jb::core::Error>(result.error().error).code == "jobu.client.not_ready");
        CHECK_FALSE(result.error().outcome_unknown);
        CHECK(fixture.device->written_data().empty());
    }
    SECTION("represented remote error")
    {
        auto remote = jb::core::parse_json(
            R"({"code":-32000,"message":"observed","data":{"code":"test.failure","category":"conflict"}})");
        REQUIRE(remote);
        REQUIRE(
            fixture.scope->loop().post([&] { fixture.device->inject_input(reply_frame(2, remote.value(), true)); }));
        auto result = fixture.read(Clock::now() + 1s);
        REQUIRE_FALSE(result);
        CHECK(result.error().kind == jb::jobu::ControlFailureKind::Remote);
        auto const& error = std::get<jb::rpc::RpcError>(result.error().error);
        CHECK(error.code == -32000);
        CHECK(error.message == "observed");
        REQUIRE(error.data);
        CHECK(error.data == remote->as_object().at("data"));
        CHECK_FALSE(result.error().outcome_unknown);
    }
}

TEST_CASE("Bridge timeout retires observations and ignores late replies", "[php][bridge]")
{
    TypedFixture fixture;
    auto         expired = fixture.read(Clock::now() + 2ms);
    REQUIRE_FALSE(expired);
    CHECK(std::get<jb::core::Error>(expired.error().error).category == jb::core::ErrorCategory::Timeout);
    CHECK_FALSE(expired.error().outcome_unknown);
    fixture.device->inject_input(reply_frame(2, jb::jobu::system_info_to_json(peer_info())));
    REQUIRE(fixture.scope->loop().post(
        [&] { fixture.device->inject_input(reply_frame(3, jb::jobu::system_info_to_json(peer_info()))); }));
    CHECK(fixture.read(Clock::now() + 1s));
}

TEST_CASE("Bridge terminal and timeout failures retain mutation uncertainty", "[php][bridge]")
{
    TypedFixture     fixture;
    bool             terminal_seen = false;
    jb::core::Object receiver;
    fixture.typed->failed.connect(&receiver, [&](jb::core::Error const&) { terminal_seen = true; });
    bool terminal = false;
    SECTION("terminal failure preceding call failure")
    {
        terminal = true;
        REQUIRE(fixture.scope->loop().post([&] { fixture.device->close(); }));
    }
    SECTION("deadline expiry")
    {}
    auto result = jb::php::call_synchronously(
        *fixture.typed,
        fixture.scope->loop(),
        Clock::now() + (terminal ? 1s : 2ms),
        [&](ControlCallOptions options) { return fixture.typed->delete_secret("test-secret", options); });
    REQUIRE_FALSE(result);
    CHECK(result.error().kind == jb::jobu::ControlFailureKind::Local);
    CHECK(result.error().outcome_unknown);
    CHECK(terminal_seen == terminal);
    if (terminal) {
        CHECK(std::get<jb::core::Error>(result.error().error).code == "rpc.connection_closed");
    }
    else {
        CHECK(std::get<jb::core::Error>(result.error().error).category == jb::core::ErrorCategory::Timeout);
    }
}

TEST_CASE("Bridge rejects expired deadlines and reentrancy before submitting", "[php][bridge]")
{
    TypedFixture fixture;
    bool         submitted = false;
    auto         submit    = [&](ControlCallOptions options) {
        submitted = true;
        return fixture.typed->get_system_info(options);
    };
    auto expired = jb::php::call_synchronously(*fixture.typed, fixture.scope->loop(), Clock::now(), submit);
    REQUIRE_FALSE(expired);
    CHECK_FALSE(submitted);
    bool checked = false;
    REQUIRE(fixture.scope->loop().post([&] {
        auto nested = jb::php::call_synchronously(*fixture.typed, fixture.scope->loop(), Clock::now() + 1s, submit);
        REQUIRE_FALSE(nested);
        CHECK(std::get<jb::core::Error>(nested.error().error).code == "core.event_loop.reentrant_wait");
        checked = true;
    }));
    REQUIRE(fixture.scope->loop().run_until(Clock::now() + 1s, [&] { return checked; }));
    CHECK_FALSE(submitted);
    CHECK(fixture.device->written_data().empty());
}

TEST_CASE("Deadline conversion rejects nonpositive values and overflow", "[php][bridge]")
{
    auto const start = Clock::now();
    CHECK_FALSE(jb::php::make_deadline(start, 0ms));
    CHECK_FALSE(jb::php::make_deadline(start, -1ms));
    CHECK_FALSE(jb::php::make_deadline(start, std::chrono::milliseconds::max()));
    CHECK_FALSE(jb::php::make_deadline(jb::core::TimePoint::max(), 1ms));
    auto deadline = jb::php::make_deadline(start, 10s);
    REQUIRE(deadline);
    CHECK(deadline.value() == start + 10s);
}

TEST_CASE("Bridge uses the remaining operation budget and fences stopped waits", "[php][bridge]")
{
    TypedFixture fixture;
    auto const   deadline = Clock::now() + 50ms;
    // Consume an earlier phase's budget with a bounded event wait, not a sleep.
    // A later typed call must receive only what remains, not its native 5s default.
    auto         earlier  = fixture.scope->loop().run_until(Clock::now() + 5ms, [] { return false; });
    REQUIRE_FALSE(earlier);
    REQUIRE(fixture.scope->loop().post([&] { fixture.scope->loop().quit(); }));
    auto result =
        jb::php::call_synchronously(*fixture.typed, fixture.scope->loop(), deadline, [&](ControlCallOptions options) {
            CHECK(options.timeout <= 45ms);
            return fixture.typed->get_system_info(options);
        });
    REQUIRE_FALSE(result);
    CHECK(std::get<jb::core::Error>(result.error().error).code == "core.event_loop.stopped");
    CHECK_FALSE(result.error().outcome_unknown);
    // A late response after a stopped wait must neither access dead stack state
    // nor get mistaken for the next operation's reply.
    fixture.device->inject_input(reply_frame(2, jb::jobu::system_info_to_json(peer_info())));
    REQUIRE(fixture.scope->loop().post(
        [&] { fixture.device->inject_input(reply_frame(3, jb::jobu::system_info_to_json(peer_info()))); }));
    CHECK(fixture.read(Clock::now() + 1s));
}

TEST_CASE("Native Client eagerly enforces version and every required capability", "[php][bridge]")
{
    auto original = peer_info();
    // Each omitted capability must fail, not just the first or last list entry.
    for (std::size_t scenario = 0; scenario < original.capabilities.size() + 4; ++scenario) {
        INFO(scenario);
        jb::php::Request request;
        auto             anchor = request.create_session();
        REQUIRE(anchor);
        jb::test::TemporaryDirectory directory{std::filesystem::perms::owner_all};
        jb::net::LocalServer         listener;
        jb::rpc::Server              server;
        jb::core::Object             receiver;
        auto                         info = original;
        if (scenario < original.capabilities.size()) {
            info.capabilities.erase(info.capabilities.begin() + static_cast<std::ptrdiff_t>(scenario));
        }
        else if (scenario == original.capabilities.size()) {
            info.api_version.minor = 6;
        }
        else if (scenario == original.capabilities.size() + 1) {
            info.api_version.major = 2;
        }
        else if (scenario == original.capabilities.size() + 2) {
            info.api_version.minor = 99;
            info.capabilities.emplace_back("future.method");
        }
        REQUIRE(server.register_method("system.info", [&](auto const&, auto const&) {
            return jb::rpc::MethodResult::success(jb::jobu::system_info_to_json(info));
        }));
        listener.new_connection.connect(&receiver, [&] {
            while (auto socket = listener.take_next_connection()) {
                REQUIRE(server.add_connection(std::move(socket)));
            }
        });
        auto endpoint = directory.path() / "peer.sock";
        REQUIRE(listener.listen(endpoint));
        jb::php::Client client;
        auto            result     = client.initialize(request, endpoint.string(), Clock::now() + 1s, 10s);
        auto const      compatible = scenario >= original.capabilities.size() + 2;
        CHECK(static_cast<bool>(result) == compatible);
        if (compatible) {
            auto read = client.call(Clock::now() + 1s, [&](ControlCallOptions options) {
                return client.typed()->get_system_info(options);
            });
            REQUIRE(read);
            client.close();
            client.close();
            bool submitted = false;
            auto closed    = client.call(Clock::now() + 1s, [&](ControlCallOptions options) {
                submitted = true;
                return client.typed()->get_system_info(options);
            });
            CHECK_FALSE(closed);
            CHECK_FALSE(submitted);
        }
        else {
            CHECK(result.error().category == jb::core::ErrorCategory::Unsupported);
            CHECK(client.typed() == nullptr);
        }
    }
}
