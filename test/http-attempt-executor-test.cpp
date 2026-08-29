#include "http/http_attempt_executor.hpp"

#include "attribute_registry.hpp"
#include "byte_buffer.hpp"
#include "json.hpp"
#include "logging.hpp"
#include "support/fake_http_client.hpp"
#include "support/fake_time_source.hpp"
#include "uuid.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace jb::core;
using namespace jb::jobu;
using namespace jb::jobu::http;
using namespace jb::net;
using namespace jb::test;
using namespace std::chrono_literals;

namespace {

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

auto bytes(std::string_view value) -> ByteBuffer
{
    auto const view = as_bytes(value);
    return ByteBuffer{view.begin(), view.end()};
}

auto byte_text(ByteBuffer const& value) -> std::string_view
{
    return as_string_view(ByteView{value});
}

auto uuid(std::string_view value) -> Uuid
{
    auto parsed = Uuid::parse(value);
    REQUIRE(parsed);
    return std::move(parsed).value();
}

auto job_id() -> Uuid
{
    return uuid("11111111-1111-4111-8111-111111111111");
}

auto run_id() -> Uuid
{
    return uuid("22222222-2222-4222-8222-222222222222");
}

auto queue_id() -> Uuid
{
    return uuid("33333333-3333-4333-8333-333333333333");
}

auto http_payload() -> JsonValue
{
    return json_object({
        {"body",
         json_object({
             {"data", json_string("secret-body")},
             {"encoding", json_string("utf8")},
         })                                                                         },
        {"headers",
         json_array({json_object({
             {"name", json_string("X-Test-Input")},
             {"sensitive", json_bool(true)},
             {"value", json_string("secret-header")},
         })})                                                                       },
        {"method",  json_string("POST")                                             },
        {"url",     json_string("https://sensitive.example/path?token=secret-query")},
    });
}

auto maximum_header_payload() -> JsonValue
{
    // Four worst-case executor metadata headers occupy 183 generic name/value bytes.
    constexpr std::size_t maximum_count{128U - 4U};
    constexpr std::size_t maximum_bytes{(std::size_t{64} * 1024U) - 183U};

    auto name_bytes = std::size_t{0};
    for (std::size_t index = 0; index < maximum_count; ++index) {
        name_bytes += ("X-" + std::to_string(index)).size();
    }

    auto headers = JsonValue::Array{};
    headers.reserve(maximum_count);
    for (std::size_t index = 0; index < maximum_count; ++index) {
        headers.push_back(json_object({
            {"name", json_string("X-" + std::to_string(index))},
            {"value", json_string(index == 0U ? std::string(maximum_bytes - name_bytes, 'a') : "")},
        }));
    }
    return json_object({
        {"headers", json_array(std::move(headers))              },
        {"url",     json_string("https://example.test/boundary")},
    });
}

auto materialized_attributes(AttributeSet const& overrides = {}) -> AttributeSet
{
    StandardAttributeRegistry registry;
    auto                      attributes = materialize_attributes(registry, {}, {}, overrides);
    REQUIRE(attributes);
    return std::move(attributes).value();
}

auto start_request(AttemptNumber attempt_number = 1, AttributeSet const& overrides = {}) -> AttemptStartRequest
{
    return {
        .key        = {.run_id = run_id(), .attempt_number = attempt_number},
        .job_id     = job_id(),
        .queue_id   = queue_id(),
        .type       = JobType::Http,
        .attributes = materialized_attributes(overrides),
        .payload    = http_payload(),
        .started_at = UtcTimePoint{10s},
    };
}

auto capture(std::string_view value, std::uint64_t total_bytes = 0) -> HttpCapturedData
{
    auto retained = bytes(value);
    auto total    = total_bytes == 0 ? static_cast<std::uint64_t>(retained.size()) : total_bytes;
    return {
        .bytes       = std::move(retained),
        .total_bytes = total,
        .truncated   = total > value.size(),
    };
}

auto response(std::uint16_t status = 204U, std::vector<HttpHeader> headers = {}) -> HttpResponse
{
    return {
        .status_code    = status,
        .headers        = std::move(headers),
        .body           = capture("response-body"),
        .raw_headers    = capture("HTTP/1.1 response-headers\r\n\r\n"),
        .redirect_count = 1,
        .elapsed        = 125ms,
        .tls_verified   = true,
    };
}

auto transport_error(HttpErrorKind kind = HttpErrorKind::Receive) -> HttpError
{
    return {
        .kind = kind,
        .error =
            {
                    .category = ErrorCategory::Io,
                    .code     = "net.http.receive_failed",
                    .message  = "Safe transport failure",
                    },
        .body         = capture("partial-body"),
        .raw_headers  = capture("partial-headers"),
        .elapsed      = 50ms,
        .tls_verified = true,
    };
}

auto header_value(HttpRequest const& request, std::string_view name) -> std::optional<std::string_view>
{
    for (auto const& header : request.headers) {
        if (header.name == name) {
            return header.value;
        }
    }
    return std::nullopt;
}

auto result_string(JsonValue const& result, std::string_view name) -> std::string_view
{
    auto const& object = result.as_object();
    auto const  found  = object.find(name);
    REQUIRE(found != object.end());
    return found->second.as_string();
}

struct LogRecord {
    LogLevel    level{LogLevel::Warning};
    std::string message;
};

class CaptureLogger final : public Logger {
public:
    void log(LogMessage const& message) override
    {
        std::lock_guard lock{_mutex};
        _records.push_back({.level = message.level, .message = std::string{message.message}});
    }

