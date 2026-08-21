#include "application.hpp"
#include "client.hpp"
#include "protocol_priv.hpp"
#include "server.hpp"
#include "support/memory_io_device.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
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

auto success_frame(RequestId const& id, JsonValue const& result = make_json(JsonNull{}), FramingLimits limits = {})
    -> std::string
{
    return encode_frame(encode_success_response(id, result), limits);
}

auto error_frame(RequestId const& id, RpcError const& error, FramingLimits limits = {}) -> std::string
{
    return encode_frame(encode_error_response(id, error), limits);
}

auto take_request_documents(MemoryIODevice& device) -> std::vector<RequestDocument>
{
    StreamFramer framer;
    auto         bodies = framer.append(device.take_written_data());
    REQUIRE(bodies);

    auto documents = std::vector<RequestDocument>{};
    documents.reserve(bodies->size());
    for (auto const& body : bodies.value()) {
        auto parsed = parse_json(body);
        REQUIRE(parsed);
        documents.push_back(decode_request_document(parsed.value()));
    }
    return documents;
}

auto require_request(RequestDocument const& document) -> RequestEnvelope const&
{
    REQUIRE(document.kind == RequestDocumentKind::Single);
    REQUIRE(document.entries.size() == 1U);
    REQUIRE(std::holds_alternative<RequestEnvelope>(document.entries.front()));
    return std::get<RequestEnvelope>(document.entries.front());
}

auto require_unsigned(RequestId const& id) -> std::uint64_t
{
    REQUIRE(std::holds_alternative<std::uint64_t>(id));
    return std::get<std::uint64_t>(id);
}

auto pump(MemoryIODevice& source, MemoryIODevice& destination) -> void
{
    auto bytes = source.take_written_data();
    if (!bytes.empty()) {
        destination.inject_input(bytes);
    }
}

} // anonymous namespace

TEST_CASE("Client public defaults and object contract are stable", "[rpc][client][api]")
{
    static_assert(std::is_base_of_v<Object, Client>);
    static_assert(!std::is_copy_constructible_v<Client>);
    static_assert(!std::is_move_constructible_v<Client>);

    auto const options = ClientOptions{};
    CHECK(options.framing.max_header_bytes == std::size_t{16} * 1024U);
    CHECK(options.framing.max_body_bytes == std::size_t{1024} * 1024U);
    CHECK(options.json.max_depth == 64U);
    CHECK(options.max_batch_entries == 64U);
    CHECK(options.max_pending_requests == 128U);
    CHECK(options.max_queued_output_bytes == std::size_t{2} * 1024U * 1024U);

    Application    app{0, nullptr};
    Object         parent;
    MemoryIODevice device;
    device.open();
    Client client{device, {}, &parent};
    CHECK(client.parent() == &parent);
    CHECK(client.pending_request_count() == 0U);
}

TEST_CASE("Calls encode deterministic envelopes and monotonically generated IDs", "[rpc][client][outbound]")
{
    Application    app{0, nullptr};
    MemoryIODevice device;
    device.open();
    Client client{device};

    auto object = make_json(JsonValue::Object{
        {"value", make_json(std::uint64_t{7})}
    });
    auto array  = make_json(JsonValue::Array{make_json(std::string{"item"})});
    auto first  = client.call("first");
    auto second = client.call("rpc.internal", object);
    auto third  = client.call("third", array);
    REQUIRE(first);
    REQUIRE(second);
    REQUIRE(third);
    CHECK(require_unsigned(first.value()) == 1U);
    CHECK(require_unsigned(second.value()) == 2U);
    CHECK(require_unsigned(third.value()) == 3U);
    CHECK(client.pending_request_count() == 3U);

    auto documents = take_request_documents(device);
    REQUIRE(documents.size() == 3U);
    auto const& first_request = require_request(documents[0]);
    REQUIRE(first_request.id);
    CHECK(*first_request.id == RequestId{std::uint64_t{1}});
    CHECK(first_request.method == "first");
    CHECK_FALSE(first_request.params);
    auto const& second_request = require_request(documents[1]);
    CHECK(second_request.method == "rpc.internal");
    REQUIRE(second_request.params);
    CHECK(*second_request.params == object);
    auto const& third_request = require_request(documents[2]);
    REQUIRE(third_request.params);
    CHECK(*third_request.params == array);
}

TEST_CASE("Notifications encode without IDs or pending entries", "[rpc][client][outbound]")
{
    Application    app{0, nullptr};
    MemoryIODevice device;
    device.open();
    Client client{device};
    auto   results = 0;
    client.result_received.connect([&](RequestId const&, JsonValue const&) { ++results; });

    REQUIRE(client.notify("none"));
    REQUIRE(client.notify("object", make_json(JsonValue::Object{})));
    REQUIRE(client.notify("array", make_json(JsonValue::Array{})));
    CHECK(client.pending_request_count() == 0U);
    CHECK(results == 0);

    auto documents = take_request_documents(device);
    REQUIRE(documents.size() == 3U);
    for (auto const& document : documents) {
        CHECK_FALSE(require_request(document).id);
    }
}

