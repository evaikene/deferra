#include "http_job_payload_priv.hpp"

#include "job_validation_priv.hpp"
#include "json.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace jb::core;
using namespace jb::jobu;
using namespace jb::jobu::detail;

namespace {

// Four worst-case executor metadata headers occupy 183 generic name/value bytes.
constexpr std::size_t kMaximumPayloadHeaderCount{128U - 4U};
constexpr std::size_t kMaximumPayloadHeaderBytes{(std::size_t{64} * 1024U) - 183U};

auto json_string(std::string value) -> JsonValue
{
    return JsonValue{.data = std::move(value)};
}

auto json_bool(bool value) -> JsonValue
{
    return JsonValue{.data = value};
}

auto json_array(JsonValue::Array value) -> JsonValue
{
    return JsonValue{.data = std::move(value)};
}

auto json_object(JsonValue::Object value) -> JsonValue
{
    return JsonValue{.data = std::move(value)};
}

auto http_payload(std::string url = "https://example.test/path") -> JsonValue
{
    return json_object({
        {"url", json_string(std::move(url))}
    });
}

auto& object(JsonValue& value)
{
    return std::get<JsonValue::Object>(value.data);
}

void set_member(JsonValue& value, std::string name, JsonValue member_value)
{
    object(value).insert_or_assign(std::move(name), std::move(member_value));
}

auto body(std::string encoding, std::string data) -> JsonValue
{
    return json_object({
        {"data",     json_string(std::move(data))    },
        {"encoding", json_string(std::move(encoding))},
    });
}

auto header(std::string name, std::string value, std::optional<bool> sensitive = std::nullopt) -> JsonValue
{
    auto entry = JsonValue::Object{
        {"name",  json_string(std::move(name)) },
        {"value", json_string(std::move(value))},
    };
    if (sensitive) {
        entry.emplace("sensitive", json_bool(*sensitive));
    }
    return json_object(std::move(entry));
}

auto selectors(std::initializer_list<std::string_view> values) -> JsonValue
{
    auto result = JsonValue::Array{};
    result.reserve(values.size());
    for (auto const value : values) {
        result.push_back(json_string(std::string{value}));
    }
    return json_array(std::move(result));
}

auto bytes(std::initializer_list<unsigned int> values) -> ByteBuffer
{
    auto result = ByteBuffer{};
    result.reserve(values.size());
    for (auto const value : values) {
        result.push_back(static_cast<std::byte>(value));
    }
    return result;
}

void check_issue(JsonValue const& payload, JobPayloadIssue expected)
{
    auto decoded = decode_http_job_payload(payload);
    REQUIRE_FALSE(decoded);
    CHECK(decoded.error() == expected);
    CHECK(job_payload_structure_issue(JobType::Http, payload) == expected);
}

} // anonymous namespace

TEST_CASE("HTTP payload defaults produce one owning request policy", "[jobu][http][payload]")
{
    auto payload = http_payload();
    set_member(payload,
               "future",
               json_object({
                   {"nested", json_bool(true)}
    }));

    auto decoded = decode_http_job_payload(payload);
    REQUIRE(decoded);
    CHECK(decoded->url == "https://example.test/path");
    CHECK(decoded->method == "GET");
    CHECK(decoded->headers.empty());
    CHECK_FALSE(decoded->body);
    CHECK(decoded->expected_statuses.ranges() == std::vector<HttpStatusRange>{
                                                     {.first = 200U, .last = 299U}
    });
    CHECK_FALSE(decoded->expected_statuses.contains(199U));
    CHECK(decoded->expected_statuses.contains(200U));
    CHECK(decoded->expected_statuses.contains(299U));
    CHECK_FALSE(decoded->expected_statuses.contains(300U));

    auto serialized = serialize_json(payload);
    auto validated  = validate_and_serialize_job_payload(JobType::Http, payload);
    REQUIRE(serialized);
    REQUIRE(validated);
    CHECK(validated->serialized() == *serialized);
    CHECK(validated->serialized().find("future") != std::string_view::npos);
}

