#include "http_retry_priv.hpp"

#include "attribute_registry.hpp"
#include "byte_buffer.hpp"
#include "http_job_payload_priv.hpp"
#include "json.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace jb::core;
using namespace jb::jobu;
using namespace jb::jobu::detail;
using namespace jb::net;
using namespace std::chrono_literals;

namespace {

auto json_string(std::string value) -> JsonValue
{
    return JsonValue{.data = std::move(value)};
}

auto string_list(std::initializer_list<std::string_view> values) -> AttributeValue::List
{
    auto result = AttributeValue::List{};
    result.reserve(values.size());
    for (auto const value : values) {
        result.push_back({.data = std::string{value}});
    }
    return result;
}

auto decoded_payload(std::initializer_list<std::string_view> expected = {"200-299"}) -> HttpJobPayload
{
    auto selectors = JsonValue::Array{};
    selectors.reserve(expected.size());
    for (auto const selector : expected) {
        selectors.push_back(json_string(std::string{selector}));
    }
    auto decoded = decode_http_job_payload(JsonValue{
        .data = JsonValue::Object{
                                  {"expected_statuses", JsonValue{.data = std::move(selectors)}},
                                  {"url", json_string("https://example.test")},
                                  }
    });
    REQUIRE(decoded);
    return std::move(decoded).value();
}

auto materialized_attributes(AttributeSet const& overrides = {}) -> AttributeSet
{
    StandardAttributeRegistry registry;
    auto                      attributes = materialize_attributes(registry, {}, {}, overrides);
    REQUIRE(attributes);
    return std::move(attributes).value();
}

auto response(std::uint16_t status, std::vector<HttpHeader> headers = {}) -> HttpCompletionResult
{
    return HttpCompletionResult::success({
        .status_code = status,
        .headers     = std::move(headers),
    });
}

auto http_error(HttpErrorKind kind, std::string code = "net.http.test") -> HttpCompletionResult
{
    return HttpCompletionResult::failure({
        .kind = kind,
        .error =
            {
                    .category = ErrorCategory::Internal,
                    .code     = std::move(code),
                    .message  = "Safe test error",
                    },
    });
}

auto utc(int year, unsigned month, unsigned day, unsigned hour, unsigned minute, unsigned second) -> UtcTimePoint
{
    using namespace std::chrono;
    auto const value = sys_days{std::chrono::year{year} / std::chrono::month{month} / std::chrono::day{day}} +
                       hours{hour} + minutes{minute} + seconds{second};
    return time_point_cast<UtcTimePoint::duration>(value);
}

auto retry_after(JsonValue const& result) -> JsonValue::Object const*
{
    auto const& object = result.as_object();
    auto const  found  = object.find("retry_after");
    return found == object.end() ? nullptr : &found->second.as_object();
}

void check_retry_after(JsonValue const& result, bool accepted, bool clamped)
{
    auto const* value = retry_after(result);
    REQUIRE(value != nullptr);
    CHECK(value->at("accepted").as_bool() == accepted);
    CHECK(value->at("clamped").as_bool() == clamped);
}

auto bytes(std::string_view value) -> ByteBuffer
{
    auto const view = as_bytes(value);
    return ByteBuffer{view.begin(), view.end()};
}

void check_invalid_policy(auto const& result)
{
    REQUIRE_FALSE(result);
    CHECK(result.error().category == ErrorCategory::Internal);
}

} // anonymous namespace