    [[nodiscard]] auto records() const -> std::vector<LogRecord>
    {
        std::lock_guard lock{_mutex};
        return _records;
    }

private:
    mutable std::mutex     _mutex;
    std::vector<LogRecord> _records;
};

class LoggerGuard final {
public:
    LoggerGuard()
        : capture{std::make_shared<CaptureLogger>()}
        , _previous{logger()}
    {
        capture->set_level(LogLevel::Warning);
        set_logger(capture);
    }

    ~LoggerGuard() { set_logger(std::move(_previous)); }

    LoggerGuard(LoggerGuard const&)                    = delete;
    auto operator=(LoggerGuard const&) -> LoggerGuard& = delete;

    std::shared_ptr<CaptureLogger> capture;

private:
    std::shared_ptr<Logger> _previous;
};

} // anonymous namespace

TEST_CASE("HTTP attempt executor builds owning requests and stable retry metadata", "[jobu][http][executor]")
{
    FakeHttpClient      client;
    FakeTimeSource      time;
    HttpAttemptExecutor executor{client, time};

    CHECK(executor.is_available(JobType::Http));
    CHECK_FALSE(executor.is_available(JobType::Cli));
    CHECK_FALSE(executor.is_available(static_cast<JobType>(255)));
    client.set_available(false);
    CHECK_FALSE(executor.is_available(JobType::Http));
    client.set_available(true);

    auto overrides = AttributeSet{
        {"http.follow_redirects",     {.data = true}                 },
        {"http.idempotency_key",      {.data = true}                 },
        {"http.max_redirects",        {.data = std::int64_t{7}}      },
        {"job.timeout",               {.data = 42s}                  },
        {"output.capture",            {.data = std::string{"always"}}},
        {"output.http_body_limit",    {.data = std::int64_t{64}}     },
        {"output.http_headers_limit", {.data = std::int64_t{32}}     },
    };
    auto completions = std::vector<AttemptCompletion>{};
    auto started = executor.start(start_request(1, overrides),
                                  [&](AttemptCompletion completion) { completions.push_back(std::move(completion)); });
    REQUIRE(started);
    CHECK(completions.empty());
    REQUIRE(client.start_records().size() == 1U);

    auto const& first = client.start_records().front();
    CHECK(first.request.method == "POST");
    CHECK(first.request.url == "https://sensitive.example/path?token=secret-query");
    REQUIRE(first.request.body);
    CHECK(byte_text(*first.request.body) == "secret-body");
    CHECK(first.request.timeout == 42s);
    CHECK(first.request.verify_tls);
    CHECK(first.request.follow_redirects);
    CHECK(first.request.max_redirects == 7U);
    CHECK(first.request.response_body_limit == 64U);
    CHECK(first.request.response_header_limit == 32U);
    CHECK(header_value(first.request, "X-JobU-Job-ID") == job_id().to_string());
    CHECK(header_value(first.request, "X-JobU-Run-ID") == run_id().to_string());
    CHECK(header_value(first.request, "X-JobU-Attempt") == "1");
    CHECK(header_value(first.request, "Idempotency-Key") == run_id().to_string());

    REQUIRE(client.complete_success(first.id, response()));
    REQUIRE(completions.size() == 1U);
    CHECK(completions[0].key == AttemptKey{.run_id = run_id(), .attempt_number = 1});
    CHECK(completions[0].outcome == AttemptOutcome::Succeeded);
    REQUIRE(completions[0].output);
    REQUIRE(completions[0].output->primary);
    REQUIRE(completions[0].output->diagnostic);
    CHECK(byte_text(completions[0].output->primary->bytes) == "response-body");
    CHECK(byte_text(completions[0].output->diagnostic->bytes) == "HTTP/1.1 response-headers\r\n\r\n");
    auto const first_run_header         = std::string{*header_value(first.request, "X-JobU-Run-ID")};
    auto const first_idempotency_header = std::string{*header_value(first.request, "Idempotency-Key")};

    REQUIRE(executor.start(start_request(2, overrides),
                           [&](AttemptCompletion completion) { completions.push_back(std::move(completion)); }));
    REQUIRE(client.start_records().size() == 2U);
    auto const& retry = client.start_records().back();
    CHECK(header_value(retry.request, "X-JobU-Run-ID") == first_run_header);
    CHECK(header_value(retry.request, "Idempotency-Key") == first_idempotency_header);
    CHECK(header_value(retry.request, "X-JobU-Attempt") == "2");
    REQUIRE(client.complete_success(retry.id, response()));
    REQUIRE(completions.size() == 2U);
    CHECK(completions[1].key.attempt_number == 2U);
}