TEST_CASE("Local validation and codec failures are nonterminal and consume no ID", "[rpc][client][validation]")
{
    Application    app{0, nullptr};
    MemoryIODevice device;
    device.open();
    Client client{device};

    auto empty = client.call("");
    REQUIRE_FALSE(empty);
    CHECK(empty.error().category == ErrorCategory::InvalidArgument);
    CHECK(empty.error().code == "rpc.invalid_argument");
    CHECK(empty.error().message == "RPC method must not be empty");
    CHECK(empty.error().detail.empty());

    for (auto primitive : {make_json(JsonNull{}), make_json(true), make_json(std::uint64_t{1})}) {
        auto call = client.call("bad", primitive);
        REQUIRE_FALSE(call);
        CHECK(call.error().code == "rpc.invalid_argument");
        auto notification = client.notify("bad", primitive);
        REQUIRE_FALSE(notification);
        CHECK(notification.error().code == "rpc.invalid_argument");
    }

    auto invalid_method = std::string{1U, static_cast<char>(0xFF)};
    auto invalid_utf8   = client.call(invalid_method);
    REQUIRE_FALSE(invalid_utf8);
    CHECK(invalid_utf8.error().code == "rpc.json.invalid_utf8");
    auto non_finite = client.notify("bad-number", make_json(JsonValue::Array{make_json(std::nan(""))}));
    REQUIRE_FALSE(non_finite);
    CHECK(non_finite.error().code == "rpc.json.non_finite");

    auto constrained                   = ClientOptions{};
    constrained.framing.max_body_bytes = 1U;
    MemoryIODevice constrained_device;
    constrained_device.open();
    Client constrained_client{constrained_device, constrained};
    auto   framing = constrained_client.call("too-large");
    REQUIRE_FALSE(framing);
    CHECK(framing.error().code == "rpc.framing.body_too_large");

    CHECK(device.written_data().empty());
    CHECK(client.pending_request_count() == 0U);
    auto valid = client.call("valid");
    REQUIRE(valid);
    CHECK(valid.value() == RequestId{std::uint64_t{1}});
}

TEST_CASE("Outbound framing limits are inclusive at the complete request boundary", "[rpc][client][framing]")
{
    auto body = serialize_json(encode_request(std::uint64_t{1}, "boundary"));
    REQUIRE(body);
    auto frame = frame_message(body.value());
    REQUIRE(frame);
    auto const header_size = frame->find("\r\n\r\n") + 4U;

    SECTION("exact body and header limits succeed")
    {
        Application    app{0, nullptr};
        MemoryIODevice device;
        device.open();
        ClientOptions options;
        options.framing.max_body_bytes   = body->size();
        options.framing.max_header_bytes = header_size;
        Client client{device, options};
        REQUIRE(client.call("boundary"));
        CHECK(device.written_data() == frame.value());
    }

    SECTION("one below either limit fails without closing the client")
    {
        Application    app{0, nullptr};
        MemoryIODevice body_device;
        body_device.open();
        ClientOptions body_options;
        body_options.framing.max_body_bytes = body->size() - 1U;
        Client body_client{body_device, body_options};
        auto   body_failure = body_client.call("boundary");
        REQUIRE_FALSE(body_failure);
        CHECK(body_failure.error().code == "rpc.framing.body_too_large");
        CHECK(body_device.is_open());

        MemoryIODevice header_device;
        header_device.open();
        ClientOptions header_options;
        header_options.framing.max_header_bytes = header_size - 1U;
        Client header_client{header_device, header_options};
        auto   header_failure = header_client.call("boundary");
        REQUIRE_FALSE(header_failure);
        CHECK(header_failure.error().code == "rpc.framing.header_too_large");
        CHECK(header_device.is_open());
    }
}

TEST_CASE("Pending limits are inclusive and do not affect notifications", "[rpc][client][pending]")
{
    Application    app{0, nullptr};
    MemoryIODevice device;
    device.open();
    Client client{device, {.max_pending_requests = 2U}};

    REQUIRE(client.call("one"));
    REQUIRE(client.call("two"));
    auto over = client.call("three");
    REQUIRE_FALSE(over);
    CHECK(over.error().category == ErrorCategory::ResourceExhausted);
    CHECK(over.error().code == "rpc.pending_limit");
    CHECK(over.error().message == "RPC client pending request limit reached");
    REQUIRE(client.notify("still-allowed"));
    CHECK(client.pending_request_count() == 2U);
    client.cancel(RequestId{std::uint64_t{1}});
    auto after_failure = client.call("three");
    REQUIRE(after_failure);
    CHECK(after_failure.value() == RequestId{std::uint64_t{3}});

    MemoryIODevice zero_device;
    zero_device.open();
    Client zero{zero_device, {.max_pending_requests = 0U}};
    auto   rejected = zero.call("no-calls");
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().code == "rpc.pending_limit");
    REQUIRE(zero.notify("notification"));
}

