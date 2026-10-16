#include "job_payload_admission_priv.hpp"

#include "http_job_method_priv.hpp"
#include "http_job_payload_priv.hpp"
#include "job_validation_priv.hpp"
#include "json.hpp"
#include "support/catch_utils.hpp" // IWYU pragma: keep for Catch::StringMaker specializations
#include "text_validation.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

using namespace jb::core;
using namespace jb::jobu;
using namespace jb::jobu::detail;

namespace {

auto json(std::string_view text) -> JsonValue
{
    auto parsed = parse_json(text);
    REQUIRE(parsed);
    return std::move(parsed).value();
}

auto& object(JsonValue& value)
{
    return std::get<JsonValue::Object>(value.data);
}

auto payload_with_method(std::string method) -> JsonValue
{
    auto payload = json(R"({"url":"https://example.test/","future":{"keep":true}})");
    object(payload).emplace("method", JsonValue{.data = std::move(method)});
    return payload;
}

void check_payload_error(Error const& error, std::string_view reason)
{
    CHECK(error.category == ErrorCategory::InvalidArgument);
    CHECK(error.code == "jobu.job.invalid_payload");
    CHECK(error.detail == "reason=" + std::string{reason});
}

auto payload_at_limit() -> JsonValue
{
    auto payload = payload_with_method("get");
    object(payload).emplace("padding", JsonValue{.data = std::string{}});
    auto initial = serialize_json(payload);
    REQUIRE(initial);
    REQUIRE(initial->size() < maximum_job_document_bytes);
    object(payload).at("padding").data = std::string(maximum_job_document_bytes - initial->size(), 'x');
    return payload;
}

} // anonymous namespace

TEST_CASE("HTTP admission canonicalizes every ASCII casing of all six methods", "[jobu][admission][http]")
{
    struct Case {
        HttpJobMethod    method;
        std::string_view text;
    };

    auto const cases = std::array{
        Case{.method = HttpJobMethod::Get,    .text = "GET"   },
        Case{.method = HttpJobMethod::Post,   .text = "POST"  },
        Case{.method = HttpJobMethod::Put,    .text = "PUT"   },
        Case{.method = HttpJobMethod::Patch,  .text = "PATCH" },
        Case{.method = HttpJobMethod::Delete, .text = "DELETE"},
        Case{.method = HttpJobMethod::Head,   .text = "HEAD"  },
    };

    for (auto const& test : cases) {
        auto const expected     = payload_with_method(std::string{test.text});
        // Enumerate every combination, including uppercase, lowercase, and all mixed spellings.
        auto const combinations = std::size_t{1} << test.text.size();
        for (std::size_t mask = 0; mask < combinations; ++mask) {
            auto spelling = std::string{test.text};
            for (std::size_t index = 0; index < spelling.size(); ++index) {
                if ((mask & (std::size_t{1} << index)) != 0U) {
                    spelling[index] = static_cast<char>(spelling[index] + ('a' - 'A'));
                }
            }
            CAPTURE(spelling);
            auto parsed = parse_submitted_http_method(spelling);
            REQUIRE(parsed);
            CHECK(*parsed == test.method);
            CHECK(http_method_text(*parsed) == test.text);

            auto admitted = normalize_submitted_job_payload(JobType::Http, payload_with_method(spelling));
            REQUIRE(admitted);
            CHECK(*admitted == expected);
            auto stored        = validate_and_serialize_job_payload(JobType::Http, *admitted);
            auto expected_text = serialize_json(expected);
            REQUIRE(stored);
            REQUIRE(expected_text);
            CHECK(stored->serialized() == *expected_text);

            auto decoded = decode_http_job_payload(*admitted);
            REQUIRE(decoded);
            REQUIRE(std::holds_alternative<HttpJobMethod>(decoded->method));
            CHECK(std::get<HttpJobMethod>(decoded->method) == test.method);
        }
    }
}

TEST_CASE("HTTP admission inserts GET in its owned copy and preserves other members", "[jobu][admission][http]")
{
    auto const original = json(R"({"url":"https://example.test/","future":{"secret":"ignored"}})");
    auto       admitted = normalize_submitted_job_payload(JobType::Http, original);
    REQUIRE(admitted);
    CHECK_FALSE(original.as_object().contains("method"));
    auto expected = original;
    object(expected).emplace("method", JsonValue{.data = std::string{"GET"}});
    CHECK(*admitted == expected);
    auto repeated = normalize_submitted_job_payload(JobType::Http, *admitted);
    REQUIRE(repeated);
    CHECK(*repeated == *admitted);
}

TEST_CASE("HTTP admission rejects unsupported empty whitespace and alias methods", "[jobu][admission][http]")
{
    for (auto const* value : {"", " ", " GET", "GET ", "\tGET", "GET\n", "OPTIONS", "GTE", "CUSTOM", "GET/"}) {
        CAPTURE(value);
        auto parsed = parse_submitted_http_method(value);
        REQUIRE_FALSE(parsed);
        check_payload_error(parsed.error(), "invalid_method");
        CHECK(parsed.error().message.find(supported_http_job_methods) != std::string::npos);

        auto admitted = normalize_submitted_job_payload(JobType::Http, payload_with_method(value));
        REQUIRE_FALSE(admitted);
        CHECK(admitted.error() == parsed.error());
    }
}

TEST_CASE("HTTP admission diagnoses every non-string method without printing its value", "[jobu][admission][http]")
{
    for (auto const* text : {"null", "true", "7", "1.5", "[]", R"({"secret":"hidden-value"})"}) {
        auto payload                 = payload_with_method("GET");
        object(payload).at("method") = json(text);
        auto admitted                = normalize_submitted_job_payload(JobType::Http, std::move(payload));
        REQUIRE_FALSE(admitted);
        check_payload_error(admitted.error(), "invalid_method");
        CHECK(admitted.error().message ==
              "HTTP method must be a string. Supported methods: GET, POST, PUT, PATCH, DELETE, HEAD.");
        CHECK(admitted.error().detail.find("hidden-value") == std::string::npos);
    }
}

TEST_CASE("HTTP method errors escape bytes and bound output without unrelated payload content", "[jobu][admission]")
{
    auto method = std::string{"bad\n\"\\"};
    method.push_back('\0');
    method.push_back('\x7f');
    method.push_back(static_cast<char>(0xff));
    method.append(200U, 'x');
    method       += "hidden-suffix";
    auto payload  = payload_with_method(method);
    object(payload).insert_or_assign("url", JsonValue{.data = std::string{"https://url-secret.test/"}});
    object(payload).emplace("body", json(R"({"encoding":"utf8","data":"body-secret"})"));
    object(payload).emplace("headers", json(R"([{"name":"Authorization","value":"header-secret"}])"));

    auto admitted = normalize_submitted_job_payload(JobType::Http, std::move(payload));
    REQUIRE_FALSE(admitted);
    auto const& error = admitted.error();
    check_payload_error(error, "invalid_method");
    CHECK(error.message.starts_with("Unsupported HTTP method \"bad\\x0a\\\"\\\\\\x00\\x7f\\xff"));
    CHECK(error.message.find("...\". Supported methods:") != std::string::npos);
    CHECK(error.message.size() < 400U);
    CHECK(is_valid_utf8(error.message));
    CHECK_FALSE(has_ascii_control(error.message));
    for (auto const* secret : {"hidden-suffix", "url-secret", "body-secret", "header-secret"}) {
        CHECK(error.message.find(secret) == std::string::npos);
        CHECK(error.detail.find(secret) == std::string::npos);
    }

    // Worst-case byte expansion must be bounded too, including embedded controls at the first position.
    auto escaped = parse_submitted_http_method(std::string(1000U, '\x01'));
    REQUIRE_FALSE(escaped);
    CHECK(escaped.error().message.size() < 400U);
    CHECK_FALSE(has_ascii_control(escaped.error().message));
}

TEST_CASE("HTTP admission applies HEAD body checks after normalization", "[jobu][admission][http]")
{
    for (auto const* body : {R"({"encoding":"utf8","data":""})", R"({"secret":"body.bytes"})"}) {
        auto payload = payload_with_method("head");
        object(payload).emplace("body", json(body));
        // Shared legacy validation still regards lowercase head as a custom token.
        REQUIRE(validate_and_serialize_job_payload(JobType::Http, payload));
        auto admitted = normalize_submitted_job_payload(JobType::Http, std::move(payload));
        REQUIRE_FALSE(admitted);
        check_payload_error(admitted.error(), "invalid_body");
    }
}

TEST_CASE("Payload admission preserves template values and existing structural errors", "[jobu][admission]")
{
    auto payload                       = json(R"({"url":"https://example.test/","method":"post",
        "headers":[{"name":"Authorization","value":{"secret":"auth.token"}}],
        "body":{"secret":"body.bytes"}})");
    auto expected                      = payload;
    object(expected).at("method").data = std::string{"POST"};
    auto admitted                      = normalize_submitted_job_payload(JobType::Http, payload);
    REQUIRE(admitted);
    CHECK(*admitted == expected);

    struct Case {
        std::string_view text;
        std::string_view reason;
    };

    auto const cases = std::array{
        Case{.text = "[]",                                                                 .reason = "not_object"     },
        Case{.text = "{}",                                                                 .reason = "missing_url"    },
        Case{.text = R"({"url":"ftp://example.test/"})",                                   .reason = "invalid_url"    },
        Case{.text = R"({"url":"https://example.test/","headers":true})",                  .reason = "invalid_headers"},
        Case{.text   = R"({"url":"https://example.test/","expected_statuses":["bogus"]})",
             .reason = "invalid_expected_statuses"                                                                    },
        Case{.text   = R"({"url":"https://example.test/","body":{"secret":"bad name"}})",
             .reason = "invalid_secret_name"                                                                          },
    };
    for (auto const& test : cases) {
        CAPTURE(test.text);
        auto invalid = normalize_submitted_job_payload(JobType::Http, json(test.text));
        REQUIRE_FALSE(invalid);
        check_payload_error(invalid.error(), test.reason);
    }
}

TEST_CASE("Payload admission retains CLI payloads and validation", "[jobu][admission][cli]")
{
    auto const payload  = json(R"({"command":"/bin/true","arguments":[{"secret":"arg.value"}],
        "method":"OPTIONS","future":{"keep":true}})");
    auto       admitted = normalize_submitted_job_payload(JobType::Cli, payload);
    REQUIRE(admitted);
    CHECK(*admitted == payload);

    auto invalid = normalize_submitted_job_payload(JobType::Cli, json(R"({"command":"/bin/true","arguments":true})"));
    REQUIRE_FALSE(invalid);
    check_payload_error(invalid.error(), "invalid_arguments");
    auto unknown = normalize_submitted_job_payload(static_cast<JobType>(255), payload);
    REQUIRE_FALSE(unknown);
    check_payload_error(unknown.error(), "unknown_type");
}

TEST_CASE("Payload admission enforces the normalized document size and JSON limits", "[jobu][admission][limits]")
{
    auto payload  = payload_at_limit();
    auto admitted = normalize_submitted_job_payload(JobType::Http, payload);
    REQUIRE(admitted);
    auto serialized = serialize_json(*admitted);
    REQUIRE(serialized);
    CHECK(serialized->size() == maximum_job_document_bytes);

    auto oversized = payload;
    std::get<std::string>(object(oversized).at("padding").data).push_back('x');
    auto rejected = normalize_submitted_job_payload(JobType::Http, std::move(oversized));
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().category == ErrorCategory::ResourceExhausted);
    CHECK(rejected.error().code == "jobu.protocol.value_too_large");
    CHECK(rejected.error().detail == "reason=too_large");

    // A formerly valid document may exceed the limit when the default method becomes explicit.
    object(payload).erase("method");
    std::get<std::string>(object(payload).at("padding").data)
        .append(std::string_view{R"(,"method":"get")"}.size(), 'x');
    REQUIRE(validate_and_serialize_job_payload(JobType::Http, payload));
    auto default_exceeds_limit = normalize_submitted_job_payload(JobType::Http, std::move(payload));
    REQUIRE_FALSE(default_exceeds_limit);
    CHECK(default_exceeds_limit.error() == rejected.error());

    auto invalid_json = payload_with_method("GET");
    object(invalid_json).emplace("future_number", JsonValue{.data = std::numeric_limits<double>::infinity()});
    auto invalid = normalize_submitted_job_payload(JobType::Http, std::move(invalid_json));
    REQUIRE_FALSE(invalid);
    check_payload_error(invalid.error(), "invalid_json");
}