TEST_CASE("HTTP status policy prioritizes expected statuses and honors retry overrides", "[jobu][http][retry]")
{
    auto const payload    = decoded_payload();
    auto const attributes = materialized_attributes();
    auto const completed  = UtcTimePoint{100s};

    auto expected = map_http_completion(response(204U), payload.expected_statuses, attributes, completed);
    REQUIRE(expected);
    CHECK(expected->outcome == AttemptOutcome::Succeeded);
    CHECK_FALSE(expected->failure_disposition);
    CHECK_FALSE(expected->retry_not_before);

    auto serialized = serialize_json(expected->result);
    REQUIRE(serialized);
    CHECK(
        *serialized ==
        R"({"body":{"captured_bytes":0,"total_bytes":0,"truncated":false},"duration_ms":0,"headers":{"captured_bytes":0,"total_bytes":0,"truncated":false},"outcome":"expected_status","redirects":0,"status":204,"type":"http"})");

    auto retryable = map_http_completion(response(503U), payload.expected_statuses, attributes, completed);
    REQUIRE(retryable);
    CHECK(retryable->outcome == AttemptOutcome::Failed);
    REQUIRE(retryable->failure_disposition);
    CHECK(*retryable->failure_disposition == FailureDisposition::Retryable);

    auto terminal = map_http_completion(response(404U), payload.expected_statuses, attributes, completed);
    REQUIRE(terminal);
    REQUIRE(terminal->failure_disposition);
    CHECK(*terminal->failure_disposition == FailureDisposition::Terminal);

    auto expected_503 = decoded_payload({"503"});
    auto precedence   = map_http_completion(response(503U,
                                                     {
                                                         {.name = "Retry-After", .value = "10"}
    }),
                                            expected_503.expected_statuses,
                                            attributes,
                                            completed);
    REQUIRE(precedence);
    CHECK(precedence->outcome == AttemptOutcome::Succeeded);
    CHECK_FALSE(precedence->failure_disposition);
    CHECK(retry_after(precedence->result) == nullptr);

    auto overrides      = materialized_attributes({
        {"http.retry_statuses", {.data = string_list({"404"})}}
    });
    auto overridden_404 = map_http_completion(response(404U), payload.expected_statuses, overrides, completed);
    auto overridden_503 = map_http_completion(response(503U), payload.expected_statuses, overrides, completed);
    REQUIRE(overridden_404);
    REQUIRE(overridden_503);
    CHECK(*overridden_404->failure_disposition == FailureDisposition::Retryable);
    CHECK(*overridden_503->failure_disposition == FailureDisposition::Terminal);
}

TEST_CASE("HTTP error policy maps every kind and applies only configurable overrides", "[jobu][http][retry]")
{
    struct Case {
        HttpErrorKind    kind;
        std::string_view category;
        bool             retryable;
        AttemptOutcome   outcome{AttemptOutcome::Failed};
    };

    constexpr std::array cases{
        Case{.kind = HttpErrorKind::InvalidRequest, .category = "invalid_request", .retryable = false},
        Case{.kind = HttpErrorKind::Resolve, .category = "resolve", .retryable = true},
        Case{.kind = HttpErrorKind::Connect, .category = "connect", .retryable = true},
        Case{.kind = HttpErrorKind::TlsVerification, .category = "tls_verification", .retryable = false},
        Case{.kind = HttpErrorKind::TlsHandshake, .category = "tls_handshake", .retryable = true},
        Case{.kind = HttpErrorKind::Timeout, .category = "timeout", .retryable = true},
        Case{.kind = HttpErrorKind::Send, .category = "send", .retryable = true},
        Case{.kind = HttpErrorKind::Receive, .category = "receive", .retryable = true},
        Case{.kind = HttpErrorKind::Redirect, .category = "redirect", .retryable = false},
        Case{.kind = HttpErrorKind::Protocol, .category = "protocol", .retryable = false},
        Case{.kind      = HttpErrorKind::Cancelled,
             .category  = "cancelled",
             .retryable = false,
             .outcome   = AttemptOutcome::Cancelled},
        Case{.kind = HttpErrorKind::Internal, .category = "internal", .retryable = false},
    };

    auto const payload    = decoded_payload();
    auto const attributes = materialized_attributes();
    for (auto const& test : cases) {
        CAPTURE(test.category);
        auto mapped = map_http_completion(http_error(test.kind), payload.expected_statuses, attributes, {});
        REQUIRE(mapped);
        CHECK(mapped->outcome == test.outcome);
        CHECK_FALSE(mapped->retry_not_before);
        CHECK(mapped->result.as_object().at("error_category").as_string() == test.category);
        if (test.outcome == AttemptOutcome::Cancelled) {
            CHECK_FALSE(mapped->failure_disposition);
        }
        else {
            REQUIRE(mapped->failure_disposition);
            CHECK(*mapped->failure_disposition ==
                  (test.retryable ? FailureDisposition::Retryable : FailureDisposition::Terminal));
        }
    }

    auto const overrides = materialized_attributes({
        {"http.retry_errors", {.data = string_list({"tls_verification", "redirect", "protocol"})}}
    });
    for (auto const kind : {HttpErrorKind::TlsVerification, HttpErrorKind::Redirect, HttpErrorKind::Protocol}) {
        auto mapped = map_http_completion(http_error(kind), payload.expected_statuses, overrides, {});
        REQUIRE(mapped);
        REQUIRE(mapped->failure_disposition);
        CHECK(*mapped->failure_disposition == FailureDisposition::Retryable);
    }
    auto connect = map_http_completion(http_error(HttpErrorKind::Connect), payload.expected_statuses, overrides, {});
    REQUIRE(connect);
    CHECK(*connect->failure_disposition == FailureDisposition::Terminal);

    auto backend = map_http_completion(http_error(HttpErrorKind::Internal, "net.http.backend_failed"),
                                       payload.expected_statuses,
                                       overrides,
                                       {});
    REQUIRE(backend);
    CHECK(*backend->failure_disposition == FailureDisposition::Terminal);

    auto unknown =
        map_http_completion(http_error(static_cast<HttpErrorKind>(255)), payload.expected_statuses, attributes, {});
    REQUIRE(unknown);
    CHECK(*unknown->failure_disposition == FailureDisposition::Terminal);
    CHECK(unknown->result.as_object().at("error_category").as_string() == "internal");
}