TEST_CASE("Results and remote errors correlate exactly and out of order", "[rpc][client][correlation]")
{
    Application    app{0, nullptr};
    MemoryIODevice device;
    device.open();
    Client client{device};
    auto   events = std::vector<std::string>{};
    client.result_received.connect([&](RequestId const& id, JsonValue const& result) {
        events.push_back("result:" + std::to_string(require_unsigned(id)) + ":" + result.as_string());
        CHECK(client.pending_request_count() == 1U);
    });
    client.error_received.connect([&](RequestId const& id, RpcError const& error) {
        events.push_back("error:" + std::to_string(require_unsigned(id)) + ":" + std::to_string(error.code));
        CHECK(client.pending_request_count() == 0U);
    });

    REQUIRE(client.call("one"));
    REQUIRE(client.call("two"));
    static_cast<void>(device.take_written_data());
    device.inject_input(success_frame(std::uint64_t{2}, make_json(std::string{"second"})));
    device.inject_input(error_frame(std::uint64_t{1}, {.code = 42, .message = "remote"}));

    CHECK(events == std::vector<std::string>{"result:2:second", "error:1:42"});
    CHECK(client.pending_request_count() == 0U);
    CHECK(device.is_open());
    REQUIRE(client.call("still-open"));
}

TEST_CASE("Response batches are preflighted and delivered in wire order", "[rpc][client][batch]")
{
    Application    app{0, nullptr};
    MemoryIODevice device;
    device.open();
    Client client{device, {.max_batch_entries = 3U}};
    auto   events = std::vector<std::string>{};
    client.result_received.connect([&](RequestId const& id, JsonValue const&) {
        events.push_back("result:" + std::to_string(require_unsigned(id)));
    });
    client.error_received.connect([&](RequestId const& id, RpcError const&) {
        events.push_back("error:" + std::to_string(require_unsigned(id)));
    });

    REQUIRE(client.call("one"));
    REQUIRE(client.call("two"));
    REQUIRE(client.call("three"));
    static_cast<void>(device.take_written_data());
    auto batch = encode_batch({encode_success_response(std::uint64_t{3}, make_json(JsonNull{})),
                               encode_error_response(std::uint64_t{1}, {.code = 9, .message = "remote"}),
                               encode_success_response(std::uint64_t{2}, make_json(JsonNull{}))});
    REQUIRE(batch);
    device.inject_input(encode_frame(*batch));
    CHECK(events == std::vector<std::string>{"result:3", "error:1", "result:2"});
    CHECK(client.pending_request_count() == 0U);
}

TEST_CASE("Additive response members are accepted and completed IDs cannot repeat", "[rpc][client][correlation]")
{
    Application    app{0, nullptr};
    MemoryIODevice device;
    device.open();
    Client client{device};
    auto   results   = 0;
    auto   protocols = 0;
    client.result_received.connect([&](RequestId const& id, JsonValue const& value) {
        ++results;
        CHECK(id == RequestId{std::uint64_t{1}});
        CHECK(value.as_string() == "accepted");
    });
    client.protocol_error.connect([&](Error const& error) {
        ++protocols;
        CHECK(error.code == "rpc.protocol_error");
    });

    REQUIRE(client.call("one"));
    auto response = make_json(JsonValue::Object{
        {"future",  make_json(JsonValue::Object{{"ignored", make_json(true)}})},
        {"id",      make_json(std::uint64_t{1})                               },
        {"jsonrpc", make_json(std::string{"2.0"})                             },
        {"result",  make_json(std::string{"accepted"})                        },
    });
    auto frame    = encode_frame(response);
    device.inject_input(frame);
    CHECK(results == 1);
    CHECK(protocols == 0);

    device.inject_input(frame);
    CHECK(results == 1);
    CHECK(protocols == 1);
}

