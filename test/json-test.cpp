#include "json.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

using namespace jb::core;
using namespace jb::rpc;

namespace {

template <typename T>
auto make_json(T value) -> JsonValue
{
    JsonValue result;
    result.data = std::move(value);
    return result;
}

auto contains(std::string const& text, std::string_view marker) -> bool
{
    return text.find(marker) != std::string::npos;
}

} // anonymous namespace

TEST_CASE("JSON values expose owning dependency-independent alternatives", "[rpc][json]")
{
    static_assert(std::is_same_v<JsonValue::Array, std::vector<JsonValue>>);
    static_assert(std::is_same_v<JsonValue::Object, std::map<std::string, JsonValue, std::less<>>>);

    JsonValue const  value;
    JsonLimits const limits;

    CHECK(std::holds_alternative<JsonNull>(value.data));
    CHECK(limits.max_depth == 64);

    auto text = std::string{"owned"};
    auto json = make_json(text);
    text.clear();
    CHECK(std::get<std::string>(json.data) == "owned");
}

TEST_CASE("JSON values report their active alternatives", "[rpc][json]")
{
    auto check_flags = [](JsonValue const& value,
                          bool             is_null,
                          bool             is_bool,
                          bool             is_int,
                          bool             is_uint,
                          bool             is_double,
                          bool             is_string,
                          bool             is_array,
                          bool             is_object) {
        CHECK(value.is_null() == is_null);
        CHECK(value.is_bool() == is_bool);
        CHECK(value.is_int() == is_int);
        CHECK(value.is_uint() == is_uint);
        CHECK(value.is_double() == is_double);
        CHECK(value.is_string() == is_string);
        CHECK(value.is_array() == is_array);
        CHECK(value.is_object() == is_object);
    };

    JsonValue const null_value;
    check_flags(null_value, true, false, false, false, false, false, false, false);

    check_flags(make_json(false), false, true, false, false, false, false, false, false);
    check_flags(make_json(std::int64_t{-1}), false, false, true, false, false, false, false, false);
    check_flags(make_json(std::uint64_t{1}), false, false, false, true, false, false, false, false);
    check_flags(make_json(1.5), false, false, false, false, true, false, false, false);
    check_flags(make_json(std::string{"text"}), false, false, false, false, false, true, false, false);
    check_flags(make_json(JsonValue::Array{}), false, false, false, false, false, false, true, false);
    check_flags(make_json(JsonValue::Object{}), false, false, false, false, false, false, false, true);
}

TEST_CASE("JSON parsing preserves scalar alternatives", "[rpc][json]")
{
    SECTION("null")
    {
        auto value = parse_json("null");
        REQUIRE(value);
        CHECK(std::holds_alternative<JsonNull>(value->data));
    }

    SECTION("booleans")
    {
        auto false_value = parse_json("false");
        auto true_value  = parse_json("true");
        REQUIRE(false_value);
        REQUIRE(true_value);
        CHECK_FALSE(std::get<bool>(false_value->data));
        CHECK(std::get<bool>(true_value->data));
    }

    SECTION("signed integer")
    {
        auto value = parse_json("-42");
        REQUIRE(value);
        REQUIRE(std::holds_alternative<std::int64_t>(value->data));
        CHECK(std::get<std::int64_t>(value->data) == -42);
    }

    SECTION("unsigned integer")
    {
        auto value = parse_json("42");
        REQUIRE(value);
        REQUIRE(std::holds_alternative<std::uint64_t>(value->data));
        CHECK(std::get<std::uint64_t>(value->data) == 42);
    }

    SECTION("floating point")
    {
        auto value = parse_json("1.5");
        REQUIRE(value);
        REQUIRE(std::holds_alternative<double>(value->data));
        CHECK(std::get<double>(value->data) == 1.5);
    }

    SECTION("string")
    {
        auto value = parse_json(R"("text")");
        REQUIRE(value);
        REQUIRE(std::holds_alternative<std::string>(value->data));
        CHECK(std::get<std::string>(value->data) == "text");
    }
}

TEST_CASE("JSON round trips every value alternative in nested containers", "[rpc][json]")
{
    auto const expected = make_json(JsonValue::Object{
        {"array",
         make_json(JsonValue::Array{
             make_json(JsonNull{}),
             make_json(false),
             make_json(std::int64_t{-7}),
             make_json(std::uint64_t{9}),
             make_json(2.5),
             make_json(std::string{"text"}),
             make_json(JsonValue::Array{}),
             make_json(JsonValue::Object{}),
         })                                                                  },
        {"nested", make_json(JsonValue::Object{{"enabled", make_json(true)}})},
    });

    auto encoded = serialize_json(expected);
    REQUIRE(encoded);

    auto decoded = parse_json(*encoded);
    REQUIRE(decoded);
    CHECK(*decoded == expected);
}

