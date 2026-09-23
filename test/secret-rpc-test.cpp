#include "secret_json.hpp"
#include "secret_rpc.hpp"

#include "application.hpp"
#include "framing.hpp"
#include "protocol_priv.hpp"
#include "secret_repository_priv.hpp"
#include "secret_service.hpp"
#include "server.hpp"
#include "support/fake_time_source.hpp"
#include "support/memory_io_device.hpp"
#include "support/recovery_fixture.hpp"
#include "transaction.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

using namespace jb::core;
using namespace jb::db;
using namespace jb::jobu;
using namespace jb::rpc;
using namespace jb::rpc::detail;
using namespace jb::test;
using namespace std::chrono_literals;

namespace {

auto json(std::string_view text) -> JsonValue
{
    auto parsed = parse_json(text);
    REQUIRE(parsed);
    return std::move(parsed).value();
}

auto result(ResponseEnvelope const& response) -> JsonValue const&
{
    REQUIRE(std::holds_alternative<JsonValue>(response.payload));
    return std::get<JsonValue>(response.payload);
}

auto error(ResponseEnvelope const& response) -> RpcError const&
{
    REQUIRE(std::holds_alternative<RpcError>(response.payload));
    return std::get<RpcError>(response.payload);
}

void check_application_error(ResponseEnvelope const& response, std::string_view category, std::string_view code)
{
    auto const& failure = error(response);
    CHECK(failure.code == static_cast<std::int64_t>(ErrorCode::ApplicationError));
    REQUIRE(failure.data);
    REQUIRE(failure.data->is_object());
    auto const& data = failure.data->as_object();
    REQUIRE(data.size() == 2U);
    CHECK(data.at("category").as_string() == category);
    CHECK(data.at("code").as_string() == code);
}

void check_invalid_params(ResponseEnvelope const& response)
{
    auto const& failure = error(response);
    CHECK(failure.code == static_cast<std::int64_t>(ErrorCode::InvalidParams));
    CHECK_FALSE(failure.data);
}

struct Fixture {
    Application                        app{0, nullptr};
    RecoveryFixture                    storage;
    FakeTimeSource                     time;
    SecretService                      service{storage.database, time};
    jb::jobu::detail::SecretRepository repository{storage.database};
    Server                             server;
    MemoryIODevice*                    peer{};
    std::string                        last_response;
    std::uint64_t                      next_id{1};

    Fixture()
    {
        time.set_utc(UtcTimePoint{10s});
        REQUIRE(register_secret_methods(server, service));

        auto device = std::make_unique<MemoryIODevice>();
        peer        = device.get();
        device->open();
        REQUIRE(server.add_connection(std::move(device)));
    }

    auto call(std::string_view method, std::optional<JsonValue> params) -> ResponseEnvelope
    {
        auto request    = encode_request(next_id++, method, std::move(params));
        auto serialized = serialize_json(request);
        REQUIRE(serialized);
        auto framed = frame_message(*serialized);
        REQUIRE(framed);
        peer->inject_input(*framed);

        StreamFramer framer;
        auto         bodies = framer.append(peer->take_written_data());
        REQUIRE(bodies);
        REQUIRE(bodies->size() == 1U);
        last_response = std::move(bodies->front());

        auto parsed = parse_json(last_response);
        REQUIRE(parsed);
        auto decoded = decode_response_document(*parsed);
        REQUIRE(decoded);
        REQUIRE(decoded->entries.size() == 1U);
        return std::move(decoded->entries.front());
    }

    void seed_current_reference(std::string_view secret_name)
    {
        auto queue = recovery_queue(recovery_id(1));
        storage.insert_queue(queue);
        auto job = storage.make_job(recovery_id(2), queue.id);
        storage.insert_job(job);

        auto begun = Transaction::begin(storage.database);
        REQUIRE(begun);
        auto transaction = std::move(begun).value();
        auto references  = std::array{
            jb::jobu::detail::SecretReference{.secret_name = std::string{secret_name}, .field_path = "/arguments/0"}
        };
        REQUIRE(repository.replace_references_for_job(job.id, references));
        REQUIRE(transaction.commit());
    }
};

} // namespace

