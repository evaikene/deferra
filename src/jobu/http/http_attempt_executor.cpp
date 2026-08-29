#include "http_attempt_executor.hpp"

#include "attribute_registry.hpp"
#include "http_job_payload_priv.hpp"
#include "http_retry_priv.hpp"
#include "json.hpp"
#include "logging.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>

namespace jb::jobu::http {

namespace {

template <typename T = void>
using ExecutorResult = jb::core::Result<T, jb::core::Error>;

enum class CaptureMode : std::uint8_t {
    None,
    OnError,
    Always,
};

struct RequestPolicy {
    jb::core::Duration timeout{};
    bool               verify_tls{true};
    bool               follow_redirects{false};
    bool               idempotency_key{false};
    std::uint32_t      max_redirects{0};
    std::size_t        body_limit{0};
    std::size_t        header_limit{0};
    CaptureMode        capture{CaptureMode::None};
};

auto executor_error(jb::core::ErrorCategory category, std::string code, std::string message, std::string detail = {})
    -> jb::core::Error
{
    return {
        .category = category,
        .code     = std::move(code),
        .message  = std::move(message),
        .detail   = std::move(detail),
    };
}

auto invalid_snapshot(std::string detail) -> jb::core::Error
{
    return executor_error(jb::core::ErrorCategory::Internal,
                          "jobu.http.invalid_snapshot",
                          "The durable HTTP attempt snapshot is invalid",
                          std::move(detail));
}

template <typename T>
auto snapshot_attribute(AttributeSet const& attributes, std::string_view name) -> ExecutorResult<T const*>
{
    auto const found = attributes.find(name);
    if (found == attributes.end()) {
        return ExecutorResult<T const*>::failure(invalid_snapshot("attribute.missing"));
    }
    auto const* value = std::get_if<T>(&found->second.data);
    if (value == nullptr) {
        return ExecutorResult<T const*>::failure(invalid_snapshot("attribute.invalid_type"));
    }
    return ExecutorResult<T const*>::success(value);
}

auto decode_request_policy(AttributeSet const& attributes, StandardAttributeRegistry const& registry)
    -> ExecutorResult<RequestPolicy>
{
    auto validated = registry.validate_materialized(attributes);
    if (!validated) {
        return ExecutorResult<RequestPolicy>::failure(invalid_snapshot("attributes.invalid"));
    }

    auto timeout          = snapshot_attribute<jb::core::Duration>(attributes, "job.timeout");
    auto verify_tls       = snapshot_attribute<bool>(attributes, "http.tls_verify");
    auto follow_redirects = snapshot_attribute<bool>(attributes, "http.follow_redirects");
    auto idempotency_key  = snapshot_attribute<bool>(attributes, "http.idempotency_key");
    auto max_redirects    = snapshot_attribute<std::int64_t>(attributes, "http.max_redirects");
    auto body_limit       = snapshot_attribute<std::int64_t>(attributes, "output.http_body_limit");
    auto header_limit     = snapshot_attribute<std::int64_t>(attributes, "output.http_headers_limit");
    auto capture          = snapshot_attribute<std::string>(attributes, "output.capture");
    if (!timeout || !verify_tls || !follow_redirects || !idempotency_key || !max_redirects || !body_limit ||
        !header_limit || !capture) {
        return ExecutorResult<RequestPolicy>::failure(invalid_snapshot("attributes.invalid"));
    }

    auto capture_mode = CaptureMode::None;
    if (**capture == "on_error") {
        capture_mode = CaptureMode::OnError;
    }
    else if (**capture == "always") {
        capture_mode = CaptureMode::Always;
    }
    else if (**capture != "none") {
        return ExecutorResult<RequestPolicy>::failure(invalid_snapshot("attribute.invalid_capture"));
    }

    return ExecutorResult<RequestPolicy>::success({
        .timeout          = **timeout,
        .verify_tls       = **verify_tls,
        .follow_redirects = **follow_redirects,
        .idempotency_key  = **idempotency_key,
        .max_redirects    = static_cast<std::uint32_t>(**max_redirects),
        .body_limit       = capture_mode == CaptureMode::None ? 0U : static_cast<std::size_t>(**body_limit),
        .header_limit     = capture_mode == CaptureMode::None ? 0U : static_cast<std::size_t>(**header_limit),
        .capture          = capture_mode,
    });
}

auto internal_completion_policy() -> detail::HttpCompletionPolicy
{
    auto result = jb::core::JsonValue::Object{};
    result.emplace("error_category", jb::core::JsonValue{.data = std::string{"internal"}});
    result.emplace("error_code", jb::core::JsonValue{.data = std::string{"jobu.http.completion_failed"}});
    result.emplace("outcome", jb::core::JsonValue{.data = std::string{"internal_error"}});
    result.emplace("type", jb::core::JsonValue{.data = std::string{"http"}});
    return {
        .outcome             = AttemptOutcome::Failed,
        .failure_disposition = FailureDisposition::Terminal,
        .retry_not_before    = std::nullopt,
        .result              = jb::core::JsonValue{.data = std::move(result)},
    };
}

auto output_channel(jb::net::HttpCapturedData data) -> AttemptOutputChannel
{
    return {
        .bytes       = std::move(data.bytes),
        .total_bytes = data.total_bytes,
        .truncated   = data.truncated,
    };
}

auto captured_output(jb::net::HttpCompletionResult& transfer) -> AttemptOutput
{
    if (transfer) {
        auto& response = *transfer;
        return {
            .primary      = output_channel(std::move(response.body)),
            .diagnostic   = output_channel(std::move(response.raw_headers)),
            .capture_lost = false,
        };
    }
    auto& error = transfer.error();
    return {
        .primary      = output_channel(std::move(error.body)),
        .diagnostic   = output_channel(std::move(error.raw_headers)),
        .capture_lost = false,
    };
}

auto should_capture(CaptureMode mode, AttemptOutcome outcome) noexcept -> bool
{
    return mode == CaptureMode::Always || (mode == CaptureMode::OnError && outcome != AttemptOutcome::Succeeded);
}

struct AttemptKeyHash {
    auto operator()(AttemptKey const& key) const noexcept -> std::size_t
    {
        auto seed = std::hash<jb::core::Uuid>{}(key.run_id);
        seed ^= std::hash<AttemptNumber>{}(key.attempt_number) + std::size_t{0x9e3779b9U} + (seed << 6U) + (seed >> 2U);
        return seed;
    }
};

} // anonymous namespace

struct HttpAttemptExecutor::Private {
    struct ActiveAttempt {
        Private*                 owner{nullptr};
        AttemptKey               key;
        jb::net::HttpRequestId   request_id{0};
        AttributeSet             attributes;
        detail::HttpStatusSet    expected_statuses;
        CaptureMode              capture{CaptureMode::None};
        AttemptCompletionHandler completion;
    };

