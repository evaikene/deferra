#include "application.hpp"
#include "event_loop_types.hpp"
#include "event_thread.hpp"
#include "protocol_priv.hpp"
#include "server.hpp"
#include "support/memory_io_device.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

// NOLINTBEGIN(readability-magic-numbers)

using namespace jb::core;
using namespace jb::rpc;
using namespace jb::rpc::detail;
using jb::test::MemoryIODevice;

namespace {

template <typename T>
auto make_json(T value) -> JsonValue
{
    JsonValue result;
    result.data = std::move(value);
    return result;
}

auto encode_frame(JsonValue const& value, FramingLimits limits = {}) -> std::string
{
    auto serialized = serialize_json(value);
    REQUIRE(serialized);
    auto framed = frame_message(serialized.value(), limits);
    REQUIRE(framed);
    return std::move(framed).value();
}

auto request_frame(RequestId                       id,
                   std::string_view                method,
                   std::optional<JsonValue> const& params = std::nullopt,
                   FramingLimits                   limits = {}) -> std::string
{
    return encode_frame(encode_request(id, method, params), limits);
}

auto notification_frame(std::string_view                method,
                        std::optional<JsonValue> const& params = std::nullopt,
                        FramingLimits                   limits = {}) -> std::string
{
    return encode_frame(encode_notification(method, params), limits);
}

auto take_values(MemoryIODevice& device) -> std::vector<JsonValue>
{
    StreamFramer framer;
    auto         bodies = framer.append(device.take_written_data());
    REQUIRE(bodies);

    auto values = std::vector<JsonValue>{};
    values.reserve(bodies->size());
    for (auto const& body : bodies.value()) {
        auto parsed = parse_json(body);
        REQUIRE(parsed);
        values.push_back(std::move(parsed).value());
    }
    return values;
}

auto require_response(JsonValue const& value) -> ResponseEnvelope
{
    auto decoded = decode_response_document(value);
    REQUIRE(decoded);
    REQUIRE(decoded->entries.size() == 1U);
    return decoded->entries.front();
}

auto require_error(ResponseEnvelope const& response) -> RpcError const&
{
    REQUIRE(std::holds_alternative<RpcError>(response.payload));
    return std::get<RpcError>(response.payload);
}

auto require_result(ResponseEnvelope const& response) -> JsonValue const&
{
    REQUIRE(std::holds_alternative<JsonValue>(response.payload));
    return std::get<JsonValue>(response.payload);
}

struct AttachedDevice {
    MemoryIODevice* device;
    ConnectionId    id;
};

auto attach(Server& server, std::unique_ptr<MemoryIODevice> device, OperationContext operation = {}) -> AttachedDevice
{
    auto* raw    = device.get();
    auto  result = server.add_connection(std::move(device), std::move(operation));
    REQUIRE(result);
    return {.device = raw, .id = result.value()};
}

auto attach(Server& server, OperationContext operation = {}) -> AttachedDevice
{
    auto device = std::make_unique<MemoryIODevice>();
    device->open();
    return attach(server, std::move(device), std::move(operation));
}

auto null_success_handler() -> MethodHandler
{
    return [](RequestContext const&, std::optional<JsonValue> const&) {
        return MethodResult::success(make_json(JsonNull{}));
    };
}

auto response_frame_size(RequestId const& id = RequestId{std::uint64_t{1}}) -> std::size_t
{
    return encode_frame(encode_success_response(id, make_json(JsonNull{}))).size();
}

} // anonymous namespace

TEST_CASE("MemoryIODevice provides deterministic byte and lifecycle behavior", "[rpc][server][support]")
{
    MemoryIODevice device;
    auto           events = std::vector<std::string>{};
    device.ready_read.connect([&events]() { events.emplace_back("ready"); });
    device.bytes_written.connect(
        [&events](std::size_t bytes) { events.push_back("written:" + std::to_string(bytes)); });
    device.closed.connect([&events]() { events.emplace_back("closed"); });

    device.inject_input("before-open\n");
    CHECK(events.empty());
    device.open();
    device.open();
    CHECK(device.is_open());
    CHECK(device.can_read_line());
    device.fail(IOError::ReadError, "stale read error");
    CHECK(device.read_line() == "before-open");
    CHECK(device.error() == IOError::NoError);

    device.inject_input("abcdef");
    CHECK(device.bytes_available() == 6U);
    device.fail(IOError::ReadError, "stale read error");
    CHECK(device.read(2U) == "ab");
    CHECK(device.error() == IOError::NoError);
    device.fail(IOError::ReadError, "stale read error");
    CHECK(device.read_all() == "cdef");
    CHECK(device.error() == IOError::NoError);
    REQUIRE(events == std::vector<std::string>{"ready"});

    device.fail(IOError::WriteError, "stale write error");
    CHECK(device.write("first") == 5U);
    CHECK(device.error() == IOError::NoError);
    CHECK(device.unacknowledged_bytes() == 0U);
    CHECK(device.written_data() == "first");
    CHECK(device.take_written_data() == "first");
    CHECK(device.written_data().empty());

    device.set_auto_acknowledge_writes(false);
    device.set_write_limit(3U);
    CHECK(device.write("second") == 3U);
    CHECK(device.written_data() == "sec");
    CHECK(device.unacknowledged_bytes() == 3U);
    device.acknowledge_writes(2U);
    CHECK(device.unacknowledged_bytes() == 1U);
    device.acknowledge_writes();
    CHECK(device.unacknowledged_bytes() == 0U);

    device.close();
    device.close();
    CHECK_FALSE(device.is_open());
    CHECK(events.back() == "closed");
}