TEST_CASE("HTTP payload decodes headers body and merged statuses", "[jobu][http][payload]")
{
    auto payload = http_payload("https://example.test/hooks");
    set_member(payload, "method", json_string("POST"));
    set_member(payload,
               "headers",
               json_array({
                   header("Content-Type", "application/octet-stream"),
                   header("authorization", "literal", false),
                   header("Cookie", "session"),
                   header("Proxy-Authorization", "proxy", false),
                   header("X-Private", "marked", true),
                   header("X-Public", "plain"),
               }));
    set_member(payload, "body", body("utf8", std::string{"h\xC3\xA9\0", 4U}));
    set_member(payload, "expected_statuses", selectors({"304", "200-204", "202-299", "300", "304"}));

    auto decoded = decode_http_job_payload(payload);
    REQUIRE(decoded);
    REQUIRE(decoded->headers.size() == 6U);
    CHECK_FALSE(decoded->headers[0].sensitive);
    CHECK(decoded->headers[1].sensitive);
    CHECK(decoded->headers[2].sensitive);
    CHECK(decoded->headers[3].sensitive);
    CHECK(decoded->headers[4].sensitive);
    CHECK_FALSE(decoded->headers[5].sensitive);
    REQUIRE(decoded->body);
    CHECK(*decoded->body == bytes({0x68U, 0xc3U, 0xa9U, 0x00U}));
    CHECK(decoded->expected_statuses.ranges() == std::vector<HttpStatusRange>{
                                                     {.first = 200U, .last = 300U},
                                                     {.first = 304U, .last = 304U}
    });
    CHECK(decoded->expected_statuses.contains(250U));
    CHECK_FALSE(decoded->expected_statuses.contains(303U));
    CHECK(decoded->expected_statuses.contains(304U));
}

TEST_CASE("HTTP payload accepts canonical base64 including arbitrary bytes", "[jobu][http][payload][base64]")
{
    struct Case {
        std::string_view encoded;
        ByteBuffer       expected;
    };

    auto const cases = std::vector<Case>{
        {.encoded = "",         .expected = {}                                 },
        {.encoded = "TQ==",     .expected = bytes({0x4dU})                     },
        {.encoded = "TWE=",     .expected = bytes({0x4dU, 0x61U})              },
        {.encoded = "TWFu",     .expected = bytes({0x4dU, 0x61U, 0x6eU})       },
        {.encoded = "AAH/",     .expected = bytes({0x00U, 0x01U, 0xffU})       },
        {.encoded = "/+7dzA==", .expected = bytes({0xffU, 0xeeU, 0xddU, 0xccU})},
    };

    for (auto const& test : cases) {
        CAPTURE(test.encoded);
        auto payload = http_payload();
        set_member(payload, "method", json_string("POST"));
        set_member(payload, "body", body("base64", std::string{test.encoded}));
        auto decoded = decode_http_job_payload(payload);
        REQUIRE(decoded);
        REQUIRE(decoded->body);
        CHECK(*decoded->body == test.expected);
    }
}

TEST_CASE("HTTP payload rejects noncanonical base64", "[jobu][http][payload][base64]")
{
    for (auto const* const encoded :
         {"A", "TWE", "====", "A===", "TQ=A", "TQ==AAAA", "TQ=\n", "TQ--", "TR==", "TWF="}) {
        CAPTURE(encoded);
        auto payload = http_payload();
        set_member(payload, "method", json_string("POST"));
        set_member(payload, "body", body("base64", encoded));
        check_issue(payload, JobPayloadIssue::InvalidBody);
    }
}

TEST_CASE("HTTP payload enforces method URL and HEAD body rules", "[jobu][http][payload][validation]")
{
    auto maximum_url = std::string{"http://a/"};
    maximum_url.append((std::size_t{16} * 1024U) - maximum_url.size(), 'x');
    auto boundary_payload = http_payload(std::move(maximum_url));
    set_member(boundary_payload, "method", json_string("!#$%&'*+-.^_`|~0123456789ABCDEFG"));
    REQUIRE(decode_http_job_payload(boundary_payload));

    check_issue(json_object({}), JobPayloadIssue::MissingUrl);
    check_issue(json_object({
                    {"url", json_bool(true)}
    }),
                JobPayloadIssue::MissingUrl);
    check_issue(http_payload(""), JobPayloadIssue::MissingUrl);
    check_issue(http_payload("ftp://example.test/file"), JobPayloadIssue::InvalidUrl);
    check_issue(http_payload("https://example.test/has space"), JobPayloadIssue::InvalidUrl);

    for (auto method : {std::string{}, std::string{"HAS SPACE"}, std::string(33U, 'A')}) {
        auto payload = http_payload();
        set_member(payload, "method", json_string(std::move(method)));
        check_issue(payload, JobPayloadIssue::InvalidMethod);
    }
    auto wrong_method = http_payload();
    set_member(wrong_method, "method", json_bool(true));
    check_issue(wrong_method, JobPayloadIssue::InvalidMethod);

    auto head_body = http_payload();
    set_member(head_body, "method", json_string("HEAD"));
    set_member(head_body, "body", body("utf8", ""));
    check_issue(head_body, JobPayloadIssue::InvalidBody);
}

