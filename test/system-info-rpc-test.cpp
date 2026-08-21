#include "system_info_rpc.hpp"

#include "application.hpp"
#include "framing.hpp"
#include "protocol_priv.hpp"
#include "server.hpp"
#include "support/memory_io_device.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

using namespace jb::core;
using namespace jb::jobu;
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

auto request_frame(std::uint64_t id, std::optional<JsonValue> const& params = std::nullopt) -> std::string
{
    auto serialized = serialize_json(encode_request(id, system_info_rpc_method_name(), params));
    REQUIRE(serialized);
    auto framed = frame_message(serialized.value());
    REQUIRE(framed);
    return std::move(framed).value();
}

auto take_response(MemoryIODevice& device) -> ResponseEnvelope
{
    StreamFramer framer;
    auto         bodies = framer.append(device.take_written_data());
    REQUIRE(bodies);
    REQUIRE(bodies->size() == 1U);

    auto parsed = parse_json(bodies->front());
    REQUIRE(parsed);
    auto decoded = decode_response_document(parsed.value());
    REQUIRE(decoded);
    REQUIRE(decoded->entries.size() == 1U);
    return std::move(decoded->entries.front());
}

auto require_result(ResponseEnvelope const& response) -> JsonValue const&
{
    REQUIRE(std::holds_alternative<JsonValue>(response.payload));
    return std::get<JsonValue>(response.payload);
}

auto require_error(ResponseEnvelope const& response) -> RpcError const&
{
    REQUIRE(std::holds_alternative<RpcError>(response.payload));
    return std::get<RpcError>(response.payload);
}

class RpcConnection {
public:
    explicit RpcConnection(Server& server)
    {
        auto device = std::make_unique<MemoryIODevice>();
        _device     = device.get();
        device->open();
        REQUIRE(server.add_connection(std::move(device)));
    }

    [[nodiscard]] auto call(std::optional<JsonValue> const& params = std::nullopt) -> ResponseEnvelope
    {
        _device->inject_input(request_frame(_next_id++, params));
        return take_response(*_device);
    }

private:
    MemoryIODevice* _device{nullptr};
    std::uint64_t   _next_id{1};
};

void require_info(ResponseEnvelope const& response)
{
    auto decoded = system_info_from_json(require_result(response));
    REQUIRE(decoded);
    CHECK(decoded->daemon_version == "1.2.3");
    CHECK(decoded->api_version.major == 1U);
    CHECK(decoded->api_version.minor == 1U);
    CHECK(decoded->capabilities == std::vector<std::string>{"queue.list", "system.info"});
}

} // anonymous namespace

TEST_CASE("system.info public registration owns its snapshot and ignores dispatched params", "[jobu][system-info][rpc]")
{
    Application app{0, nullptr};
    Server      server;

    CHECK(system_info_rpc_method_name() == "system.info");
    {
        auto info = SystemInfo{
            .daemon_version = "1.2.3",
            .api_version    = {.major = 1,    .minor = 1  },
            .capabilities   = {"system.info", "queue.list"},
        };
        REQUIRE(register_system_info_method(server, std::move(info)));
    }
    CHECK(server.has_method("system.info"));

    RpcConnection connection{server};
    require_info(connection.call());
    require_info(connection.call(make_json(JsonValue::Object{})));
    require_info(connection.call(make_json(JsonValue::Object{
        {"future", make_json(true)}
    })));
    require_info(connection.call(make_json(JsonValue::Array{make_json(std::string{"future"})})));
}

TEST_CASE("system.info primitive params remain transport-level invalid requests", "[jobu][system-info][rpc]")
{
    Application app{0, nullptr};
    Server      server;
    REQUIRE(register_system_info_method(server, SystemInfo{.daemon_version = "1.2.3"}));
    RpcConnection connection{server};

    auto const primitive_params = std::vector<JsonValue>{
        make_json(JsonNull{}),
        make_json(std::string{"scalar"}),
    };
    for (auto const& params : primitive_params) {
        auto        response = connection.call(params);
        auto const& error    = require_error(response);
        CHECK(error.code == static_cast<std::int64_t>(ErrorCode::InvalidRequest));
        CHECK(error.message == "Invalid Request");
        CHECK_FALSE(error.data);
    }
}

TEST_CASE("system.info public registration preserves an existing method", "[jobu][system-info][rpc]")
{
    Application app{0, nullptr};
    Server      server;
    REQUIRE(server.register_method(std::string{system_info_rpc_method_name()}, [](auto const&, auto const&) {
        return MethodResult::success(make_json(std::string{"original"}));
    }));

    CHECK_FALSE(register_system_info_method(server, SystemInfo{.daemon_version = "replacement"}));
    CHECK(server.has_method("system.info"));

    RpcConnection connection{server};
    auto          response = connection.call();
    auto const&   result   = require_result(response);
    REQUIRE(result.is_string());
    CHECK(result.as_string() == "original");
}