    Private(jb::net::HttpClient& client_value, jb::core::TimeSource& time_source_value)
        : client{client_value}
        , time_source{time_source_value}
    {}

    jb::net::HttpClient&                                                       client;
    jb::core::TimeSource&                                                      time_source;
    StandardAttributeRegistry                                                  attributes;
    std::unordered_map<AttemptKey, jb::net::HttpRequestId, AttemptKeyHash>     request_by_attempt;
    std::unordered_map<jb::net::HttpRequestId, std::shared_ptr<ActiveAttempt>> attempt_by_request;

    [[nodiscard]] auto start(AttemptStartRequest request, AttemptCompletionHandler completion) -> ExecutorResult<>
    {
        if (request.type != JobType::Http) {
            return ExecutorResult<>::failure(executor_error(jb::core::ErrorCategory::Unsupported,
                                                            "jobu.http.unsupported_type",
                                                            "The HTTP executor supports only HTTP attempts"));
        }
        if (!completion || request.key.attempt_number == 0) {
            return ExecutorResult<>::failure(executor_error(jb::core::ErrorCategory::InvalidArgument,
                                                            "jobu.http.invalid_start",
                                                            "The HTTP attempt start input is invalid"));
        }
        if (request_by_attempt.contains(request.key)) {
            return ExecutorResult<>::failure(executor_error(jb::core::ErrorCategory::Conflict,
                                                            "jobu.http.duplicate_attempt",
                                                            "The HTTP attempt is already active"));
        }

        auto policy = decode_request_policy(request.attributes, attributes);
        if (!policy) {
            return ExecutorResult<>::failure(std::move(policy).error());
        }
        auto payload = detail::decode_http_job_payload(request.payload);
        if (!payload) {
            return ExecutorResult<>::failure(
                invalid_snapshot(std::string{detail::job_payload_issue_text(payload.error())}));
        }

        auto http_request = jb::net::HttpRequest{
            .method                = std::move(payload->method),
            .url                   = std::move(payload->url),
            .headers               = std::move(payload->headers),
            .body                  = std::move(payload->body),
            .timeout               = policy->timeout,
            .verify_tls            = policy->verify_tls,
            .follow_redirects      = policy->follow_redirects,
            .max_redirects         = policy->max_redirects,
            .response_body_limit   = policy->body_limit,
            .response_header_limit = policy->header_limit,
        };

        auto const job_id = request.job_id.to_string();
        auto const run_id = request.key.run_id.to_string();
        http_request.headers.push_back({.name = "X-JobU-Job-ID", .value = job_id});
        http_request.headers.push_back({.name = "X-JobU-Run-ID", .value = run_id});
        http_request.headers.push_back({.name = "X-JobU-Attempt", .value = std::to_string(request.key.attempt_number)});
        if (policy->idempotency_key) {
            http_request.headers.push_back({.name = "Idempotency-Key", .value = run_id});
        }
        if (!policy->verify_tls) {
            jb::core::log_warning("HTTP TLS verification disabled for job {} run {} attempt {}",
                                  job_id,
                                  run_id,
                                  request.key.attempt_number);
        }

        auto active  = std::make_shared<ActiveAttempt>(ActiveAttempt{
            .owner             = this,
            .key               = request.key,
            .attributes        = std::move(request.attributes),
            .expected_statuses = std::move(payload->expected_statuses),
            .capture           = policy->capture,
            .completion        = std::move(completion),
        });
        auto started = client.start(
            std::move(http_request),
            [active](jb::net::HttpRequestId request_id, jb::net::HttpCompletionResult transfer) mutable -> void {
                if (active->owner != nullptr) {
                    active->owner->complete(active, request_id, std::move(transfer));
                }
            });
        if (!started) {
            active->owner      = nullptr;
            active->completion = {};
            return ExecutorResult<>::failure(executor_error(jb::core::ErrorCategory::Unavailable,
                                                            "jobu.http.start_failed",
                                                            "The HTTP client rejected the attempt start",
                                                            started.error().code));
        }

        active->request_id = *started;
        request_by_attempt.emplace(active->key, active->request_id);
        attempt_by_request.emplace(active->request_id, std::move(active));
        return ExecutorResult<>::success();
    }