TEST_CASE("Server public defaults and object contract are stable", "[rpc][server][api]")
{
    static_assert(std::is_base_of_v<Object, Server>);
    static_assert(!std::is_copy_constructible_v<Server>);
    static_assert(!std::is_move_constructible_v<Server>);

    auto const options = ServerOptions{};
    CHECK(options.framing.max_header_bytes == 16U * 1024U);
    CHECK(options.framing.max_body_bytes == 1024U * 1024U);
    CHECK(options.json.max_depth == 64U);
    CHECK(options.max_batch_entries == 64U);
    CHECK(options.max_connections == 128U);
    CHECK(options.max_queued_output_bytes == 2U * 1024U * 1024U);

    Application app{0, nullptr};
    Object      parent;
    Server      server{{}, &parent};
    CHECK(server.parent() == &parent);
    CHECK(server.connection_count() == 0U);
    CHECK_FALSE(server.has_method("missing"));
}

TEST_CASE("Server method registration enforces its configuration contract", "[rpc][server][registration]")
{
    Application app{0, nullptr};
    Server      server;

    CHECK_FALSE(server.register_method("", null_success_handler()));
    CHECK_FALSE(server.register_method("empty", {}));
    CHECK_FALSE(server.register_method("rpc.internal", null_success_handler()));
    CHECK(server.register_method("RPC.internal", null_success_handler()));
    CHECK(server.register_method("echo", null_success_handler()));
    CHECK(server.has_method("echo"));
    CHECK_FALSE(server.register_method("echo", [](auto const&, auto const&) {
        return MethodResult::success(make_json(std::string{"replacement"}));
    }));
    auto connection = attach(server);
    connection.device->inject_input(request_frame(std::uint64_t{1}, "echo"));
    auto values = take_values(*connection.device);
    REQUIRE(values.size() == 1U);
    CHECK(require_result(require_response(values.front())).is_null());
    CHECK(server.unregister_method("echo"));
    CHECK_FALSE(server.unregister_method("echo"));
    CHECK(server.register_method("echo", null_success_handler()));
}

TEST_CASE("A handler may unregister itself during dispatch", "[rpc][server][registration]")
{
    Application app{0, nullptr};
    Server      server;
    auto        calls = 0;
    REQUIRE(server.register_method("once", [&server, &calls](auto const&, auto const&) {
        ++calls;
        CHECK(server.unregister_method("once"));
        return MethodResult::success(make_json(std::string{"done"}));
    }));
    auto connection = attach(server);

    connection.device->inject_input(request_frame(std::uint64_t{1}, "once"));
    auto first = take_values(*connection.device);
    REQUIRE(first.size() == 1U);
    CHECK(require_result(require_response(first.front())).as_string() == "done");
    CHECK_FALSE(server.has_method("once"));

    connection.device->inject_input(request_frame(std::uint64_t{2}, "once"));
    auto second = take_values(*connection.device);
    REQUIRE(second.size() == 1U);
    CHECK(require_error(require_response(second.front())).code == static_cast<std::int64_t>(ErrorCode::MethodNotFound));
    CHECK(calls == 1);
}

TEST_CASE("Server rejects invalid devices and destroys transferred ownership", "[rpc][server][admission]")
{
    SECTION("null and closed devices")
    {
        Application app{0, nullptr};
        Server      server;
        auto        opened = 0;
        auto        closed = 0;
        auto        errors = 0;
        server.connection_opened.connect([&opened](ConnectionId) { ++opened; });
        server.connection_closed.connect([&closed](ConnectionId) { ++closed; });
        server.connection_error.connect([&errors](ConnectionId, Error const&) { ++errors; });

        auto null_result = server.add_connection(nullptr);
        REQUIRE_FALSE(null_result);
        CHECK(null_result.error().category == ErrorCategory::InvalidArgument);
        CHECK(null_result.error().code == "rpc.invalid_argument");
        CHECK(null_result.error().detail.empty());

        auto destroyed = false;
        auto device    = std::make_unique<MemoryIODevice>();
        device->destroyed.connect([&destroyed]() { destroyed = true; });
        auto closed_result = server.add_connection(std::move(device));
        REQUIRE_FALSE(closed_result);
        CHECK(destroyed);
        CHECK(opened == 0);
        CHECK(closed == 0);
        CHECK(errors == 0);
    }

    SECTION("server and device require the same non-null event loop")
    {
        auto server_without_loop = Server{};
        auto device              = std::make_unique<MemoryIODevice>();
        device->open();
        auto no_loop = server_without_loop.add_connection(std::move(device));
        REQUIRE_FALSE(no_loop);
        CHECK(no_loop.error().code == "rpc.invalid_argument");
    }

    SECTION("a device created before the application has no event loop")
    {
        auto device = std::make_unique<MemoryIODevice>();
        device->open();
        Application app{0, nullptr};
        Server      server;
        auto        result = server.add_connection(std::move(device));
        REQUIRE_FALSE(result);
        CHECK(result.error().code == "rpc.invalid_argument");
    }

    SECTION("a different event loop is rejected")
    {
        Application app{0, nullptr};
        Server      server;
        EventThread other_thread;
        auto        device = std::make_unique<MemoryIODevice>();
        device->open();
        REQUIRE(device->move_to_thread(&other_thread));
        auto result = server.add_connection(std::move(device));
        REQUIRE_FALSE(result);
        CHECK(result.error().code == "rpc.invalid_argument");
    }
}