TEST_CASE("HTTP retry policy fails closed for corrupted materialized attributes", "[jobu][http][retry]")
{
    auto const payload  = decoded_payload();
    auto const observed = response(503U);

    for (auto const* const name : {"http.retry_errors", "http.retry_statuses", "retry.max_delay"}) {
        CAPTURE(name);
        auto attributes = materialized_attributes();
        REQUIRE(attributes.erase(name) == 1U);
        check_invalid_policy(map_http_completion(observed, payload.expected_statuses, attributes, {}));
    }

    auto wrong_list                         = materialized_attributes();
    wrong_list.at("http.retry_errors").data = true;
    check_invalid_policy(map_http_completion(observed, payload.expected_statuses, wrong_list, {}));

    auto wrong_element                         = materialized_attributes();
    wrong_element.at("http.retry_errors").data = AttributeValue::List{{.data = true}};
    check_invalid_policy(map_http_completion(observed, payload.expected_statuses, wrong_element, {}));

    auto duplicate_error                         = materialized_attributes();
    duplicate_error.at("http.retry_errors").data = string_list({"connect", "connect"});
    check_invalid_policy(map_http_completion(observed, payload.expected_statuses, duplicate_error, {}));

    auto invalid_status                           = materialized_attributes();
    invalid_status.at("http.retry_statuses").data = string_list({"private-selector"});
    auto invalid = map_http_completion(observed, payload.expected_statuses, invalid_status, {});
    check_invalid_policy(invalid);
    CHECK(invalid.error().message.find("private-selector") == std::string::npos);
    CHECK(invalid.error().detail.find("private-selector") == std::string::npos);

    auto duplicate_status                           = materialized_attributes();
    duplicate_status.at("http.retry_statuses").data = string_list({"503", "503"});
    check_invalid_policy(map_http_completion(observed, payload.expected_statuses, duplicate_status, {}));
}