    void complete(std::shared_ptr<ActiveAttempt> const& active,
                  jb::net::HttpRequestId                request_id,
                  jb::net::HttpCompletionResult         transfer)
    {
        auto const found = attempt_by_request.find(active->request_id);
        if (active->owner != this || found == attempt_by_request.end() || found->second != active) {
            return;
        }

        auto mapped = request_id == active->request_id
                        ? detail::map_http_completion(transfer,
                                                      active->expected_statuses,
                                                      active->attributes,
                                                      time_source.utc_now())
                        : ExecutorResult<detail::HttpCompletionPolicy>::failure(
                              executor_error(jb::core::ErrorCategory::Internal,
                                             "jobu.http.callback_identity_mismatch",
                                             "The HTTP client completion identity did not match the accepted request"));
        auto policy = mapped ? std::move(mapped).value() : internal_completion_policy();

        auto completion = AttemptCompletion{
            .key                 = active->key,
            .outcome             = policy.outcome,
            .failure_disposition = policy.failure_disposition,
            .retry_not_before    = policy.retry_not_before,
            .result              = std::move(policy.result),
        };
        if (should_capture(active->capture, completion.outcome)) {
            completion.output = captured_output(transfer);
        }

        // Retire every executor-owned reference before user code can re-enter start() or cancel().
        auto handler  = std::move(active->completion);
        active->owner = nullptr;
        request_by_attempt.erase(active->key);
        attempt_by_request.erase(found);
        handler(std::move(completion));
    }

    [[nodiscard]] auto cancel(AttemptKey const& key) -> ExecutorResult<>
    {
        auto const found = request_by_attempt.find(key);
        if (found == request_by_attempt.end()) {
            return ExecutorResult<>::failure(executor_error(jb::core::ErrorCategory::NotFound,
                                                            "jobu.http.attempt_not_found",
                                                            "The HTTP attempt is not active"));
        }
        auto cancelled = client.cancel(found->second);
        if (!cancelled) {
            return ExecutorResult<>::failure(executor_error(jb::core::ErrorCategory::Unavailable,
                                                            "jobu.http.cancel_failed",
                                                            "The HTTP client rejected attempt cancellation",
                                                            cancelled.error().code));
        }
        return ExecutorResult<>::success();
    }

    void shutdown()
    {
        // Disable the callback gate before cancellation because accepted cancellation still owes a later client
        // callback.
        for (auto& [request_id, active] : attempt_by_request) {
            active->owner      = nullptr;
            active->completion = {};
            auto cancelled     = client.cancel(request_id);
            (void)cancelled;
        }
        request_by_attempt.clear();
        attempt_by_request.clear();
    }
};

HttpAttemptExecutor::HttpAttemptExecutor(jb::net::HttpClient& client, jb::core::TimeSource& time_source)
    : _data{std::make_unique<Private>(client, time_source)}
{}

HttpAttemptExecutor::~HttpAttemptExecutor()
{
    _data->shutdown();
}

auto HttpAttemptExecutor::is_available(JobType type) const noexcept -> bool
{
    return type == JobType::Http && _data->client.is_available();
}

auto HttpAttemptExecutor::start(AttemptStartRequest request, AttemptCompletionHandler completion) -> ExecutorResult<>
{
    return _data->start(std::move(request), std::move(completion));
}

auto HttpAttemptExecutor::cancel(AttemptKey const& key) -> ExecutorResult<>
{
    return _data->cancel(key);
}

} // namespace jb::jobu::http