TEST_CASE("Server enforces connection limits and monotonic identifiers", "[rpc][server][admission]")
{
    SECTION("zero rejects every connection")
    {
        Application app{0, nullptr};
        Server      server{{.max_connections = 0U}};
        auto        device = std::make_unique<MemoryIODevice>();
        device->open();
        auto result = server.add_connection(std::move(device));
        REQUIRE_FALSE(result);
        CHECK(result.error().category == ErrorCategory::ResourceExhausted);
        CHECK(result.error().code == "rpc.connection_limit");
        CHECK(result.error().message == "RPC server connection limit reached");
        CHECK(result.error().detail.empty());
    }

    SECTION("the configured count is inclusive and IDs are not reused")
    {
        Application app{0, nullptr};
        Server      server{{.max_connections = 2U}};
        auto        first  = attach(server);
        auto        second = attach(server);
        CHECK(first.id == 1U);
        CHECK(second.id == 2U);
        CHECK(server.connection_count() == 2U);

        auto excess = std::make_unique<MemoryIODevice>();
        excess->open();
        auto rejected = server.add_connection(std::move(excess));
        REQUIRE_FALSE(rejected);
        CHECK(rejected.error().code == "rpc.connection_limit");

        server.close_connection(first.id);
        CHECK(server.connection_count() == 1U);
        auto third = attach(server);
        CHECK(third.id == 3U);
        CHECK(server.connection_count() == 2U);
    }
}

TEST_CASE("Connection opening installs ownership before signals and buffered dispatch", "[rpc][server][admission]")
{
    Application app{0, nullptr};
    Server      server;
    auto        calls = 0;
    REQUIRE(server.register_method("buffered", [&calls](auto const&, auto const&) {
        ++calls;
        return MethodResult::success(make_json(std::string{"processed"}));
    }));

    auto  device = std::make_unique<MemoryIODevice>();
    auto* raw    = device.get();
    device->open();
    device->inject_input(request_frame(std::uint64_t{1}, "buffered"));

    auto opened = 0;
    server.connection_opened.connect([&](ConnectionId id) {
        ++opened;
        CHECK(id == 1U);
        CHECK(server.connection_count() == 1U);
        CHECK(raw->parent() == &server);
        CHECK(calls == 0);
    });

    auto connection = attach(server, std::move(device));
    CHECK(opened == 1);
    CHECK(calls == 1);
    auto values = take_values(*connection.device);
    REQUIRE(values.size() == 1U);
    CHECK(require_result(require_response(values.front())).as_string() == "processed");
}

TEST_CASE("An opened listener may close the connection immediately", "[rpc][server][admission]")
{
    Application app{0, nullptr};
    Server      server;
    auto        calls  = 0;
    auto        closed = 0;
    REQUIRE(server.register_method("unused", [&calls](auto const&, auto const&) {
        ++calls;
        return MethodResult::success(make_json(JsonNull{}));
    }));

    auto device = std::make_unique<MemoryIODevice>();
    device->open();
    device->inject_input(request_frame(std::uint64_t{1}, "unused"));
    server.connection_opened.connect([&server](ConnectionId id) { server.close_connection(id); });
    server.connection_closed.connect([&closed](ConnectionId) { ++closed; });

    auto result = server.add_connection(std::move(device));
    REQUIRE(result);
    CHECK(result.value() == 1U);
    CHECK(server.connection_count() == 0U);
    CHECK(calls == 0);
    CHECK(closed == 1);
}

TEST_CASE("Handlers receive connection and operation context", "[rpc][server][context]")
{
    Application app{0, nullptr};
    Server      server;
    auto        seen = RequestContext{};
    REQUIRE(server.register_method("context", [&seen](RequestContext const& context, auto const&) {
        seen = context;
        return MethodResult::success(make_json(JsonNull{}));
    }));

    auto operation                    = OperationContext{};
    operation.peer.process_id         = 42U;
    operation.peer.user_id            = 1000U;
    operation.peer.group_id           = 100U;
    operation.authenticated_principal = "alice";
    auto connection                   = attach(server, operation);
    connection.device->inject_input(request_frame(std::uint64_t{1}, "context"));

    CHECK(seen.connection_id == connection.id);
    REQUIRE(seen.operation.peer.process_id);
    REQUIRE(seen.operation.peer.user_id);
    REQUIRE(seen.operation.peer.group_id);
    REQUIRE(seen.operation.authenticated_principal);
    CHECK(*seen.operation.peer.process_id == 42U);
    CHECK(*seen.operation.peer.user_id == 1000U);
    CHECK(*seen.operation.peer.group_id == 100U);
    CHECK(*seen.operation.authenticated_principal == "alice");
}

TEST_CASE("Server dispatches representative IDs and parameter shapes", "[rpc][server][dispatch]")
{
    Application app{0, nullptr};
    Server      server;
    auto        params_seen = std::vector<std::optional<JsonValue>>{};
    REQUIRE(server.register_method("echo", [&params_seen](auto const&, std::optional<JsonValue> const& params) {
        params_seen.push_back(params);
        return MethodResult::success(params.value_or(make_json(JsonNull{})));
    }));
    auto connection = attach(server);

    auto ids    = std::vector<RequestId>{NullRequestId{}, std::int64_t{-7}, std::uint64_t{9}, std::string{"request"}};
    auto params = std::vector<std::optional<JsonValue>>{
        std::nullopt,
        make_json(JsonValue::Object{{"key", make_json(std::string{"value"})}}
        ),
        make_json(JsonValue::Array{make_json(std::uint64_t{1})             }
        ),
        std::nullopt,
    };
    for (std::size_t index = 0; index < ids.size(); ++index) {
        connection.device->inject_input(request_frame(ids[index], "echo", params[index]));
    }

    auto values = take_values(*connection.device);
    REQUIRE(values.size() == ids.size());
    REQUIRE(params_seen == params);
    for (std::size_t index = 0; index < ids.size(); ++index) {
        auto response = require_response(values[index]);
        CHECK(response.id == ids[index]);
        CHECK(require_result(response) == params[index].value_or(make_json(JsonNull{})));
    }
}