TEST_CASE("Invalid response batches never partially complete", "[rpc][client][batch][failure]")
{
    Application    app{0, nullptr};
    MemoryIODevice device;
    device.open();
    Client client{device};
    auto   events = std::vector<std::string>{};
    client.result_received.connect([&](RequestId const&, JsonValue const&) { events.emplace_back("result"); });
    client.protocol_error.connect([&](Error const& error) {
        events.push_back("protocol:" + error.code);
        client.close();
    });
    client.request_failed.connect([&](RequestId const& id, Error const& error) {
        events.push_back("failed:" + std::to_string(require_unsigned(id)) + ":" + error.code);
    });

    REQUIRE(client.call("one"));
    REQUIRE(client.call("two"));
    static_cast<void>(device.take_written_data());
    auto duplicate = encode_batch({encode_success_response(std::uint64_t{1}, make_json(JsonNull{})),
                                   encode_success_response(std::uint64_t{1}, make_json(JsonNull{}))});
    REQUIRE(duplicate);
    device.inject_input(encode_frame(*duplicate));

    CHECK(events == std::vector<std::string>{"protocol:rpc.protocol_error",
                                             "failed:1:rpc.protocol_error",
                                             "failed:2:rpc.protocol_error"});
    CHECK(client.pending_request_count() == 0U);
}

TEST_CASE("Cancellation is local and a later response becomes terminal", "[rpc][client][cancel]")
{
    Application    app{0, nullptr};
    MemoryIODevice device;
    device.open();
    Client client{device};
    auto   protocol_errors = 0;
    client.protocol_error.connect([&](Error const& error) {
        ++protocol_errors;
        CHECK(error.code == "rpc.protocol_error");
    });

    auto call = client.call("cancel-me");
    REQUIRE(call);
    auto written = device.written_data();
    client.cancel(RequestId{NullRequestId{}});
    client.cancel(RequestId{std::int64_t{1}});
    client.cancel(RequestId{std::string{"1"}});
    client.cancel(RequestId{std::uint64_t{999}});
    CHECK(client.pending_request_count() == 1U);
    client.cancel(call.value());
    CHECK(client.pending_request_count() == 0U);
    CHECK(device.written_data() == written);

    device.inject_input(success_frame(call.value()));
    CHECK(protocol_errors == 1);
    CHECK(device.is_open());
    auto closed = client.call("closed");
    REQUIRE_FALSE(closed);
    CHECK(closed.error().code == "rpc.connection_closed");
}

TEST_CASE("Every invalid or uncorrelatable response ID is terminal", "[rpc][client][correlation][failure]")
{
    auto const cases = std::vector<std::string>{
        success_frame(NullRequestId{}),
        success_frame(std::uint64_t{0}),
        success_frame(std::uint64_t{2}),
        success_frame(std::int64_t{-1}),
        success_frame(std::string{"1"}),
        frame_message(R"({"id":-0,"jsonrpc":"2.0","result":null})").value(),
        frame_message(R"({"id":1.0,"jsonrpc":"2.0","result":null})").value(),
    };
    for (auto const& response : cases) {
        Application    app{0, nullptr};
        MemoryIODevice device;
        device.open();
        Client client{device};
        auto   errors = 0;
        client.protocol_error.connect([&](Error const& error) {
            ++errors;
            CHECK(error.code == "rpc.protocol_error");
        });
        REQUIRE(client.call("one"));
        static_cast<void>(device.take_written_data());
        device.inject_input(response);
        CHECK(errors == 1);
        CHECK(client.pending_request_count() == 0U);
    }
}

TEST_CASE("Malformed response framing JSON and envelopes are terminal", "[rpc][client][failure]")
{
    auto const inputs = std::vector<std::pair<std::string, std::string>>{
        {"Broken\r\n\r\n",                                       "rpc.framing.invalid_header"},
        {frame_message("{broken").value(),                       "rpc.json.syntax"           },
        {encode_frame(make_json(std::string{"not a response"})), "rpc.protocol_error"        },
        {encode_frame(make_json(JsonValue::Array{})),            "rpc.protocol_error"        },
    };

    for (auto const& [input, expected_code] : inputs) {
        Application    app{0, nullptr};
        MemoryIODevice device;
        device.open();
        Client client{device};
        auto   events = std::vector<std::string>{};
        client.protocol_error.connect([&](Error const& error) {
            CHECK(error.detail.empty());
            events.push_back("protocol:" + error.code);
        });
        client.request_failed.connect(
            [&](RequestId const&, Error const& error) { events.push_back("failed:" + error.code); });
        REQUIRE(client.call("pending"));
        static_cast<void>(device.take_written_data());
        device.inject_input(input);
        device.inject_input(success_frame(std::uint64_t{1}));
        CHECK(events == std::vector<std::string>{"protocol:" + expected_code, "failed:" + expected_code});
    }
}