TEST_CASE("Secret JSON accepts bounded UTF-8 and canonical base64 without changing raw bytes", "[secret][rpc]")
{
    auto binary = SetSecretRequest{
        .name  = "token",
        .value = {std::byte{0x00}, std::byte{0xff}, std::byte{0x7f}}
    };
    auto wire = set_secret_request_to_json(binary);
    REQUIRE(wire);
    CHECK(wire->as_object().at("value").as_object().at("data").as_string() == "AP9/");
    auto decoded = set_secret_request_from_json(*wire);
    REQUIRE(decoded);
    CHECK(decoded->value == binary.value);

    auto text = set_secret_request_from_json(json(R"({"name":"token","value":{"encoding":"utf8","data":"Tere!"}})"));
    REQUIRE(text);
    CHECK(text->value.size() == 5U);

    for (auto const* malformed : {
             R"({"name":"token","value":{"encoding":"base64","data":"Zh=="}})",
             R"({"name":"token","value":{"encoding":"base64","data":"Zg="}})",
             R"({"name":"token","value":{"encoding":"base64","data":"Zg==\n"}})",
             R"({"name":"token","value":{"encoding":"base64","data":"Zg==","extra":1}})",
             R"({"name":"token","value":{"encoding":"hex","data":"ff"}})",
         }) {
        auto invalid = set_secret_request_from_json(json(malformed));
        REQUIRE_FALSE(invalid);
        CHECK(invalid.error().code == "jobu.protocol.invalid_request");
    }

    auto large_utf8 = JsonValue::Object{
        {"name",  JsonValue{.data = std::string{"token"}}},
        {"value",
         JsonValue{.data =
                       JsonValue::Object{
                           {"encoding", JsonValue{.data = std::string{"utf8"}}},
                           {"data", JsonValue{.data = std::string(65'537, 'x')}},
                       }}                                },
    };
    auto oversized = set_secret_request_from_json(JsonValue{.data = std::move(large_utf8)});
    REQUIRE_FALSE(oversized);
    CHECK(oversized.error().code == "jobu.secret.too_large");

    auto max_base64 =
        set_secret_request_from_json(json(std::string{R"({"name":"token","value":{"encoding":"base64","data":")"} +
                                          std::string(87'380, 'A') + "AA==" + R"("}})"));
    REQUIRE(max_base64);
    CHECK(max_base64->value.size() == 65'536U);

    auto large_base64 =
        set_secret_request_from_json(json(std::string{R"({"name":"token","value":{"encoding":"base64","data":")"} +
                                          std::string(87'380, 'A') + "AAA=" + R"("}})"));
    REQUIRE_FALSE(large_base64);
    CHECK(large_base64.error().code == "jobu.secret.too_large");
}

TEST_CASE("Secret RPC sets rotates lists and deletes without returning values", "[secret][rpc]")
{
    Fixture fixture;
    CHECK_FALSE(fixture.server.has_method("secret.get"));
    CHECK(fixture.server.has_method("secret.set"));
    CHECK(fixture.server.has_method("secret.list"));
    CHECK(fixture.server.has_method("secret.delete"));

    auto empty    = fixture.call("secret.set", json(R"({"name":"alpha","value":{"encoding":"utf8","data":""}})"));
    auto metadata = secret_metadata_from_json(result(empty));
    REQUIRE(metadata);
    CHECK(metadata->name == "alpha");
    CHECK(result(empty).as_object().size() == 3U);
    CHECK(fixture.last_response.find("value") == std::string::npos);

    auto first = fixture.repository.find_value("alpha");
    REQUIRE(first);
    CHECK(first->empty());

    fixture.time.set_utc(UtcTimePoint{20s});
    auto rotated = fixture.call("secret.set", json(R"({"name":"alpha","value":{"encoding":"base64","data":"AP9/"}})"));
    auto current = secret_metadata_from_json(result(rotated));
    REQUIRE(current);
    CHECK(current->created_at == metadata->created_at);
    CHECK(current->updated_at > metadata->updated_at);
    CHECK(fixture.last_response.find("AP9/") == std::string::npos);

    auto stored = fixture.repository.find_value("alpha");
    REQUIRE(stored);
    CHECK(*stored == ByteBuffer{std::byte{0x00}, std::byte{0xff}, std::byte{0x7f}});

    auto beta = fixture.call("secret.set", json(R"({"name":"beta","value":{"encoding":"utf8","data":"sentinel"}})"));
    REQUIRE(secret_metadata_from_json(result(beta)));
    auto first_page = fixture.call("secret.list", json(R"({"limit":1})"));
    auto page       = secret_page_from_json(result(first_page));
    REQUIRE(page);
    REQUIRE(page->items.size() == 1U);
    CHECK(page->items.front().name == "alpha");
    REQUIRE(page->next_after_name);
    CHECK(*page->next_after_name == "alpha");
    CHECK(fixture.last_response.find("sentinel") == std::string::npos);

    auto next_page = fixture.call("secret.list", json(R"({"limit":1,"after_name":"alpha"})"));
    auto next      = secret_page_from_json(result(next_page));
    REQUIRE(next);
    REQUIRE(next->items.size() == 1U);
    CHECK(next->items.front().name == "beta");
    CHECK_FALSE(next->next_after_name);

    auto deleted = fixture.call("secret.delete", json(R"({"name":"alpha"})"));
    CHECK(result(deleted).is_null());
    check_application_error(fixture.call("secret.delete", json(R"({"name":"alpha"})")),
                            "not_found",
                            "jobu.secret.not_found");
}

TEST_CASE("Secret RPC preserves validation conflicts and stopped mutation admission", "[secret][rpc]")
{
    Fixture fixture;
    for (auto const* malformed : {
             R"({"name":"token","value":{"encoding":"base64","data":"Zg="}})",
             R"({"name":"token","value":{"encoding":"hex","data":"ff"}})",
             R"({"name":"token","value":{"encoding":"utf8","data":"x"},"extra":1})",
             R"({"name":"token","value":{"encoding":"utf8"}})",
         }) {
        check_invalid_params(fixture.call("secret.set", json(malformed)));
    }
    check_invalid_params(fixture.call("secret.list", json(R"({"limit":null})")));
    check_invalid_params(fixture.call("secret.delete", std::nullopt));
    check_application_error(fixture.call("secret.list", json(R"({"limit":201})")),
                            "invalid_argument",
                            "jobu.storage.invalid_limit");

    auto oversized = json(std::string{R"({"name":"token","value":{"encoding":"base64","data":")"} +
                          std::string(87'380, 'A') + "AAA=" + R"("}})");
    check_application_error(fixture.call("secret.set", std::move(oversized)),
                            "resource_exhausted",
                            "jobu.secret.too_large");
    CHECK(fixture.last_response.find(std::string(32, 'A')) == std::string::npos);

    auto set = fixture.call("secret.set", json(R"({"name":"token","value":{"encoding":"utf8","data":"sentinel"}})"));
    REQUIRE(secret_metadata_from_json(result(set)));
    fixture.seed_current_reference("token");
    check_application_error(fixture.call("secret.delete", json(R"({"name":"token"})")),
                            "conflict",
                            "jobu.secret.in_use");
    CHECK(fixture.last_response.find("sentinel") == std::string::npos);

    fixture.service.stop_mutations();
    check_application_error(
        fixture.call("secret.set", json(R"({"name":"other","value":{"encoding":"utf8","data":"x"}})")),
        "unavailable",
        "jobu.service.stopping");
    check_application_error(fixture.call("secret.delete", json(R"({"name":"token"})")),
                            "unavailable",
                            "jobu.service.stopping");
    CHECK(secret_page_from_json(result(fixture.call("secret.list", json(R"({})")))));
}