TEST_CASE("HTTP attempt executor preserves generic limits after metadata injection", "[jobu][http][executor]")
{
    FakeHttpClient      client;
    FakeTimeSource      time;
    HttpAttemptExecutor executor{client, time};

    auto request     = start_request(std::numeric_limits<AttemptNumber>::max(),
                                     {
                                         {"http.idempotency_key", {.data = true}}
    });
    request.payload  = maximum_header_payload();
    auto completions = std::size_t{0};
    REQUIRE(executor.start(std::move(request), [&](AttemptCompletion const&) { ++completions; }));
    REQUIRE(client.start_records().size() == 1U);

    auto const& accepted = client.start_records().front();
    CHECK(accepted.request.headers.size() == 128U);
    auto header_bytes = std::size_t{0};
    for (auto const& header : accepted.request.headers) {
        header_bytes += header.name.size() + header.value.size();
    }
    CHECK(header_bytes == std::size_t{64} * 1024U);
    CHECK(header_value(accepted.request, "X-JobU-Attempt") ==
          std::to_string(std::numeric_limits<AttemptNumber>::max()));
    CHECK(header_value(accepted.request, "Idempotency-Key") == run_id().to_string());

    REQUIRE(client.complete_success(accepted.id, response()));
    CHECK(completions == 1U);
}