TEST_CASE("Response batch and JSON depth limits are enforced independently", "[rpc][client][limits]")
{
    SECTION("exact batch limit succeeds and one over fails")
    {
        Application    app{0, nullptr};
        MemoryIODevice device;
        device.open();
        Client client{device, {.max_batch_entries = 1U}};
        REQUIRE(client.call("one"));
        auto one = encode_batch({encode_success_response(std::uint64_t{1}, make_json(JsonNull{}))});
        REQUIRE(one);
        device.inject_input(encode_frame(*one));
        REQUIRE(client.call("two"));
        REQUIRE(client.call("three"));
        auto over = encode_batch({encode_success_response(std::uint64_t{2}, make_json(JsonNull{})),
                                  encode_success_response(std::uint64_t{3}, make_json(JsonNull{}))});
        REQUIRE(over);
        auto errors = 0;
        client.protocol_error.connect([&](Error const&) { ++errors; });
        device.inject_input(encode_frame(*over));
        CHECK(errors == 1);
    }

    SECTION("zero rejects a response batch but permits a single response")
    {
        Application    app{0, nullptr};
        MemoryIODevice device;
        device.open();
        Client client{device, {.max_batch_entries = 0U}};
        REQUIRE(client.call("single"));
        device.inject_input(success_frame(std::uint64_t{1}));
        REQUIRE(client.call("batch"));
        auto batch = encode_batch({encode_success_response(std::uint64_t{2}, make_json(JsonNull{}))});
        REQUIRE(batch);
        auto errors = 0;
        client.protocol_error.connect([&](Error const&) { ++errors; });
        device.inject_input(encode_frame(*batch));
        CHECK(errors == 1);
    }

    SECTION("JSON nesting limit is terminal inbound only")
    {
        Application    app{0, nullptr};
        MemoryIODevice device;
        device.open();
        ClientOptions options;
        options.json.max_depth = 1U;
        Client client{device, options};
        REQUIRE(client.call("nested"));
        auto errors = std::vector<std::string>{};
        client.protocol_error.connect([&](Error const& error) { errors.push_back(error.code); });
        device.inject_input(
            success_frame(std::uint64_t{1},
                          make_json(JsonValue::Array{make_json(JsonValue::Array{make_json(JsonNull{})})})));
        CHECK(errors == std::vector<std::string>{"rpc.json.depth_limit"});
    }
}

TEST_CASE("Fragmented and coalesced response frames are non-recursive", "[rpc][client][framing][reentrancy]")
{
    Application    app{0, nullptr};
    MemoryIODevice device;
    device.open();
    Client client{device};
    auto   depth         = 0;
    auto   maximum_depth = 0;
    auto   ids           = std::vector<std::uint64_t>{};
    client.result_received.connect([&](RequestId const& id, JsonValue const&) {
        ++depth;
        maximum_depth = std::max(maximum_depth, depth);
        ids.push_back(require_unsigned(id));
        if (ids.size() == 1U) {
            device.inject_input(success_frame(std::uint64_t{3}));
        }
        --depth;
    });
    REQUIRE(client.call("one"));
    REQUIRE(client.call("two"));
    REQUIRE(client.call("three"));
    static_cast<void>(device.take_written_data());

    auto first = success_frame(std::uint64_t{1});
    device.inject_input(first.substr(0U, 7U));
    CHECK(ids.empty());
    device.inject_input(first.substr(7U) + success_frame(std::uint64_t{2}));
    CHECK(ids == std::vector<std::uint64_t>{1U, 2U, 3U});
    CHECK(maximum_depth == 1);
}

TEST_CASE("Synchronous input during write waits until the call is pending", "[rpc][client][write][reentrancy]")
{
    Application    app{0, nullptr};
    MemoryIODevice device;
    device.open();
    auto inject = true;
    device.bytes_written.connect([&](std::size_t) {
        if (inject) {
            inject = false;
            device.inject_input(success_frame(std::uint64_t{1}, make_json(std::string{"done"})));
        }
    });
    Client client{device};
    auto   pending_during_signal = std::size_t{};
    auto   result                = std::string{};
    client.result_received.connect([&](RequestId const& id, JsonValue const& value) {
        CHECK(id == RequestId{std::uint64_t{1}});
        pending_during_signal = client.pending_request_count();
        result                = value.as_string();
    });

    auto call = client.call("sync");
    REQUIRE(call);
    CHECK(result == "done");
    CHECK(pending_during_signal == 0U);
    CHECK(client.pending_request_count() == 0U);
}

TEST_CASE("Nested writes are rejected without consuming an ID", "[rpc][client][write][reentrancy]")
{
    Application    app{0, nullptr};
    MemoryIODevice device;
    device.open();
    Client* client      = nullptr;
    auto    nested_code = std::string{};
    device.bytes_written.connect([&](std::size_t) {
        if (client != nullptr && nested_code.empty()) {
            auto nested = client->call("nested");
            REQUIRE_FALSE(nested);
            nested_code = nested.error().code;
        }
    });
    Client concrete{device};
    client = &concrete;

    auto outer = client->call("outer");
    REQUIRE(outer);
    CHECK(nested_code == "rpc.invalid_argument");
    CHECK(outer.value() == RequestId{std::uint64_t{1}});
    CHECK(client->pending_request_count() == 1U);
}