TEST_CASE("JSON parsing decodes escapes and Unicode", "[rpc][json]")
{
    auto value = parse_json(R"({"escaped":"line\nquote\"slash\\","euro":"\u20ac","face":"\uD83D\uDE03"})");
    REQUIRE(value);
    auto const& object = std::get<JsonValue::Object>(value->data);

    CHECK(std::get<std::string>(object.at("escaped").data) == "line\nquote\"slash\\");
    CHECK(std::get<std::string>(object.at("euro").data) == "\xe2\x82\xac");
    CHECK(std::get<std::string>(object.at("face").data) == "\xf0\x9f\x98\x83");
}

TEST_CASE("JSON serialization orders object members and preserves floating syntax", "[rpc][json]")
{
    auto const object = make_json(JsonValue::Object{
        {"zeta",   make_json(std::uint64_t{1})    },
        {"alpha",  make_json(1.0)                 },
        {"middle", make_json(std::string{"value"})},
    });

    auto encoded = serialize_json(object);
    REQUIRE(encoded);
    CHECK(*encoded == R"({"alpha":1.0,"middle":"value","zeta":1})");

    auto decoded = parse_json(*encoded);
    REQUIRE(decoded);
    auto const& decoded_object = std::get<JsonValue::Object>(decoded->data);
    CHECK(std::holds_alternative<double>(decoded_object.at("alpha").data));
}

TEST_CASE("JSON positive signed integers serialize but parse as unsigned", "[rpc][json]")
{
    auto encoded = serialize_json(make_json(std::int64_t{7}));
    REQUIRE(encoded);
    CHECK(*encoded == "7");

    auto decoded = parse_json(*encoded);
    REQUIRE(decoded);
    REQUIRE(std::holds_alternative<std::uint64_t>(decoded->data));
    CHECK(std::get<std::uint64_t>(decoded->data) == 7);
}

TEST_CASE("JSON parsing rejects invalid syntax and trailing roots", "[rpc][json]")
{
    auto const invalid = std::array<std::string_view, 7>{
        "",
        " ",
        "{",
        "[1,]",
        R"({"value":})",
        "null true",
        "null trailing",
    };

    for (auto input : invalid) {
        CAPTURE(input);
        auto result = parse_json(input);
        REQUIRE_FALSE(result);
        CHECK(result.error().category == ErrorCategory::InvalidArgument);
        CHECK(result.error().code == "rpc.json.syntax");
    }
}

TEST_CASE("JSON parsing rejects invalid UTF-8 and malformed Unicode escapes", "[rpc][json]")
{
    auto const invalid_utf8 = std::array{
        std::string{"\"\xc3\x28\""},
        std::string{"\"\xc0\xaf\""},
        std::string{"\"\xed\xa0\x80\""},
        std::string{"\"\xf4\x90\x80\x80\""},
    };

    for (auto const& input : invalid_utf8) {
        auto result = parse_json(input);
        REQUIRE_FALSE(result);
        CHECK(result.error().code == "rpc.json.invalid_utf8");
    }

    auto const malformed_escapes = std::array<std::string_view, 3>{
        R"("\uD800")",
        R"("\uDC00")",
        R"("\uD800\u0041")",
    };
    for (auto input : malformed_escapes) {
        CAPTURE(input);
        auto result = parse_json(input);
        REQUIRE_FALSE(result);
        CHECK(result.error().code == "rpc.json.syntax");
    }
}

TEST_CASE("JSON parsing rejects duplicate members at every nesting level", "[rpc][json]")
{
    auto const duplicates = std::array<std::string_view, 2>{
        R"({"key":1,"key":2})",
        R"({"outer":{"key":1,"key":2}})",
    };

    for (auto input : duplicates) {
        CAPTURE(input);
        auto result = parse_json(input);
        REQUIRE_FALSE(result);
        CHECK(result.error().category == ErrorCategory::InvalidArgument);
        CHECK(result.error().code == "rpc.json.duplicate_member");
    }
}