TEST_CASE("HTTP attempt executor rejects invalid starts without retaining callbacks", "[jobu][http][executor]")
{
    FakeHttpClient      client;
    FakeTimeSource      time;
    HttpAttemptExecutor executor{client, time};
    auto                callback_count = std::size_t{0};
    auto                callback       = [&](AttemptCompletion const&) { ++callback_count; };

    SECTION("unsupported type")
    {
        auto request = start_request();
        request.type = JobType::Cli;
        auto result  = executor.start(std::move(request), callback);
        REQUIRE_FALSE(result);
        CHECK(result.error().code == "jobu.http.unsupported_type");
    }

    SECTION("zero attempt number")
    {
        auto request               = start_request();
        request.key.attempt_number = 0;
        auto result                = executor.start(std::move(request), callback);
        REQUIRE_FALSE(result);
        CHECK(result.error().code == "jobu.http.invalid_start");
    }

    SECTION("empty completion handler")
    {
        auto result = executor.start(start_request(), {});
        REQUIRE_FALSE(result);
        CHECK(result.error().code == "jobu.http.invalid_start");
    }

    SECTION("missing materialized attribute")
    {
        auto request = start_request();
        request.attributes.erase("http.tls_verify");
        auto result = executor.start(std::move(request), callback);
        REQUIRE_FALSE(result);
        CHECK(result.error().code == "jobu.http.invalid_snapshot");
    }

    SECTION("wrong typed request attribute")
    {
        auto request = start_request();
        request.attributes.insert_or_assign("http.follow_redirects", AttributeValue{.data = std::int64_t{1}});
        auto result = executor.start(std::move(request), callback);
        REQUIRE_FALSE(result);
        CHECK(result.error().code == "jobu.http.invalid_snapshot");
    }

    SECTION("out of range capture attribute")
    {
        auto request = start_request();
        request.attributes.insert_or_assign("output.http_headers_limit",
                                            AttributeValue{.data = (std::int64_t{4} * 1024 * 1024) + 1});
        auto result = executor.start(std::move(request), callback);
        REQUIRE_FALSE(result);
        CHECK(result.error().code == "jobu.http.invalid_snapshot");
    }

    SECTION("invalid payload snapshot")
    {
        auto request    = start_request();
        request.payload = json_object({});
        auto result     = executor.start(std::move(request), callback);
        REQUIRE_FALSE(result);
        CHECK(result.error().code == "jobu.http.invalid_snapshot");
    }

    SECTION("client rejects start")
    {
        client.set_start_error(Error{
            .category = ErrorCategory::Unavailable,
            .code     = "net.http.unavailable",
            .message  = "Safe rejection",
        });
        auto result = executor.start(start_request(), callback);
        REQUIRE_FALSE(result);
        CHECK(result.error().code == "jobu.http.start_failed");
        CHECK(result.error().detail == "net.http.unavailable");
    }

    CHECK(callback_count == 0U);
    CHECK(client.start_records().empty());
    CHECK(client.active_request_count() == 0U);
}

TEST_CASE("HTTP attempt executor rejects duplicate active keys", "[jobu][http][executor]")
{
    FakeHttpClient      client;
    FakeTimeSource      time;
    HttpAttemptExecutor executor{client, time};
    auto                completions = std::vector<AttemptCompletion>{};

    REQUIRE(executor.start(start_request(),
                           [&](AttemptCompletion completion) { completions.push_back(std::move(completion)); }));
    auto duplicate = executor.start(start_request(), [&](AttemptCompletion completion) {
        completions.push_back(std::move(completion));
    });
    REQUIRE_FALSE(duplicate);
    CHECK(duplicate.error().code == "jobu.http.duplicate_attempt");
    CHECK(client.start_records().size() == 1U);
    CHECK(completions.empty());

    REQUIRE(client.complete_success(client.start_records().front().id, response()));
    CHECK(completions.size() == 1U);
}