TEST_CASE("Queued output accounting observes inclusive boundaries and acknowledgements", "[rpc][client][output]")
{
    Application    sizing_app{0, nullptr};
    MemoryIODevice sizing_device;
    sizing_device.open();
    Client sizing_client{sizing_device};
    REQUIRE(sizing_client.call("one"));
    auto const frame_size = sizing_device.written_data().size();
    sizing_client.close();

    SECTION("exact limit succeeds and delayed acknowledgements free exact capacity")
    {
        MemoryIODevice device;
        device.open();
        device.set_auto_acknowledge_writes(false);
        Client client{device, {.max_queued_output_bytes = (std::size_t{2} * frame_size) - 1U}};
        auto   events = std::vector<std::string>{};
        client.protocol_error.connect([&](Error const& error) { events.push_back("protocol:" + error.code); });
        client.request_failed.connect([&](RequestId const& id, Error const& error) {
            events.push_back("failed:" + std::to_string(require_unsigned(id)) + ":" + error.code);
        });
        REQUIRE(client.call("one"));
        device.acknowledge_writes(1U);
        REQUIRE(client.call("one"));
        auto over = client.notify("one");
        REQUIRE_FALSE(over);
        CHECK(over.error().code == "rpc.output_limit");
        CHECK(events == std::vector<std::string>{"protocol:rpc.output_limit",
                                                 "failed:1:rpc.output_limit",
                                                 "failed:2:rpc.output_limit"});
    }

    SECTION("exact single-frame limit succeeds repeatedly with automatic acknowledgement")
    {
        MemoryIODevice device;
        device.open();
        Client client{device, {.max_queued_output_bytes = frame_size}};
        REQUIRE(client.call("one"));
        REQUIRE(client.call("one"));
    }

    SECTION("one under and zero are terminal output-limit failures")
    {
        for (auto const limit : std::vector<std::size_t>{0U, frame_size - 1U}) {
            MemoryIODevice device;
            device.open();
            Client client{device, {.max_queued_output_bytes = limit}};
            auto   codes = std::vector<std::string>{};
            client.protocol_error.connect([&](Error const& error) { codes.push_back(error.code); });
            auto result = client.call("one");
            REQUIRE_FALSE(result);
            CHECK(result.error().category == ErrorCategory::ResourceExhausted);
            CHECK(result.error().code == "rpc.output_limit");
            CHECK(codes == std::vector<std::string>{"rpc.output_limit"});
            CHECK(device.written_data().empty());
        }
    }

    SECTION("an acknowledgement larger than tracked client output clamps safely")
    {
        MemoryIODevice device;
        device.open();
        device.set_auto_acknowledge_writes(false);
        REQUIRE(device.write("untracked") == 9U);
        static_cast<void>(device.take_written_data());
        Client client{device, {.max_queued_output_bytes = frame_size}};
        REQUIRE(client.call("one"));
        device.acknowledge_writes();
        REQUIRE(client.call("one"));
    }
}

TEST_CASE("Short writes are terminal and fail only established pending calls", "[rpc][client][write][failure]")
{
    for (auto const limit : {0U, 3U}) {
        Application    app{0, nullptr};
        MemoryIODevice device;
        device.open();
        Client client{device};
        REQUIRE(client.call("pending"));
        static_cast<void>(device.take_written_data());
        device.set_write_limit(limit);
        auto events = std::vector<std::string>{};
        client.protocol_error.connect([&](Error const& error) { events.push_back("protocol:" + error.code); });
        client.request_failed.connect([&](RequestId const& id, Error const& error) {
            events.push_back("failed:" + std::to_string(require_unsigned(id)) + ":" + error.code);
        });

        auto attempted = client.call("short");
        REQUIRE_FALSE(attempted);
        CHECK(attempted.error().category == ErrorCategory::Io);
        CHECK(attempted.error().code == "rpc.short_write");
        CHECK(events == std::vector<std::string>{"protocol:rpc.short_write", "failed:1:rpc.short_write"});
        CHECK(device.written_data().size() == limit);
    }
}