TEST_CASE("JSON depth limits count only array and object containers", "[rpc][json]")
{
    auto scalar_at_zero = parse_json("0", {.max_depth = 0});
    REQUIRE(scalar_at_zero);

    auto container_at_zero = parse_json("[]", {.max_depth = 0});
    REQUIRE_FALSE(container_at_zero);
    CHECK(container_at_zero.error().category == ErrorCategory::ResourceExhausted);
    CHECK(container_at_zero.error().code == "rpc.json.depth_limit");

    auto boundary = parse_json(R"([{"value":1}])", {.max_depth = 2});
    REQUIRE(boundary);

    auto beyond = parse_json(R"([{"value":[]}])", {.max_depth = 2});
    REQUIRE_FALSE(beyond);
    CHECK(beyond.error().code == "rpc.json.depth_limit");
}

TEST_CASE("JSON parsing preserves integer boundaries and rejects overflow", "[rpc][json]")
{
    auto minimum = parse_json("-9223372036854775808");
    REQUIRE(minimum);
    REQUIRE(std::holds_alternative<std::int64_t>(minimum->data));
    CHECK(std::get<std::int64_t>(minimum->data) == std::numeric_limits<std::int64_t>::min());

    auto negative_zero = parse_json("-0");
    REQUIRE(negative_zero);
    REQUIRE(std::holds_alternative<std::int64_t>(negative_zero->data));
    CHECK(std::get<std::int64_t>(negative_zero->data) == 0);

    auto signed_boundary = parse_json("9223372036854775807");
    REQUIRE(signed_boundary);
    CHECK(std::holds_alternative<std::uint64_t>(signed_boundary->data));

    auto above_signed = parse_json("9223372036854775808");
    REQUIRE(above_signed);
    CHECK(std::holds_alternative<std::uint64_t>(above_signed->data));

    auto maximum = parse_json("18446744073709551615");
    REQUIRE(maximum);
    REQUIRE(std::holds_alternative<std::uint64_t>(maximum->data));
    CHECK(std::get<std::uint64_t>(maximum->data) == std::numeric_limits<std::uint64_t>::max());

    auto const overflow = std::array<std::string_view, 2>{
        "-9223372036854775809",
        "18446744073709551616",
    };
    for (auto input : overflow) {
        CAPTURE(input);
        auto result = parse_json(input);
        REQUIRE_FALSE(result);
        CHECK(result.error().category == ErrorCategory::InvalidArgument);
        CHECK(result.error().code == "rpc.json.integer_overflow");
    }
}

TEST_CASE("JSON parsing rejects non-finite results and non-JSON number spellings", "[rpc][json]")
{
    auto finite = parse_json("1e308");
    REQUIRE(finite);
    REQUIRE(std::holds_alternative<double>(finite->data));
    CHECK(std::isfinite(std::get<double>(finite->data)));

    auto overflow = parse_json("1e9999");
    REQUIRE_FALSE(overflow);
    CHECK(overflow.error().code == "rpc.json.non_finite");

    auto const invalid_spellings = std::array<std::string_view, 3>{"NaN", "Infinity", "-Infinity"};
    for (auto input : invalid_spellings) {
        CAPTURE(input);
        auto result = parse_json(input);
        REQUIRE_FALSE(result);
        CHECK(result.error().code == "rpc.json.syntax");
    }
}

TEST_CASE("JSON serialization rejects non-finite floating-point values", "[rpc][json]")
{
    auto const values = std::array{
        std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::quiet_NaN(),
    };

    for (auto value : values) {
        auto result = serialize_json(make_json(value));
        REQUIRE_FALSE(result);
        CHECK(result.error().category == ErrorCategory::InvalidArgument);
        CHECK(result.error().code == "rpc.json.non_finite");
        CHECK(result.error().detail.empty());
    }
}

TEST_CASE("JSON serialization rejects invalid UTF-8 in values and keys", "[rpc][json]")
{
    auto invalid_string = serialize_json(make_json(std::string{"\xc3\x28"}));
    REQUIRE_FALSE(invalid_string);
    CHECK(invalid_string.error().code == "rpc.json.invalid_utf8");

    auto invalid_key = serialize_json(make_json(JsonValue::Object{
        {std::string{"\xc3\x28"}, make_json(JsonNull{})},
    }));
    REQUIRE_FALSE(invalid_key);
    CHECK(invalid_key.error().code == "rpc.json.invalid_utf8");
}

TEST_CASE("JSON errors never expose input bodies", "[rpc][json]")
{
    constexpr auto marker = std::string_view{"super-secret-marker"};
    auto           result = parse_json(R"({"super-secret-marker":})");
    REQUIRE_FALSE(result);

    CHECK(result.error().code == "rpc.json.syntax");
    CHECK_FALSE(contains(result.error().message, marker));
    CHECK_FALSE(contains(result.error().detail, marker));
}