TEST_CASE("HTTP attempt executor applies capture policy after outcome mapping", "[jobu][http][executor]")
{
    struct Case {
        std::string_view capture_mode;
        std::uint16_t    status;
        bool             expect_output;
        bool             expect_limits;
    };

    constexpr std::array cases{
        Case{.capture_mode = "none",     .status = 204U, .expect_output = false, .expect_limits = false},
        Case{.capture_mode = "on_error", .status = 204U, .expect_output = false, .expect_limits = true },
        Case{.capture_mode = "on_error", .status = 503U, .expect_output = true,  .expect_limits = true },
        Case{.capture_mode = "always",   .status = 204U, .expect_output = true,  .expect_limits = true },
    };

    for (auto const& test : cases) {
        DYNAMIC_SECTION(test.capture_mode << " status " << test.status)
        {
            FakeHttpClient      client;
            FakeTimeSource      time;
            HttpAttemptExecutor executor{client, time};
            auto                completions = std::vector<AttemptCompletion>{};
            auto                overrides   = AttributeSet{
                {"output.capture", {.data = std::string{test.capture_mode}}}
            };

            REQUIRE(executor.start(start_request(1, overrides), [&](AttemptCompletion completion) {
                completions.push_back(std::move(completion));
            }));
            auto const& started = client.start_records().front();
            CHECK(started.request.response_body_limit == (test.expect_limits ? std::size_t{1024} * 1024U : 0U));
            CHECK(started.request.response_header_limit == (test.expect_limits ? std::size_t{64} * 1024U : 0U));

            REQUIRE(client.complete_success(started.id, response(test.status)));
            REQUIRE(completions.size() == 1U);
            CHECK(static_cast<bool>(completions.front().output) == test.expect_output);
            if (test.status == 204U) {
                CHECK(completions.front().outcome == AttemptOutcome::Succeeded);
            }
            else {
                CHECK(completions.front().outcome == AttemptOutcome::Failed);
                CHECK(completions.front().failure_disposition == FailureDisposition::Retryable);
            }
            if (test.expect_output) {
                REQUIRE(completions.front().output->primary);
                CHECK(byte_text(completions.front().output->primary->bytes) == "response-body");
            }
            auto serialized = serialize_json(completions.front().result);
            REQUIRE(serialized);
            CHECK(serialized->find("response-body") == std::string::npos);
            CHECK(serialized->find("response-headers") == std::string::npos);
        }
    }
}

TEST_CASE("HTTP attempt executor maps Retry-After and transport observations with fake time", "[jobu][http][executor]")
{
    FakeHttpClient client;
    FakeTimeSource time;
    time.set_utc(UtcTimePoint{100s});
    HttpAttemptExecutor executor{client, time};
    auto                completions = std::vector<AttemptCompletion>{};

    auto overrides = AttributeSet{
        {"output.capture", {.data = std::string{"on_error"}}}
    };
    REQUIRE(executor.start(start_request(1, overrides),
                           [&](AttemptCompletion completion) { completions.push_back(std::move(completion)); }));
    auto const first_id = client.start_records().back().id;
    REQUIRE(client.complete_success(first_id,
                                    response(503U,
                                             {
                                                 {.name = "Retry-After", .value = "10"}
    })));
    REQUIRE(completions.size() == 1U);
    CHECK(completions[0].outcome == AttemptOutcome::Failed);
    CHECK(completions[0].failure_disposition == FailureDisposition::Retryable);
    CHECK(completions[0].retry_not_before == UtcTimePoint{110s});
    REQUIRE(completions[0].output);

    REQUIRE(executor.start(start_request(2, overrides),
                           [&](AttemptCompletion completion) { completions.push_back(std::move(completion)); }));
    auto const second_id = client.start_records().back().id;
    REQUIRE(client.complete_error(second_id, transport_error()));
    REQUIRE(completions.size() == 2U);
    CHECK(completions[1].outcome == AttemptOutcome::Failed);
    CHECK(completions[1].failure_disposition == FailureDisposition::Retryable);
    CHECK(result_string(completions[1].result, "error_category") == "receive");
    REQUIRE(completions[1].output);
    REQUIRE(completions[1].output->primary);
    CHECK(byte_text(completions[1].output->primary->bytes) == "partial-body");
}