TEST_CASE("Explicit close is idempotent and preserves the borrowed device", "[rpc][client][lifecycle]")
{
    Application    app{0, nullptr};
    MemoryIODevice device;
    device.open();
    auto events = std::vector<std::string>{};
    {
        Client client{device};
        REQUIRE(client.call("one"));
        REQUIRE(client.call("two"));
        client.request_failed.connect([&](RequestId const& id, Error const& error) {
            CHECK(error.category == ErrorCategory::Cancelled);
            CHECK(error.code == "rpc.connection_closed");
            CHECK(error.message == "RPC client was closed");
            CHECK(error.detail.empty());
            events.push_back(std::to_string(require_unsigned(id)));
        });
        client.close();
        client.close();
        client.cancel(RequestId{std::uint64_t{1}});
        CHECK(events == std::vector<std::string>{"1", "2"});
        CHECK(client.pending_request_count() == 0U);
        CHECK(device.is_open());
        auto call = client.call("closed");
        REQUIRE_FALSE(call);
        CHECK(call.error().category == ErrorCategory::Unavailable);
        CHECK(call.error().code == "rpc.connection_closed");
        auto notification = client.notify("closed");
        REQUIRE_FALSE(notification);
        CHECK(notification.error().code == "rpc.connection_closed");
    }
    CHECK(device.is_open());
    device.inject_input(success_frame(std::uint64_t{1}));
    CHECK(events == std::vector<std::string>{"1", "2"});
}

TEST_CASE("Client destruction disconnects and fails pending work without owning the device", "[rpc][client][lifecycle]")
{
    Application    app{0, nullptr};
    MemoryIODevice device;
    device.open();
    auto failures = std::vector<std::uint64_t>{};
    {
        Client client{device};
        REQUIRE(client.call("one"));
        client.request_failed.connect([&](RequestId const& id, Error const& error) {
            CHECK(error.category == ErrorCategory::Cancelled);
            failures.push_back(require_unsigned(id));
        });
    }

    CHECK(failures == std::vector<std::uint64_t>{1U});
    CHECK(device.is_open());
    device.inject_input(success_frame(std::uint64_t{1}));
    CHECK(failures == std::vector<std::uint64_t>{1U});
}

TEST_CASE("Device close and errors fail pending calls exactly once", "[rpc][client][lifecycle][failure]")
{
    SECTION("device close uses a stable unavailable error")
    {
        Application    app{0, nullptr};
        MemoryIODevice device;
        device.open();
        Client client{device};
        REQUIRE(client.call("one"));
        REQUIRE(client.call("two"));
        auto events = std::vector<std::string>{};
        client.request_failed.connect([&](RequestId const& id, Error const& error) {
            CHECK(error.category == ErrorCategory::Unavailable);
            CHECK(error.code == "rpc.connection_closed");
            CHECK(error.message == "RPC connection closed");
            events.push_back(std::to_string(require_unsigned(id)));
        });
        device.close();
        device.close();
        client.close();
        CHECK(events == std::vector<std::string>{"1", "2"});
    }

    SECTION("device error categories are mapped without private messages")
    {
        auto const cases = std::vector<std::pair<IOError, ErrorCategory>>{
            {IOError::InvalidArgument, ErrorCategory::InvalidArgument  },
            {IOError::Unsupported,     ErrorCategory::Unsupported      },
            {IOError::ResourceError,   ErrorCategory::ResourceExhausted},
            {IOError::ReadError,       ErrorCategory::Io               },
        };
        for (auto const& [io_error, category] : cases) {
            Application    app{0, nullptr};
            MemoryIODevice device;
            device.open();
            Client client{device};
            REQUIRE(client.call("pending"));
            auto failures  = 0;
            auto protocols = 0;
            client.protocol_error.connect([&](Error const&) { ++protocols; });
            client.request_failed.connect([&](RequestId const&, Error const& error) {
                ++failures;
                CHECK(error.category == category);
                CHECK(error.code == "rpc.connection_closed");
                CHECK(error.message == "RPC connection device failed");
                CHECK(error.detail.empty());
            });
            device.fail(IOError::NoError, "ignored");
            CHECK(failures == 0);
            device.fail(io_error, "private device detail");
            device.close();
            CHECK(failures == 1);
            CHECK(protocols == 0);
        }
    }
}

TEST_CASE("Completion listeners can cancel close and create calls safely", "[rpc][client][reentrancy]")
{
    SECTION("canceling a later preflighted response suppresses its completion")
    {
        Application    app{0, nullptr};
        MemoryIODevice device;
        device.open();
        Client client{device};
        REQUIRE(client.call("one"));
        REQUIRE(client.call("two"));
        auto completed = std::vector<std::uint64_t>{};
        client.result_received.connect([&](RequestId const& id, JsonValue const&) {
            auto value = require_unsigned(id);
            completed.push_back(value);
            if (value == 1U) {
                client.cancel(RequestId{std::uint64_t{2}});
                auto added = client.call("three");
                REQUIRE(added);
                CHECK(added.value() == RequestId{std::uint64_t{3}});
            }
        });
        auto batch = encode_batch({encode_success_response(std::uint64_t{1}, make_json(JsonNull{})),
                                   encode_success_response(std::uint64_t{2}, make_json(JsonNull{}))});
        REQUIRE(batch);
        device.inject_input(encode_frame(*batch));
        CHECK(completed == std::vector<std::uint64_t>{1U});
        CHECK(client.pending_request_count() == 1U);
    }

    SECTION("closing during batch delivery fails later pending IDs and stops delivery")
    {
        Application    app{0, nullptr};
        MemoryIODevice device;
        device.open();
        Client client{device};
        REQUIRE(client.call("one"));
        REQUIRE(client.call("two"));
        REQUIRE(client.call("three"));
        auto events = std::vector<std::string>{};
        client.result_received.connect([&](RequestId const& id, JsonValue const&) {
            events.push_back("result:" + std::to_string(require_unsigned(id)));
            client.close();
        });
        client.request_failed.connect([&](RequestId const& id, Error const&) {
            events.push_back("failed:" + std::to_string(require_unsigned(id)));
        });
        auto batch = encode_batch({encode_success_response(std::uint64_t{1}, make_json(JsonNull{})),
                                   encode_success_response(std::uint64_t{2}, make_json(JsonNull{}))});
        REQUIRE(batch);
        device.inject_input(encode_frame(*batch));
        CHECK(events == std::vector<std::string>{"result:1", "failed:2", "failed:3"});
    }
}