TEST_CASE("Notifications never receive responses and keep the connection usable", "[rpc][server][dispatch]")
{
    Application app{0, nullptr};
    Server      server;
    auto        calls = 0;
    REQUIRE(server.register_method("ok", [&calls](auto const&, auto const&) {
        ++calls;
        return MethodResult::success(make_json(JsonNull{}));
    }));
    REQUIRE(server.register_method("failed", [&calls](auto const&, auto const&) {
        ++calls;
        return MethodResult::failure(RpcError{.code = 99, .message = "represented"});
    }));
    REQUIRE(server.register_method("throws", [&calls](auto const&, auto const&) -> MethodResult {
        ++calls;
        throw std::runtime_error{"private exception detail"};
    }));
    REQUIRE(server.register_method("invalid", [&calls](auto const&, auto const&) {
        ++calls;
        return MethodResult::success(make_json(std::numeric_limits<double>::infinity()));
    }));
    auto connection = attach(server);

    auto input = notification_frame("ok") + notification_frame("failed") + notification_frame("throws") +
                 notification_frame("invalid") + notification_frame("missing");
    connection.device->inject_input(input);
    CHECK(connection.device->written_data().empty());
    CHECK(server.connection_count() == 1U);
    CHECK(calls == 4);

    connection.device->inject_input(request_frame(std::uint64_t{1}, "ok"));
    auto values = take_values(*connection.device);
    REQUIRE(values.size() == 1U);
    CHECK(std::holds_alternative<JsonValue>(require_response(values.front()).payload));
}

TEST_CASE("Missing methods, represented failures, and exceptions become RPC errors", "[rpc][server][dispatch]")
{
    Application app{0, nullptr};
    Server      server;
    auto const  represented = RpcError{
        .code    = 9001,
        .message = "safe failure",
        .data    = make_json(JsonValue::Object{{"reason", make_json(std::string{"known"})}}
             ),
    };
    REQUIRE(server.register_method("fails", [represented](auto const&, auto const&) {
        return MethodResult::failure(represented);
    }));
    REQUIRE(server.register_method("throws", [](auto const&, auto const&) -> MethodResult {
        throw std::runtime_error{"must not reach peer"};
    }));
    auto connection = attach(server);

    connection.device->inject_input(request_frame(std::uint64_t{1}, "missing") +
                                    request_frame(std::uint64_t{2}, "fails") +
                                    request_frame(std::uint64_t{3}, "throws"));
    auto values = take_values(*connection.device);
    REQUIRE(values.size() == 3U);
    CHECK(require_error(require_response(values[0])).code == static_cast<std::int64_t>(ErrorCode::MethodNotFound));
    CHECK(require_error(require_response(values[1])) == represented);
    auto const internal = require_error(require_response(values[2]));
    CHECK(internal.code == static_cast<std::int64_t>(ErrorCode::InternalError));
    CHECK(internal.message == "Internal error");
    CHECK_FALSE(internal.data);
    CHECK(server.connection_count() == 1U);
}

TEST_CASE("Invalid handler output is replaced with Internal error", "[rpc][server][dispatch]")
{
    Application app{0, nullptr};
    Server      server;
    REQUIRE(server.register_method("bad-result", [](auto const&, auto const&) {
        return MethodResult::success(make_json(std::numeric_limits<double>::infinity()));
    }));
    REQUIRE(server.register_method("bad-error", [](auto const&, auto const&) {
        return MethodResult::failure(RpcError{
            .code    = 1,
            .message = std::string{1U, static_cast<char>(0xFF)},
        });
    }));
    auto connection = attach(server);

    connection.device->inject_input(request_frame(std::uint64_t{1}, "bad-result") +
                                    request_frame(std::uint64_t{2}, "bad-error"));
    auto values = take_values(*connection.device);
    REQUIRE(values.size() == 2U);
    for (auto const& value : values) {
        auto const error = require_error(require_response(value));
        CHECK(error.code == static_cast<std::int64_t>(ErrorCode::InternalError));
        CHECK(error.message == "Internal error");
    }
    CHECK(server.connection_count() == 1U);
}

TEST_CASE("Framing supports fragmentation, coalescing, and non-recursive input", "[rpc][server][reentrancy]")
{
    Application app{0, nullptr};
    Server      server;
    auto        depth         = 0;
    auto        maximum_depth = 0;
    auto        calls         = 0;
    REQUIRE(server.register_method("ordered", [&](auto const&, auto const&) {
        ++depth;
        maximum_depth = std::max(maximum_depth, depth);
        ++calls;
        --depth;
        return MethodResult::success(make_json(std::uint64_t{static_cast<std::uint64_t>(calls)}));
    }));

    auto  device = std::make_unique<MemoryIODevice>();
    auto* raw    = device.get();
    device->open();
    auto inject_second = true;
    raw->bytes_written.connect([raw, &inject_second](std::size_t) {
        if (inject_second) {
            inject_second = false;
            raw->inject_input(request_frame(std::uint64_t{2}, "ordered"));
        }
    });
    auto connection = attach(server, std::move(device));

    auto first = request_frame(std::uint64_t{1}, "ordered");
    connection.device->inject_input(first.substr(0U, 5U));
    CHECK(calls == 0);
    connection.device->inject_input(first.substr(5U));
    CHECK(calls == 2);
    CHECK(maximum_depth == 1);

    connection.device->inject_input(request_frame(std::uint64_t{3}, "ordered") +
                                    request_frame(std::uint64_t{4}, "ordered"));
    CHECK(calls == 4);
    auto values = take_values(*connection.device);
    REQUIRE(values.size() == 4U);
    CHECK(require_response(values[0]).id == RequestId{std::uint64_t{1}});
    CHECK(require_response(values[1]).id == RequestId{std::uint64_t{2}});
    CHECK(require_response(values[2]).id == RequestId{std::uint64_t{3}});
    CHECK(require_response(values[3]).id == RequestId{std::uint64_t{4}});
}

