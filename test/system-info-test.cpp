#include "system_info.hpp"

#include "system_info_priv.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

using namespace jb::core;
using namespace jb::jobu;
using namespace jb::rpc;

namespace {

auto make_json(auto value) -> JsonValue
{
    JsonValue result;
    result.data = std::move(value);
    return result;
}

auto valid_json() -> JsonValue
{
    return make_json(JsonValue::Object{
        {"api_version",
         make_json(JsonValue::Object{
             {"major", make_json(std::uint64_t{1})},
             {"minor", make_json(std::uint64_t{2})},
         })                                               },
        {"capabilities",
         make_json(JsonValue::Array{
             make_json(std::string{"system.info"}),
             make_json(std::string{"future.method"}),
         })                                               },
        {"daemon_version", make_json(std::string{"0.1.0"})},
    });
}

} // anonymous namespace

TEST_CASE("SystemInfo encodes the stable wire shape", "[jobu][system-info]")
{
    auto const encoded = system_info_to_json({
        .daemon_version = "0.1.0",
        .api_version    = {.major = 1, .minor = 0},
        .capabilities   = {"system.info", "alpha", "system.info"},
    });

    REQUIRE(encoded.is_object());
    auto const& object = encoded.as_object();
    REQUIRE(object.size() == 3U);
    CHECK(object.at("daemon_version").as_string() == "0.1.0");
    CHECK(object.at("api_version").as_object().at("major").as_uint() == 1U);
    CHECK(object.at("api_version").as_object().at("minor").as_uint() == 0U);
    REQUIRE(object.at("capabilities").as_array().size() == 2U);
    CHECK(object.at("capabilities").as_array()[0].as_string() == "alpha");
    CHECK(object.at("capabilities").as_array()[1].as_string() == "system.info");
}

TEST_CASE("SystemInfo decodes required fields and ignores unknown members", "[jobu][system-info]")
{
    auto encoded = valid_json();
    std::get<JsonValue::Object>(encoded.data).emplace("future", make_json(true));

    auto decoded = system_info_from_json(encoded);
    REQUIRE(decoded);
    CHECK(decoded->daemon_version == "0.1.0");
    CHECK(decoded->api_version.major == 1U);
    CHECK(decoded->api_version.minor == 2U);
    CHECK(decoded->capabilities == std::vector<std::string>{"future.method", "system.info"});
}

TEST_CASE("SystemInfo rejects missing and mistyped required fields", "[jobu][system-info]")
{
    auto check_invalid = [](JsonValue const& value) -> void {
        auto decoded = system_info_from_json(value);
        REQUIRE_FALSE(decoded);
        CHECK(decoded.error().category == ErrorCategory::InvalidArgument);
        CHECK(decoded.error().code == "jobu.system_info.invalid_response");
    };

    check_invalid(make_json(JsonNull{}));

    auto missing = valid_json();
    std::get<JsonValue::Object>(missing.data).erase("daemon_version");
    check_invalid(missing);

    auto  version = valid_json();
    auto& api     = std::get<JsonValue::Object>(std::get<JsonValue::Object>(version.data).at("api_version").data);
    api["major"]  = make_json(static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1U);
    check_invalid(version);

    auto capabilities = valid_json();
    std::get<JsonValue::Array>(std::get<JsonValue::Object>(capabilities.data).at("capabilities").data)
        .push_back(make_json(std::uint64_t{1}));
    check_invalid(capabilities);
}

TEST_CASE("system.info accepts no params or an empty object only", "[jobu][system-info][handler]")
{
    auto const info = SystemInfo{
        .daemon_version = "0.1.0",
        .capabilities   = {"system.info"},
    };

    CHECK(jb::jobu::detail::handle_system_info(info, std::nullopt));
    CHECK(jb::jobu::detail::handle_system_info(info, make_json(JsonValue::Object{})));

    auto const invalid_values = std::vector<JsonValue>{
        make_json(JsonValue::Object{{"unexpected", make_json(true)}}
        ),
        make_json(JsonValue::Array{                              }
        ),
        make_json(JsonNull{}
        ),
    };
    for (auto const& params : invalid_values) {
        auto result = jb::jobu::detail::handle_system_info(info, params);
        REQUIRE_FALSE(result);
        CHECK(result.error().code == static_cast<std::int64_t>(ErrorCode::InvalidParams));
        CHECK(result.error().message == "Invalid params");
    }
}