TEST_CASE("Client and Server complete transport-independent in-memory round trips", "[rpc][client][integration]")
{
    Application app{0, nullptr};
    Server      server;
    REQUIRE(server.register_method("echo", [](auto const&, std::optional<JsonValue> const& params) {
        return MethodResult::success(params.value_or(make_json(JsonNull{})));
    }));
    REQUIRE(server.register_method("failed", [](auto const&, auto const&) {
        return MethodResult::failure(RpcError{.code = 77, .message = "remote failure"});
    }));
    auto notifications = 0;
    REQUIRE(server.register_method("notify", [&](auto const&, auto const&) {
        ++notifications;
        return MethodResult::success(make_json(JsonNull{}));
    }));

    auto  server_device = std::make_unique<MemoryIODevice>();
    auto* server_raw    = server_device.get();
    server_device->open();
    REQUIRE(server.add_connection(std::move(server_device)));

    MemoryIODevice client_device;
    client_device.open();
    Client client{client_device};
    auto   events = std::vector<std::string>{};
    client.result_received.connect([&](RequestId const& id, JsonValue const& value) {
        events.push_back("result:" + std::to_string(require_unsigned(id)) + ":" + value.as_array().front().as_string());
    });
    client.error_received.connect([&](RequestId const& id, RpcError const& error) {
        events.push_back("error:" + std::to_string(require_unsigned(id)) + ":" + std::to_string(error.code));
    });

    REQUIRE(client.call("echo", make_json(JsonValue::Array{make_json(std::string{"first"})})));
    pump(client_device, *server_raw);
    auto response = server_raw->take_written_data();
    auto split    = response.size() / 2U;
    client_device.inject_input(std::string_view{response}.substr(0U, split));
    client_device.inject_input(std::string_view{response}.substr(split));
    REQUIRE(events.size() == 1U);
    CHECK(events.front() == "result:1:first");

    REQUIRE(client.call("failed"));
    REQUIRE(client.notify("notify"));
    pump(client_device, *server_raw);
    CHECK(notifications == 1);
    pump(*server_raw, client_device);
    CHECK(events == std::vector<std::string>{"result:1:first", "error:2:77"});
    CHECK(client.pending_request_count() == 0U);
}

TEST_CASE("Several server responses may return to the client out of order", "[rpc][client][integration]")
{
    Application app{0, nullptr};
    Server      server;
    REQUIRE(server.register_method("echo", [](auto const&, std::optional<JsonValue> const& params) {
        return MethodResult::success(params.value_or(make_json(JsonNull{})));
    }));
    auto  server_device = std::make_unique<MemoryIODevice>();
    auto* server_raw    = server_device.get();
    server_device->open();
    REQUIRE(server.add_connection(std::move(server_device)));

    MemoryIODevice client_device;
    client_device.open();
    Client client{client_device};
    auto   ids = std::vector<std::uint64_t>{};
    client.result_received.connect([&](RequestId const& id, JsonValue const&) { ids.push_back(require_unsigned(id)); });
    REQUIRE(client.call("echo", make_json(JsonValue::Array{make_json(std::string{"one"})})));
    REQUIRE(client.call("echo", make_json(JsonValue::Array{make_json(std::string{"two"})})));
    pump(client_device, *server_raw);

    StreamFramer response_framer;
    auto         bodies = response_framer.append(server_raw->take_written_data());
    REQUIRE(bodies);
    REQUIRE(bodies->size() == 2U);
    client_device.inject_input(frame_message((*bodies)[1]).value() + frame_message((*bodies)[0]).value());
    CHECK(ids == std::vector<std::uint64_t>{2U, 1U});
    CHECK(client.pending_request_count() == 0U);
}

// NOLINTEND(readability-magic-numbers)