TEST_CASE("Parse and invalid-request errors keep a healthy connection open", "[rpc][server][protocol]")
{
    Application app{0, nullptr};
    Server      server;
    auto        protocol_errors = 0;
    server.connection_error.connect([&protocol_errors](ConnectionId, Error const&) { ++protocol_errors; });
    REQUIRE(server.register_method("ok", null_success_handler()));
    auto connection = attach(server);

    connection.device->inject_input(encode_frame(make_json(std::string{"not a request"})));
    connection.device->inject_input(frame_message("{broken").value());
    connection.device->inject_input(request_frame(std::uint64_t{1}, "ok"));

    auto values = take_values(*connection.device);
    REQUIRE(values.size() == 3U);
    auto invalid = require_response(values[0]);
    CHECK(invalid.id == RequestId{NullRequestId{}});
    CHECK(require_error(invalid).code == static_cast<std::int64_t>(ErrorCode::InvalidRequest));
    auto parse = require_response(values[1]);
    CHECK(parse.id == RequestId{NullRequestId{}});
    CHECK(require_error(parse).code == static_cast<std::int64_t>(ErrorCode::ParseError));
    CHECK(std::holds_alternative<JsonValue>(require_response(values[2]).payload));
    CHECK(server.connection_count() == 1U);
    CHECK(protocol_errors == 0);
}

TEST_CASE("Server preserves JSON-RPC batch response shapes and order", "[rpc][server][batch]")
{
    Application app{0, nullptr};
    Server      server;
    REQUIRE(server.register_method("echo", [](auto const&, std::optional<JsonValue> const& params) {
        return MethodResult::success(params.value_or(make_json(JsonNull{})));
    }));
    auto connection = attach(server);

    auto mixed = make_json(JsonValue::Array{
        encode_request(std::uint64_t{1}, "echo", make_json(JsonValue::Array{make_json(std::string{"first"})})),
        encode_notification("echo", make_json(JsonValue::Object{})),
        make_json(std::string{"invalid"}),
        encode_request(std::string{"last"}, "missing"),
    });
    connection.device->inject_input(encode_frame(mixed));
    auto values = take_values(*connection.device);
    REQUIRE(values.size() == 1U);
    REQUIRE(values.front().is_array());
    REQUIRE(values.front().as_array().size() == 3U);
    auto decoded = decode_response_document(values.front());
    REQUIRE(decoded);
    CHECK(decoded->kind == ResponseDocumentKind::Batch);
    REQUIRE(decoded->entries.size() == 3U);
    CHECK(decoded->entries[0].id == RequestId{std::uint64_t{1}});
    CHECK(require_result(decoded->entries[0]).as_array().front().as_string() == "first");
    CHECK(decoded->entries[1].id == RequestId{NullRequestId{}});
    CHECK(require_error(decoded->entries[1]).code == static_cast<std::int64_t>(ErrorCode::InvalidRequest));
    CHECK(decoded->entries[2].id == RequestId{std::string{"last"}});
    CHECK(require_error(decoded->entries[2]).code == static_cast<std::int64_t>(ErrorCode::MethodNotFound));

    auto one_response = make_json(JsonValue::Array{
        encode_notification("echo"),
        encode_request(std::uint64_t{2}, "echo"),
    });
    connection.device->inject_input(encode_frame(one_response));
    values = take_values(*connection.device);
    REQUIRE(values.size() == 1U);
    CHECK(values.front().is_array());
    CHECK(values.front().as_array().size() == 1U);

    auto notifications = make_json(JsonValue::Array{encode_notification("echo"), encode_notification("missing")});
    connection.device->inject_input(encode_frame(notifications));
    CHECK(connection.device->written_data().empty());

    connection.device->inject_input(encode_frame(make_json(JsonValue::Array{})));
    values = take_values(*connection.device);
    REQUIRE(values.size() == 1U);
    CHECK(values.front().is_object());
    CHECK(require_error(require_response(values.front())).code == static_cast<std::int64_t>(ErrorCode::InvalidRequest));
}

TEST_CASE("Server applies inclusive configured and default batch limits", "[rpc][server][batch]")
{
    SECTION("zero rejects non-empty batches without dispatch")
    {
        Application app{0, nullptr};
        Server      server{{.max_batch_entries = 0U}};
        auto        calls = 0;
        REQUIRE(server.register_method("call", [&calls](auto const&, auto const&) {
            ++calls;
            return MethodResult::success(make_json(JsonNull{}));
        }));
        auto connection = attach(server);
        connection.device->inject_input(
            encode_frame(make_json(JsonValue::Array{encode_request(std::uint64_t{1}, "call")})));
        CHECK(calls == 0);
        auto values = take_values(*connection.device);
        REQUIRE(values.size() == 1U);
        CHECK(values.front().is_object());
    }

    SECTION("exact configured count succeeds and one over is rejected atomically")
    {
        Application app{0, nullptr};
        Server      server{{.max_batch_entries = 2U}};
        auto        calls = 0;
        REQUIRE(server.register_method("call", [&calls](auto const&, auto const&) {
            ++calls;
            return MethodResult::success(make_json(JsonNull{}));
        }));
        auto connection = attach(server);
        auto exact      = JsonValue::Array{
            encode_request(std::uint64_t{1}, "call"),
            encode_notification("call"),
        };
        connection.device->inject_input(encode_frame(make_json(exact)));
        CHECK(calls == 2);
        auto values = take_values(*connection.device);
        REQUIRE(values.size() == 1U);
        CHECK(values.front().is_array());

        exact.push_back(encode_request(std::uint64_t{2}, "call"));
        connection.device->inject_input(encode_frame(make_json(exact)));
        CHECK(calls == 2);
        values = take_values(*connection.device);
        REQUIRE(values.size() == 1U);
        CHECK(values.front().is_object());
    }

    SECTION("the default accepts 64 and rejects 65")
    {
        Application app{0, nullptr};
        Server      server;
        auto        calls = 0;
        REQUIRE(server.register_method("call", [&calls](auto const&, auto const&) {
            ++calls;
            return MethodResult::success(make_json(JsonNull{}));
        }));
        auto connection = attach(server);
        auto batch      = JsonValue::Array{};
        batch.reserve(65U);
        for (auto index = 0U; index < 64U; ++index) {
            batch.push_back(encode_notification("call"));
        }
        connection.device->inject_input(encode_frame(make_json(batch)));
        CHECK(calls == 64);
        CHECK(connection.device->written_data().empty());

        batch.push_back(encode_request(std::uint64_t{1}, "call"));
        connection.device->inject_input(encode_frame(make_json(batch)));
        CHECK(calls == 64);
        auto values = take_values(*connection.device);
        REQUIRE(values.size() == 1U);
        CHECK(values.front().is_object());
    }
}