TEST_CASE("HTTP attempt executor preserves callback obligation through cancellation", "[jobu][http][executor]")
{
    FakeHttpClient      client;
    FakeTimeSource      time;
    HttpAttemptExecutor executor{client, time};
    auto                completions = std::vector<AttemptCompletion>{};
    auto                request     = start_request();
    auto const          key         = request.key;

    auto unknown = executor.cancel(key);
    REQUIRE_FALSE(unknown);
    CHECK(unknown.error().code == "jobu.http.attempt_not_found");

    REQUIRE(executor.start(std::move(request),
                           [&](AttemptCompletion completion) { completions.push_back(std::move(completion)); }));
    auto const request_id = client.start_records().front().id;
    client.set_cancel_error(Error{
        .category = ErrorCategory::Unavailable,
        .code     = "net.http.test_cancel_rejected",
        .message  = "Safe cancellation rejection",
    });
    auto rejected = executor.cancel(key);
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().code == "jobu.http.cancel_failed");
    CHECK(client.active_request_count() == 1U);
    CHECK(completions.empty());

    client.set_cancel_error(std::nullopt);
    REQUIRE(executor.cancel(key));
    CHECK(completions.empty());
    auto repeated = executor.cancel(key);
    REQUIRE_FALSE(repeated);
    CHECK(repeated.error().code == "jobu.http.cancel_failed");

    auto partial = transport_error(HttpErrorKind::Cancelled);
    REQUIRE(client.complete_cancelled(request_id, std::move(partial)));
    REQUIRE(completions.size() == 1U);
    CHECK(completions[0].key == key);
    CHECK(completions[0].outcome == AttemptOutcome::Cancelled);
    CHECK_FALSE(completions[0].failure_disposition);
    REQUIRE(completions[0].output);
    REQUIRE(completions[0].output->primary);
    CHECK(byte_text(completions[0].output->primary->bytes) == "partial-body");

    auto duplicate = client.complete_cancelled(request_id);
    REQUIRE_FALSE(duplicate);
    CHECK(duplicate.error().code == "test.http.duplicate_completion");
}

TEST_CASE("HTTP attempt executor handles shared client failure and permanent unavailability", "[jobu][http][executor]")
{
    FakeHttpClient      client;
    FakeTimeSource      time;
    HttpAttemptExecutor executor{client, time};
    auto                completions  = std::vector<AttemptCompletion>{};
    auto                failed_count = std::size_t{0};
    client.failed.connect([&](Error const&) { ++failed_count; });

    REQUIRE(executor.start(start_request(1),
                           [&](AttemptCompletion completion) { completions.push_back(std::move(completion)); }));
    REQUIRE(executor.start(start_request(2),
                           [&](AttemptCompletion completion) { completions.push_back(std::move(completion)); }));
    REQUIRE(client.inject_shared_failure({
        .category = ErrorCategory::Internal,
        .code     = "net.http.backend_failed",
        .message  = "The fake HTTP backend failed",
    }));

    CHECK(failed_count == 1U);
    CHECK_FALSE(executor.is_available(JobType::Http));
    CHECK(client.active_request_count() == 0U);
    REQUIRE(completions.size() == 2U);
    for (auto const& completion : completions) {
        CHECK(completion.outcome == AttemptOutcome::Failed);
        CHECK(completion.failure_disposition == FailureDisposition::Terminal);
        CHECK(result_string(completion.result, "error_code") == "net.http.backend_failed");
    }

    auto callback_count = std::size_t{0};
    auto rejected       = executor.start(start_request(3), [&](AttemptCompletion const&) { ++callback_count; });
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().code == "jobu.http.start_failed");
    CHECK(callback_count == 0U);
}