TEST_CASE("Retry-After delay seconds are restricted clamped and overflow safe", "[jobu][http][retry-after]")
{
    auto const payload    = decoded_payload();
    auto const attributes = materialized_attributes();
    auto const completed  = UtcTimePoint{100s};

    auto accepted = map_http_completion(response(429U,
                                                 {
                                                     {.name = "retry-after", .value = "10"}
    }),
                                        payload.expected_statuses,
                                        attributes,
                                        completed);
    REQUIRE(accepted);
    REQUIRE(accepted->retry_not_before);
    CHECK(*accepted->retry_not_before == completed + 10s);
    check_retry_after(accepted->result, true, false);

    auto limited = materialized_attributes({
        {"retry.max_delay", {.data = Duration{5s}}}
    });
    auto clamped = map_http_completion(response(503U,
                                                {
                                                    {.name = "Retry-After", .value = "6"}
    }),
                                       payload.expected_statuses,
                                       limited,
                                       completed);
    REQUIRE(clamped);
    REQUIRE(clamped->retry_not_before);
    CHECK(*clamped->retry_not_before == completed + 5s);
    check_retry_after(clamped->result, true, true);

    auto zero = map_http_completion(response(429U,
                                             {
                                                 {.name = "Retry-After", .value = "0"}
    }),
                                    payload.expected_statuses,
                                    attributes,
                                    completed);
    REQUIRE(zero);
    REQUIRE(zero->retry_not_before);
    CHECK(*zero->retry_not_before == completed);
    check_retry_after(zero->result, true, false);

    auto overflow = map_http_completion(response(429U,
                                                 {
                                                     {.name = "Retry-After", .value = "18446744073709551616"}
    }),
                                        payload.expected_statuses,
                                        attributes,
                                        completed);
    REQUIRE(overflow);
    CHECK_FALSE(overflow->retry_not_before);
    check_retry_after(overflow->result, false, false);

    auto duplicate = map_http_completion(response(429U,
                                                  {
                                                      {.name = "Retry-After", .value = "10"},
                                                      {.name = "retry-after", .value = "20"},
    }),
                                         payload.expected_statuses,
                                         attributes,
                                         completed);
    REQUIRE(duplicate);
    CHECK_FALSE(duplicate->retry_not_before);
    check_retry_after(duplicate->result, false, false);

    auto near_max = map_http_completion(response(503U,
                                                 {
                                                     {.name = "Retry-After", .value = "5"}
    }),
                                        payload.expected_statuses,
                                        limited,
                                        UtcTimePoint::max() - 2s);
    REQUIRE(near_max);
    REQUIRE(near_max->retry_not_before);
    CHECK(*near_max->retry_not_before == UtcTimePoint::max());
    check_retry_after(near_max->result, true, true);

    auto status_408 = map_http_completion(response(408U,
                                                   {
                                                       {.name = "Retry-After", .value = "10"}
    }),
                                          payload.expected_statuses,
                                          attributes,
                                          completed);
    REQUIRE(status_408);
    CHECK_FALSE(status_408->retry_not_before);
    CHECK(retry_after(status_408->result) == nullptr);

    auto terminal_attributes = materialized_attributes({
        {"http.retry_statuses", {.data = string_list({})}}
    });
    auto terminal            = map_http_completion(response(429U,
                                                            {
                                                                {.name = "Retry-After", .value = "10"}
    }),
                                                   payload.expected_statuses,
                                                   terminal_attributes,
                                                   completed);
    REQUIRE(terminal);
    CHECK(*terminal->failure_disposition == FailureDisposition::Terminal);
    CHECK(retry_after(terminal->result) == nullptr);

    auto transport                = http_error(HttpErrorKind::Receive, "net.http.receive_failed");
    transport.error().status_code = 503U;
    transport.error().headers.push_back({.name = "Retry-After", .value = "10"});
    auto mapped_transport = map_http_completion(transport, payload.expected_statuses, attributes, completed);
    REQUIRE(mapped_transport);
    CHECK_FALSE(mapped_transport->retry_not_before);
    CHECK(retry_after(mapped_transport->result) == nullptr);
}