TEST_CASE("Server response text and framing are deterministic", "[rpc][server][framing]")
{
    Application app{0, nullptr};
    Server      server;
    REQUIRE(server.register_method("value", [](auto const&, auto const&) {
        return MethodResult::success(make_json(JsonValue::Object{
            {"z", make_json(std::uint64_t{2})},
            {"a", make_json(std::uint64_t{1})},
        }));
    }));
    auto connection = attach(server);

    connection.device->inject_input(request_frame(std::uint64_t{1}, "value"));
    auto expected = encode_frame(encode_success_response(
        std::uint64_t{
            1
    },
        make_json(JsonValue::Object{{"a", make_json(std::uint64_t{1})}, {"z", make_json(std::uint64_t{2})}})));
    CHECK(connection.device->written_data() == expected);
}

TEST_CASE("Every framing failure is terminal only for its connection", "[rpc][server][failure]")
{
    Application app{0, nullptr};
    Server      server;
    auto        events = std::vector<std::string>{};
    server.connection_error.connect([&events](ConnectionId, Error const& error) {
        events.push_back("error:" + error.code);
        CHECK(error.detail.empty());
    });
    server.connection_closed.connect([&events](ConnectionId) { events.emplace_back("closed"); });

    auto const failures = std::vector<std::pair<std::string, std::string>>{
        {std::string(16U * 1024U + 1U, 'x'), "rpc.framing.header_too_large"},
        {"Broken\r\n\r\n", "rpc.framing.invalid_header"},
        {"X-Test: value\r\n\r\n", "rpc.framing.missing_content_length"},
        {"Content-Length: 0\r\nContent-Length: 0\r\n\r\n", "rpc.framing.duplicate_content_length"},
        {"Content-Length: nope\r\n\r\n", "rpc.framing.invalid_content_length"},
        {"Content-Length: 1048577\r\n\r\n", "rpc.framing.body_too_large"},
        {"Content-Length: 0\r\nContent-Type: application/json; charset=latin1\r\n\r\n",
         "rpc.framing.unsupported_content_type"},
    };

    for (auto const& [input, code] : failures) {
        events.clear();
        auto connection = attach(server);
        connection.device->inject_input(input);
        CHECK(server.connection_count() == 0U);
        REQUIRE(events.size() == 2U);
        CHECK(events[0] == "error:" + code);
        CHECK(events[1] == "closed");
    }
}

TEST_CASE("Response framing failures close only the affected connection", "[rpc][server][failure]")
{
    SECTION("response body exceeds its inclusive limit")
    {
        Application app{0, nullptr};
        auto        options            = ServerOptions{};
        options.framing.max_body_bytes = 80U;
        Server server{options};
        REQUIRE(server.register_method("large", [](auto const&, auto const&) {
            return MethodResult::success(make_json(std::string(200U, 'x')));
        }));
        auto code = std::string{};
        server.connection_error.connect([&code](ConnectionId, Error const& error) { code = error.code; });
        auto connection = attach(server);
        connection.device->inject_input(request_frame(std::uint64_t{1}, "large"));
        CHECK(code == "rpc.framing.body_too_large");
        CHECK(server.connection_count() == 0U);

        REQUIRE(server.register_method("small", null_success_handler()));
        auto healthy = attach(server);
        healthy.device->inject_input(request_frame(std::uint64_t{2}, "small"));
        CHECK(take_values(*healthy.device).size() == 1U);
        CHECK(server.connection_count() == 1U);
    }

    SECTION("response header exceeds the request-compatible limit")
    {
        auto const  request     = request_frame(std::uint64_t{1}, "grow");
        auto const  header_size = request.find("\r\n\r\n") + 4U;
        Application app{0, nullptr};
        auto        options              = ServerOptions{};
        options.framing.max_header_bytes = header_size;
        Server server{options};
        REQUIRE(server.register_method("grow", [](auto const&, auto const&) {
            return MethodResult::success(make_json(std::string(200U, 'x')));
        }));
        auto code = std::string{};
        server.connection_error.connect([&code](ConnectionId, Error const& error) { code = error.code; });
        auto connection = attach(server);
        connection.device->inject_input(request);
        CHECK(code == "rpc.framing.header_too_large");
        CHECK(server.connection_count() == 0U);
    }
}

TEST_CASE("Short writes report an I/O failure before closing", "[rpc][server][failure]")
{
    Application app{0, nullptr};
    Server      server;
    REQUIRE(server.register_method("reply", null_success_handler()));
    auto events = std::vector<std::string>{};
    server.connection_error.connect([&events](ConnectionId, Error const& error) {
        events.emplace_back("error");
        CHECK(error.category == ErrorCategory::Io);
        CHECK(error.code == "rpc.short_write");
        CHECK(error.message == "RPC connection did not accept the complete frame");
        CHECK(error.detail.empty());
    });
    server.connection_closed.connect([&events](ConnectionId) { events.emplace_back("closed"); });

    for (auto const limit : {0U, 3U}) {
        events.clear();
        auto device = std::make_unique<MemoryIODevice>();
        device->open();
        device->set_write_limit(limit);
        auto connection = attach(server, std::move(device));
        connection.device->inject_input(request_frame(std::uint64_t{1}, "reply"));
        CHECK(server.connection_count() == 0U);
        CHECK(events == std::vector<std::string>{"error", "closed"});
        CHECK(connection.device->written_data().size() == limit);
    }
}