TEST_CASE("HTTP payload header entries are closed bounded and JobU owned", "[jobu][http][payload][headers]")
{
    auto non_array = http_payload();
    set_member(non_array, "headers", json_object({}));
    check_issue(non_array, JobPayloadIssue::InvalidHeaders);

    auto non_object = http_payload();
    set_member(non_object, "headers", json_array({json_string("X-Test: value")}));
    check_issue(non_object, JobPayloadIssue::InvalidHeaders);

    auto missing_value = http_payload();
    set_member(missing_value,
               "headers",
               json_array({
                   json_object({{"name", json_string("X-Test")}}
                   )
    }));
    check_issue(missing_value, JobPayloadIssue::InvalidHeaders);

    auto unknown_member = http_payload();
    set_member(unknown_member,
               "headers",
               json_array({
                   json_object({
                                {"future", json_bool(true)},
                                {"name", json_string("X-Test")},
                                {"value", json_string("value")},
                                }
                   )
    }));
    check_issue(unknown_member, JobPayloadIssue::InvalidHeaders);

    auto wrong_sensitive = http_payload();
    set_member(wrong_sensitive,
               "headers",
               json_array({
                   json_object({
                                {"name", json_string("X-Test")},
                                {"sensitive", json_string("true")},
                                {"value", json_string("value")},
                                }
                   )
    }));
    check_issue(wrong_sensitive, JobPayloadIssue::InvalidHeaders);

    for (auto const* const name : {"Host",
                                   "content-length",
                                   "Transfer-Encoding",
                                   "Connection",
                                   "Proxy-Connection",
                                   "TE",
                                   "Trailer",
                                   "Upgrade",
                                   "x-jobu-run-id",
                                   "X-JOBU-",
                                   "IDEMPOTENCY-KEY"}) {
        CAPTURE(name);
        auto payload = http_payload();
        set_member(payload, "headers", json_array({header(name, "value")}));
        check_issue(payload, JobPayloadIssue::InvalidHeaders);
    }

    auto duplicate = http_payload();
    set_member(duplicate, "headers", json_array({header("X-Test", "one"), header("x-test", "two")}));
    check_issue(duplicate, JobPayloadIssue::InvalidHeaders);

    auto invalid_value = http_payload();
    set_member(invalid_value, "headers", json_array({header("X-Test", "line\nbreak")}));
    check_issue(invalid_value, JobPayloadIssue::InvalidHeaders);

    auto too_many = JsonValue::Array{};
    for (std::size_t index = 0; index < kMaximumPayloadHeaderCount + 1U; ++index) {
        too_many.push_back(header("X-" + std::to_string(index), ""));
    }
    auto maximum_count  = http_payload();
    auto maximum_values = JsonValue::Array{too_many.begin(), too_many.end() - 1};
    set_member(maximum_count, "headers", json_array(std::move(maximum_values)));
    REQUIRE(decode_http_job_payload(maximum_count));

    auto excessive_count = http_payload();
    set_member(excessive_count, "headers", json_array(std::move(too_many)));
    check_issue(excessive_count, JobPayloadIssue::InvalidHeaders);

    auto exact_bytes = http_payload();
    set_member(exact_bytes, "headers", json_array({header("X", std::string(kMaximumPayloadHeaderBytes - 1U, 'a'))}));
    REQUIRE(decode_http_job_payload(exact_bytes));

    auto excessive_bytes = http_payload();
    set_member(excessive_bytes, "headers", json_array({header("X", std::string(kMaximumPayloadHeaderBytes, 'a'))}));
    check_issue(excessive_bytes, JobPayloadIssue::InvalidHeaders);
}

TEST_CASE("HTTP payload body object is closed typed and valid UTF-8", "[jobu][http][payload][body]")
{
    auto non_object = http_payload();
    set_member(non_object, "body", json_string("data"));
    check_issue(non_object, JobPayloadIssue::InvalidBody);

    for (auto invalid : {
             json_object({{"encoding", json_string("utf8")}}
             ),
             json_object({{"data", json_string("value")}}
             ),
             json_object({{"data", json_string("value")}, {"encoding", json_bool(true)}}
             ),
             json_object({{"data", json_bool(true)}, {"encoding", json_string("utf8")}}
             ),
             json_object({
                          {"data", json_string("value")},
                          {"encoding", json_string("utf8")},
                          {"future", json_bool(true)},
                          }
             ),
             body("hex", "00"),
             body("utf8", std::string{"bad\xC3", 4U}
             ),
    }) {
        auto payload = http_payload();
        set_member(payload, "body", std::move(invalid));
        check_issue(payload, JobPayloadIssue::InvalidBody);
    }
}