TEST_CASE("HTTP attempt executor retires state before reentrant completion callbacks", "[jobu][http][executor]")
{
    FakeHttpClient      client;
    FakeTimeSource      time;
    HttpAttemptExecutor executor{client, time};
    auto                completions = std::vector<AttemptCompletion>{};
    auto                reentered   = std::optional<Result<void, Error>>{};

    REQUIRE(executor.start(start_request(), [&](AttemptCompletion completion) {
        completions.push_back(std::move(completion));
        reentered.emplace(executor.start(start_request(),
                                         [&](AttemptCompletion nested) { completions.push_back(std::move(nested)); }));
    }));
    auto const first_id = client.start_records().front().id;
    REQUIRE(client.complete_success(first_id, response()));
    REQUIRE(reentered);
    REQUIRE(*reentered);
    REQUIRE(client.start_records().size() == 2U);

    REQUIRE(client.complete_success(client.start_records().back().id, response()));
    REQUIRE(completions.size() == 2U);
    CHECK(completions[0].key == completions[1].key);
}

TEST_CASE("HTTP attempt executor converts callback identity mismatch to one safe terminal completion",
          "[jobu][http][executor]")
{
    FakeHttpClient      client;
    FakeTimeSource      time;
    HttpAttemptExecutor executor{client, time};
    auto                completions = std::vector<AttemptCompletion>{};

    REQUIRE(executor.start(start_request(),
                           [&](AttemptCompletion completion) { completions.push_back(std::move(completion)); }));
    auto const request_id = client.start_records().front().id;
    REQUIRE(client.complete_success(request_id, response(), request_id + 100U));
    REQUIRE(completions.size() == 1U);
    CHECK(completions[0].outcome == AttemptOutcome::Failed);
    CHECK(completions[0].failure_disposition == FailureDisposition::Terminal);
    CHECK(result_string(completions[0].result, "error_code") == "jobu.http.completion_failed");

    REQUIRE(executor.start(start_request(),
                           [&](AttemptCompletion completion) { completions.push_back(std::move(completion)); }));
}

TEST_CASE("HTTP attempt executor destruction suppresses late client cancellation completion", "[jobu][http][executor]")
{
    FakeHttpClient client;
    FakeTimeSource time;
    auto           callback_count = std::size_t{0};
    auto           request_id     = HttpRequestId{0};

    {
        HttpAttemptExecutor executor{client, time};
        REQUIRE(executor.start(start_request(), [&](AttemptCompletion const&) { ++callback_count; }));
        request_id = client.start_records().front().id;
    }

    REQUIRE(client.cancel_calls().size() == 1U);
    CHECK(client.cancel_calls().front() == request_id);
    CHECK(callback_count == 0U);
    REQUIRE(client.complete_cancelled(request_id));
    CHECK(callback_count == 0U);
}

TEST_CASE("HTTP attempt executor TLS warning contains identifiers but no request data", "[jobu][http][executor]")
{
    FakeHttpClient      client;
    FakeTimeSource      time;
    LoggerGuard         logger_guard;
    HttpAttemptExecutor executor{client, time};
    auto                completions = std::vector<AttemptCompletion>{};
    auto                overrides   = AttributeSet{
        {"http.tls_verify", {.data = false}}
    };

    REQUIRE(executor.start(start_request(3, overrides),
                           [&](AttemptCompletion completion) { completions.push_back(std::move(completion)); }));
    auto records = logger_guard.capture->records();
    REQUIRE(records.size() == 1U);
    CHECK(records.front().level == LogLevel::Warning);
    CHECK(records.front().message.find(job_id().to_string()) != std::string::npos);
    CHECK(records.front().message.find(run_id().to_string()) != std::string::npos);
    CHECK(records.front().message.find("attempt 3") != std::string::npos);
    for (auto const* const forbidden : {
             "sensitive.example",
             "secret-query",
             "X-Test-Input",
             "secret-header",
             "secret-body",
             "false",
             "on_error",
             "120",
         }) {
        CHECK(records.front().message.find(forbidden) == std::string::npos);
    }

    REQUIRE(client.complete_success(client.start_records().front().id, response()));
    CHECK(completions.size() == 1U);
}