TEST_CASE("Queued output accounting enforces exact and one-over boundaries", "[rpc][server][output]")
{
    auto const frame_size = response_frame_size();

    SECTION("the exact limit succeeds")
    {
        Application app{0, nullptr};
        Server      server{{.max_queued_output_bytes = frame_size}};
        REQUIRE(server.register_method("reply", null_success_handler()));
        auto device = std::make_unique<MemoryIODevice>();
        device->open();
        device->set_auto_acknowledge_writes(false);
        auto connection = attach(server, std::move(device));
        connection.device->inject_input(request_frame(std::uint64_t{1}, "reply"));
        CHECK(server.connection_count() == 1U);
        CHECK(connection.device->unacknowledged_bytes() == frame_size);
    }

    SECTION("one byte below the required frame fails")
    {
        Application app{0, nullptr};
        Server      server{{.max_queued_output_bytes = frame_size - 1U}};
        REQUIRE(server.register_method("reply", null_success_handler()));
        auto error = Error{};
        auto seen  = false;
        server.connection_error.connect([&](ConnectionId, Error const& value) {
            error = value;
            seen  = true;
        });
        auto connection = attach(server);
        connection.device->inject_input(request_frame(std::uint64_t{1}, "reply"));
        REQUIRE(seen);
        CHECK(error.category == ErrorCategory::ResourceExhausted);
        CHECK(error.code == "rpc.output_limit");
        CHECK(error.message == "RPC connection output limit reached");
        CHECK(error.detail.empty());
        CHECK(connection.device->written_data().empty());
        CHECK(server.connection_count() == 0U);
    }

    SECTION("zero still permits notification-only traffic")
    {
        Application app{0, nullptr};
        Server      server{{.max_queued_output_bytes = 0U}};
        auto        calls = 0;
        REQUIRE(server.register_method("notify", [&calls](auto const&, auto const&) {
            ++calls;
            return MethodResult::success(make_json(JsonNull{}));
        }));
        auto connection = attach(server);
        connection.device->inject_input(notification_frame("notify"));
        CHECK(calls == 1);
        CHECK(connection.device->written_data().empty());
        CHECK(server.connection_count() == 1U);
    }
}

TEST_CASE("Acknowledgements drain only tracked queued bytes", "[rpc][server][output]")
{
    auto const frame_size = response_frame_size();

    SECTION("a partial delayed acknowledgement frees exactly that capacity")
    {
        Application app{0, nullptr};
        Server      server{{.max_queued_output_bytes = 2U * frame_size - 1U}};
        REQUIRE(server.register_method("reply", null_success_handler()));
        auto device = std::make_unique<MemoryIODevice>();
        device->open();
        device->set_auto_acknowledge_writes(false);
        auto connection = attach(server, std::move(device));

        connection.device->inject_input(request_frame(std::uint64_t{1}, "reply"));
        CHECK(connection.device->unacknowledged_bytes() == frame_size);
        connection.device->acknowledge_writes(1U);
        connection.device->inject_input(request_frame(std::uint64_t{2}, "reply"));
        CHECK(server.connection_count() == 1U);
        CHECK(connection.device->unacknowledged_bytes() == 2U * frame_size - 1U);

        connection.device->inject_input(request_frame(std::uint64_t{3}, "reply"));
        CHECK(server.connection_count() == 0U);
    }

    SECTION("synchronous acknowledgements leave no phantom queued bytes")
    {
        Application app{0, nullptr};
        Server      server{{.max_queued_output_bytes = frame_size}};
        REQUIRE(server.register_method("reply", null_success_handler()));
        auto connection = attach(server);
        connection.device->inject_input(request_frame(std::uint64_t{1}, "reply") +
                                        request_frame(std::uint64_t{2}, "reply"));
        CHECK(server.connection_count() == 1U);
        CHECK(connection.device->unacknowledged_bytes() == 0U);
        CHECK(take_values(*connection.device).size() == 2U);
    }

    SECTION("an oversized device acknowledgement clamps without underflow")
    {
        Application app{0, nullptr};
        Server      server{{.max_queued_output_bytes = frame_size}};
        REQUIRE(server.register_method("reply", null_success_handler()));
        auto device = std::make_unique<MemoryIODevice>();
        device->open();
        device->set_auto_acknowledge_writes(false);
        REQUIRE(device->write("untracked") == 9U);
        static_cast<void>(device->take_written_data());
        auto connection = attach(server, std::move(device));

        connection.device->inject_input(request_frame(std::uint64_t{1}, "reply"));
        connection.device->acknowledge_writes();
        CHECK(connection.device->unacknowledged_bytes() == 0U);
        connection.device->inject_input(request_frame(std::uint64_t{2}, "reply"));
        CHECK(server.connection_count() == 1U);
    }
}