TEST_CASE("HTTP payload validates and normalizes expected status selectors", "[jobu][http][payload][statuses]")
{
    auto payload = http_payload();
    set_member(payload, "expected_statuses", selectors({"599", "100", "200-204", "202-299", "300"}));
    auto decoded = decode_http_job_payload(payload);
    REQUIRE(decoded);
    CHECK(decoded->expected_statuses.ranges() == std::vector<HttpStatusRange>{
                                                     {.first = 100U, .last = 100U},
                                                     {.first = 200U, .last = 300U},
                                                     {.first = 599U, .last = 599U}
    });

    auto non_array = http_payload();
    set_member(non_array, "expected_statuses", json_string("200"));
    check_issue(non_array, JobPayloadIssue::InvalidExpectedStatuses);

    auto empty = http_payload();
    set_member(empty, "expected_statuses", json_array({}));
    check_issue(empty, JobPayloadIssue::InvalidExpectedStatuses);

    auto non_string = http_payload();
    set_member(non_string, "expected_statuses", json_array({json_bool(true)}));
    check_issue(non_string, JobPayloadIssue::InvalidExpectedStatuses);

    for (auto const* const selector : {"99", "099", "600", "200-", "200-199", "200 - 299", "200-600", "abc"}) {
        CAPTURE(selector);
        auto invalid = http_payload();
        set_member(invalid, "expected_statuses", selectors({selector}));
        check_issue(invalid, JobPayloadIssue::InvalidExpectedStatuses);
    }

    auto too_many_values = JsonValue::Array{};
    for (std::size_t index = 0; index < 65U; ++index) {
        too_many_values.push_back(json_string("200"));
    }
    auto maximum_count  = http_payload();
    auto maximum_values = JsonValue::Array{too_many_values.begin(), too_many_values.end() - 1};
    set_member(maximum_count, "expected_statuses", json_array(std::move(maximum_values)));
    auto maximum_decoded = decode_http_job_payload(maximum_count);
    REQUIRE(maximum_decoded);
    CHECK(maximum_decoded->expected_statuses.ranges() == std::vector<HttpStatusRange>{
                                                             {.first = 200U, .last = 200U}
    });

    auto too_many = http_payload();
    set_member(too_many, "expected_statuses", json_array(std::move(too_many_values)));
    check_issue(too_many, JobPayloadIssue::InvalidExpectedStatuses);
}

TEST_CASE("Job payload validation keeps CLI behavior and safe HTTP reason tokens", "[jobu][http][payload][safety]")
{
    auto valid_cli = json_object({
        {"arguments", json_array({json_string("one"), json_string("two")})},
        {"command",   json_string("/bin/true")                            },
        {"future",    json_bool(true)                                     },
    });
    CHECK(job_payload_structure_issue(JobType::Cli, valid_cli) == JobPayloadIssue::None);
    CHECK(validate_and_serialize_job_payload(JobType::Cli, valid_cli));

    auto invalid_cli = json_object({
        {"arguments", json_string("not-an-array")},
        {"command",   json_string("/bin/true")   },
    });
    CHECK(job_payload_structure_issue(JobType::Cli, invalid_cli) == JobPayloadIssue::InvalidArguments);

    auto invalid_http = http_payload("https://url-secret.example/has space?token=secret");
    set_member(invalid_http, "headers", json_array({header("X-Secret", "header-secret")}));
    auto validated = validate_and_serialize_job_payload(JobType::Http, invalid_http);
    REQUIRE_FALSE(validated);
    CHECK(validated.error() == JobPayloadIssue::InvalidUrl);
    auto const reason = job_payload_issue_text(validated.error());
    CHECK(reason == "invalid_url");
    CHECK(reason.find("secret") == std::string_view::npos);

    auto exact_limit = http_payload();
    set_member(exact_limit, "future", json_string(""));
    auto initial = serialize_json(exact_limit);
    REQUIRE(initial);
    REQUIRE(initial->size() < maximum_job_document_bytes);
    set_member(exact_limit, "future", json_string(std::string(maximum_job_document_bytes - initial->size(), 'x')));
    auto exact_serialized = serialize_json(exact_limit);
    REQUIRE(exact_serialized);
    REQUIRE(exact_serialized->size() == maximum_job_document_bytes);
    REQUIRE(validate_and_serialize_job_payload(JobType::Http, exact_limit));

    auto  oversized = exact_limit;
    auto& filler    = std::get<std::string>(object(oversized).at("future").data);
    filler.push_back('x');
    auto too_large = validate_and_serialize_job_payload(JobType::Http, oversized);
    REQUIRE_FALSE(too_large);
    CHECK(too_large.error() == JobPayloadIssue::TooLarge);
    CHECK(job_payload_issue_text(too_large.error()) == "too_large");
}