TEST_CASE("Retry-After IMF-fixdate parsing validates calendar weekday and clamp", "[jobu][http][retry-after]")
{
    auto const payload   = decoded_payload();
    auto const limited   = materialized_attributes({
        {"retry.max_delay", {.data = Duration{5s}}}
    });
    auto const completed = utc(1994, 11, 6, 8, 49, 30);

    auto accepted =
        map_http_completion(response(503U,
                                     {
                                         {.name = "Retry-After", .value = "Sun, 06 Nov 1994 08:49:37 GMT"}
    }),
                            payload.expected_statuses,
                            materialized_attributes(),
                            completed);
    REQUIRE(accepted);
    REQUIRE(accepted->retry_not_before);
    CHECK(*accepted->retry_not_before == utc(1994, 11, 6, 8, 49, 37));
    check_retry_after(accepted->result, true, false);

    auto clamped = map_http_completion(response(503U,
                                                {
                                                    {.name = "Retry-After", .value = "Sun, 06 Nov 1994 08:50:00 GMT"}
    }),
                                       payload.expected_statuses,
                                       limited,
                                       completed);
    REQUIRE(clamped);
    REQUIRE(clamped->retry_not_before);
    CHECK(*clamped->retry_not_before == completed + 5s);
    check_retry_after(clamped->result, true, true);

    auto leap_second =
        map_http_completion(response(503U,
                                     {
                                         {.name = "Retry-After", .value = "Sat, 31 Dec 2016 23:59:60 GMT"}
    }),
                            payload.expected_statuses,
                            materialized_attributes(),
                            utc(2016, 12, 31, 23, 59, 59));
    REQUIRE(leap_second);
    REQUIRE(leap_second->retry_not_before);
    CHECK(*leap_second->retry_not_before == utc(2017, 1, 1, 0, 0, 0));
    check_retry_after(leap_second->result, true, false);

    constexpr std::array invalid_values{
        "Mon, 06 Nov 1994 08:49:37 GMT",
        "Sun, 31 Feb 1994 08:49:37 GMT",
        "Sunday, 06-Nov-94 08:49:37 GMT",
        "Sun Nov  6 08:49:37 1994",
        "Sun, 06 Nov 1994 08:49:37 UTC",
        "sun, 06 Nov 1994 08:49:37 GMT",
        "Sun, 06 Nov 1994 08:49:61 GMT",
    };
    for (auto const* const value : invalid_values) {
        CAPTURE(value);
        auto mapped = map_http_completion(response(503U,
                                                   {
                                                       {.name = "Retry-After", .value = value}
        }),
                                          payload.expected_statuses,
                                          materialized_attributes(),
                                          completed);
        REQUIRE(mapped);
        CHECK_FALSE(mapped->retry_not_before);
        check_retry_after(mapped->result, false, false);
    }

    for (auto const* const value : {"Sun, 06 Nov 1994 08:49:30 GMT", "Sun, 06 Nov 1994 08:49:29 GMT"}) {
        CAPTURE(value);
        auto mapped = map_http_completion(response(503U,
                                                   {
                                                       {.name = "Retry-After", .value = value}
        }),
                                          payload.expected_statuses,
                                          materialized_attributes(),
                                          completed);
        REQUIRE(mapped);
        CHECK_FALSE(mapped->retry_not_before);
        check_retry_after(mapped->result, false, false);
    }
}

TEST_CASE("HTTP result JSON is deterministic bounded and excludes sensitive observations", "[jobu][http][result]")
{
    auto observed = HttpError{
        .kind = HttpErrorKind::Receive,
        .error =
            {
                    .category = ErrorCategory::Io,
                    .code     = "net.http.receive_failed",
                    .message  = "message-secret",
                    .detail   = "detail-secret",
                    },
        .status_code = 502U,
        .headers =
            {
                    {.name = "X-Secret", .value = "field-secret"},
                    {.name = "Retry-After", .value = "retry-secret"},
                    },
        .body =
            {
                    .bytes       = bytes("body-secret"),
                    .total_bytes = 20U,
                    .truncated   = true,
                    },
        .raw_headers =
            {
                    .bytes       = bytes("header-secret"),
                    .total_bytes = 13U,
                    .truncated   = false,
                    },
        .redirect_count = 1U,
        .elapsed        = 125ms,
        .tls_verified   = true,
    };

    auto const payload = decoded_payload();
    auto       mapped  = map_http_completion(HttpCompletionResult::failure(std::move(observed)),
                                             payload.expected_statuses,
                                             materialized_attributes(),
                                             {});
    REQUIRE(mapped);
    auto serialized = serialize_json(mapped->result);
    REQUIRE(serialized);
    CHECK(
        *serialized ==
        R"({"body":{"captured_bytes":11,"total_bytes":20,"truncated":true},"duration_ms":125,"error_category":"receive","error_code":"net.http.receive_failed","headers":{"captured_bytes":13,"total_bytes":13,"truncated":false},"outcome":"transport_error","redirects":1,"status":502,"tls_verified":true,"type":"http"})");
    CHECK(serialized->size() < std::size_t{256} * 1024U);
    for (auto const* const secret :
         {"body-secret", "header-secret", "field-secret", "retry-secret", "message-secret", "detail-secret"}) {
        CAPTURE(secret);
        CHECK(serialized->find(secret) == std::string::npos);
    }

    auto invalid_code = http_error(HttpErrorKind::Receive, std::string{"\xc3\x28", 2U});
    auto invalid      = map_http_completion(invalid_code, payload.expected_statuses, materialized_attributes(), {});
    REQUIRE_FALSE(invalid);
    CHECK(invalid.error().code == "jobu.http.invalid_result");

    auto oversized_code = http_error(HttpErrorKind::Receive, std::string((std::size_t{256} * 1024U), 'x'));
    auto oversized      = map_http_completion(oversized_code, payload.expected_statuses, materialized_attributes(), {});
    REQUIRE_FALSE(oversized);
    CHECK(oversized.error().code == "jobu.http.invalid_result");
}