TEST_CASE("Device errors use stable category mapping and signal order", "[rpc][server][failure]")
{
    Application app{0, nullptr};
    Server      server;
    auto        events   = std::vector<std::string>{};
    auto        observed = Error{};
    server.connection_error.connect([&](ConnectionId, Error const& error) {
        observed = error;
        events.emplace_back("error");
    });
    server.connection_closed.connect([&](ConnectionId) { events.emplace_back("closed"); });

    auto const cases = std::vector<std::pair<IOError, ErrorCategory>>{
        {IOError::InvalidArgument, ErrorCategory::InvalidArgument  },
        {IOError::Unsupported,     ErrorCategory::Unsupported      },
        {IOError::ResourceError,   ErrorCategory::ResourceExhausted},
        {IOError::ReadError,       ErrorCategory::Io               },
    };
    for (auto const& [io_error, category] : cases) {
        events.clear();
        auto connection = attach(server);
        connection.device->fail(io_error, "private device detail");
        CHECK(observed.category == category);
        CHECK(observed.code == "rpc.connection_closed");
        CHECK(observed.message == "RPC connection device failed");
        CHECK(observed.detail.empty());
        CHECK(events == std::vector<std::string>{"error", "closed"});
        CHECK(server.connection_count() == 0U);
    }

    events.clear();
    auto connection = attach(server);
    connection.device->fail(IOError::NoError, "ignored");
    CHECK(events.empty());
    CHECK(server.connection_count() == 1U);
    server.close();
}

TEST_CASE("Normal close operations are idempotent and emit no connection error", "[rpc][server][lifecycle]")
{
    Application app{0, nullptr};
    Server      server;
    auto        closed = std::vector<ConnectionId>{};
    auto        errors = 0;
    server.connection_closed.connect([&closed](ConnectionId id) { closed.push_back(id); });
    server.connection_error.connect([&errors](ConnectionId, Error const&) { ++errors; });

    server.close_connection(999U);
    auto first = attach(server);
    first.device->close();
    first.device->close();
    server.close_connection(first.id);
    CHECK(closed == std::vector<ConnectionId>{first.id});
    CHECK(errors == 0);
    CHECK(server.connection_count() == 0U);
}

TEST_CASE("Server close releases all connections and preserves registrations", "[rpc][server][lifecycle]")
{
    Application app{0, nullptr};
    Server      server;
    REQUIRE(server.register_method("still-there", null_success_handler()));
    auto first  = attach(server);
    auto second = attach(server);
    auto closed = std::vector<ConnectionId>{};
    server.connection_closed.connect([&closed](ConnectionId id) { closed.push_back(id); });

    server.close();
    server.close();
    CHECK(server.connection_count() == 0U);
    CHECK(closed == std::vector<ConnectionId>{first.id, second.id});
    CHECK(server.has_method("still-there"));

    auto third = attach(server);
    third.device->inject_input(request_frame(std::uint64_t{1}, "still-there"));
    auto values = take_values(*third.device);
    REQUIRE(values.size() == 1U);
    CHECK(std::holds_alternative<JsonValue>(require_response(values.front()).payload));
    server.close();
}

TEST_CASE("Closing from a handler aborts later batch dispatch safely", "[rpc][server][lifecycle]")
{
    Application app{0, nullptr};
    Server      server;
    auto        later_calls = 0;
    REQUIRE(server.register_method("close", [&server](RequestContext const& context, auto const&) {
        server.close_connection(context.connection_id);
        return MethodResult::success(make_json(JsonNull{}));
    }));
    REQUIRE(server.register_method("later", [&later_calls](auto const&, auto const&) {
        ++later_calls;
        return MethodResult::success(make_json(JsonNull{}));
    }));
    auto connection = attach(server);
    auto batch      = make_json(JsonValue::Array{
        encode_request(std::uint64_t{1}, "close"),
        encode_request(std::uint64_t{2}, "later"),
    });
    connection.device->inject_input(encode_frame(batch));
    CHECK(later_calls == 0);
    CHECK(server.connection_count() == 0U);
    CHECK(connection.device->written_data().empty());
}

TEST_CASE("A broken connection cannot affect another connection", "[rpc][server][isolation]")
{
    Application app{0, nullptr};
    Server      server;
    REQUIRE(server.register_method("ok", null_success_handler()));
    auto broken  = attach(server);
    auto healthy = attach(server);

    broken.device->inject_input("Broken\r\n\r\n");
    CHECK(server.connection_count() == 1U);
    healthy.device->inject_input(request_frame(std::uint64_t{1}, "ok"));
    auto values = take_values(*healthy.device);
    REQUIRE(values.size() == 1U);
    CHECK(std::holds_alternative<JsonValue>(require_response(values.front()).payload));
    CHECK(server.connection_count() == 1U);
}

TEST_CASE("Retired devices are deleted safely after event processing", "[rpc][server][lifecycle]")
{
    Application app{0, nullptr};
    Server      server;
    auto        destroyed = false;
    auto        device    = std::make_unique<MemoryIODevice>();
    device->open();
    device->destroyed.connect([&destroyed]() { destroyed = true; });
    auto connection = attach(server, std::move(device));

    server.close_connection(connection.id);
    CHECK(server.connection_count() == 0U);
    CHECK_FALSE(destroyed);
    app.process_events(EventFlag::Tasks);
    CHECK(destroyed);
}

TEST_CASE("Server destruction is safe for live and already-deferred devices", "[rpc][server][lifecycle]")
{
    Application app{0, nullptr};

    auto live_destroyed = false;
    {
        auto server = std::make_unique<Server>();
        auto device = std::make_unique<MemoryIODevice>();
        device->open();
        device->destroyed.connect([&live_destroyed]() { live_destroyed = true; });
        static_cast<void>(attach(*server, std::move(device)));
    }
    CHECK(live_destroyed);

    auto deferred_destroyed = false;
    {
        auto server = std::make_unique<Server>();
        auto device = std::make_unique<MemoryIODevice>();
        device->open();
        device->destroyed.connect([&deferred_destroyed]() { deferred_destroyed = true; });
        auto connection = attach(*server, std::move(device));
        server->close_connection(connection.id);
        CHECK_FALSE(deferred_destroyed);
    }
    CHECK(deferred_destroyed);
    app.process_events(EventFlag::Tasks);
    CHECK(deferred_destroyed);
}

// NOLINTEND(readability-magic-numbers)
