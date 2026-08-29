#include "fake_http_client.hpp"

#include "http_validation_priv.hpp"

#include <algorithm>
#include <limits>
#include <string>
#include <utility>

namespace jb::test {

namespace {

template <typename T = void>
using FakeResult = core::Result<T, core::Error>;

auto fake_error(core::ErrorCategory category, std::string code, std::string message) -> core::Error
{
    return {
        .category = category,
        .code     = std::move(code),
        .message  = std::move(message),
    };
}

auto inactive_request(net::HttpRequestId request_id, std::vector<net::HttpRequestId> const& completed) -> core::Error
{
    auto const duplicate = std::ranges::find(completed, request_id) != completed.end();
    return fake_error(duplicate ? core::ErrorCategory::Conflict : core::ErrorCategory::NotFound,
                      duplicate ? "test.http.duplicate_completion" : "test.http.unknown_request",
                      duplicate ? "The fake HTTP request has already completed"
                                : "The fake HTTP request is not active");
}

auto cancellation_pending() -> core::Error
{
    return fake_error(core::ErrorCategory::Conflict,
                      "test.http.cancellation_pending",
                      "The fake HTTP request requires a cancelled completion");
}

auto cancelled_completion(net::HttpError partial = {}) -> net::HttpCompletionResult
{
    partial.kind  = net::HttpErrorKind::Cancelled;
    partial.error = {
        .category = core::ErrorCategory::Cancelled,
        .code     = "net.http.cancelled",
        .message  = "HTTP request was cancelled",
    };
    return net::HttpCompletionResult::failure(std::move(partial));
}

} // anonymous namespace

void FakeHttpClient::set_available(bool available) noexcept
{
    if (!_failure) {
        _available = available;
    }
}

void FakeHttpClient::set_start_error(std::optional<core::Error> error)
{
    _start_error = std::move(error);
}

void FakeHttpClient::set_cancel_error(std::optional<core::Error> error)
{
    _cancel_error = std::move(error);
}

auto FakeHttpClient::start_records() const noexcept -> std::vector<StartRecord> const&
{
    return _start_records;
}

auto FakeHttpClient::cancel_calls() const noexcept -> std::vector<net::HttpRequestId> const&
{
    return _cancel_calls;
}

auto FakeHttpClient::pending_request_ids() const -> std::vector<net::HttpRequestId>
{
    auto result = std::vector<net::HttpRequestId>{};
    result.reserve(_pending.size());
    for (auto const& pending : _pending) {
        result.push_back(pending.id);
    }
    return result;
}

auto FakeHttpClient::complete_success(net::HttpRequestId                request_id,
                                      net::HttpResponse                 response,
                                      std::optional<net::HttpRequestId> reported_id) -> FakeResult<>
{
    return complete(request_id, net::HttpCompletionResult::success(std::move(response)), reported_id);
}

auto FakeHttpClient::complete_error(net::HttpRequestId                request_id,
                                    net::HttpError                    error,
                                    std::optional<net::HttpRequestId> reported_id) -> FakeResult<>
{
    return complete(request_id, net::HttpCompletionResult::failure(std::move(error)), reported_id);
}

auto FakeHttpClient::complete_cancelled(net::HttpRequestId request_id, net::HttpError partial) -> FakeResult<>
{
    auto const pending =
        std::ranges::find_if(_pending, [request_id](PendingRequest const& entry) { return entry.id == request_id; });
    if (pending == _pending.end()) {
        return FakeResult<>::failure(inactive_request(request_id, _completed_ids));
    }
    if (!pending->cancellation_requested) {
        return FakeResult<>::failure(fake_error(core::ErrorCategory::Conflict,
                                                "test.http.cancellation_not_requested",
                                                "The fake HTTP request is not cancellation-pending"));
    }
    return complete(request_id, cancelled_completion(std::move(partial)), std::nullopt);
}

auto FakeHttpClient::inject_shared_failure(core::Error failure_value) -> FakeResult<>
{
    if (_failure) {
        return FakeResult<>::failure(fake_error(core::ErrorCategory::Conflict,
                                                "test.http.shared_failure_already_set",
                                                "The fake HTTP client has already failed"));
    }

    _available   = false;
    _failure     = std::move(failure_value);
    auto pending = std::exchange(_pending, {});
    for (auto& entry : pending) {
        _completed_ids.push_back(entry.id);
        auto completion = entry.cancellation_requested ? cancelled_completion()
                                                       : net::HttpCompletionResult::failure({
                                                             .kind  = net::HttpErrorKind::Internal,
                                                             .error = *_failure,
                                                         });
        entry.completion(entry.id, std::move(completion));
    }
    emit(failed, *_failure);
    return FakeResult<>::success();
}

auto FakeHttpClient::is_available() const noexcept -> bool
{
    return _available && !_failure;
}

auto FakeHttpClient::start(net::HttpRequest request, net::HttpCompletionHandler completion)
    -> FakeResult<net::HttpRequestId>
{
    auto validation = net::detail::validate_http_request(request);
    if (!validation) {
        return FakeResult<net::HttpRequestId>::failure(std::move(validation).error());
    }
    if (_failure || !_available) {
        return FakeResult<net::HttpRequestId>::failure(
            _failure.value_or(fake_error(core::ErrorCategory::Unavailable,
                                         "net.http.unavailable",
                                         "The fake HTTP client is unavailable")));
    }
    if (_start_error) {
        return FakeResult<net::HttpRequestId>::failure(*_start_error);
    }
    if (!completion) {
        return FakeResult<net::HttpRequestId>::failure(
            fake_error(core::ErrorCategory::InvalidArgument,
                       "net.http.invalid_request",
                       "The fake HTTP client requires a completion handler"));
    }
    if (_next_request_id == 0) {
        return FakeResult<net::HttpRequestId>::failure(fake_error(core::ErrorCategory::ResourceExhausted,
                                                                  "net.http.identifier_exhausted",
                                                                  "The fake HTTP request identifier is exhausted"));
    }

    auto const request_id = _next_request_id;
    _next_request_id      = request_id == std::numeric_limits<net::HttpRequestId>::max() ? 0 : request_id + 1U;
    _start_records.push_back({.id = request_id, .request = std::move(request)});
    _pending.push_back({.id = request_id, .completion = std::move(completion)});
    return FakeResult<net::HttpRequestId>::success(request_id);
}

auto FakeHttpClient::cancel(net::HttpRequestId request_id) -> FakeResult<>
{
    _cancel_calls.push_back(request_id);
    if (_cancel_error) {
        return FakeResult<>::failure(*_cancel_error);
    }
    auto const pending =
        std::ranges::find_if(_pending, [request_id](PendingRequest const& entry) { return entry.id == request_id; });
    if (pending == _pending.end() || pending->cancellation_requested) {
        return FakeResult<>::failure(fake_error(core::ErrorCategory::NotFound,
                                                "net.http.request_not_found",
                                                "The fake HTTP request is not active"));
    }
    pending->cancellation_requested = true;
    return FakeResult<>::success();
}

auto FakeHttpClient::active_request_count() const noexcept -> std::size_t
{
    return _pending.size();
}

auto FakeHttpClient::failure() const -> std::optional<core::Error>
{
    return _failure;
}

auto FakeHttpClient::complete(net::HttpRequestId                request_id,
                              net::HttpCompletionResult         result,
                              std::optional<net::HttpRequestId> reported_id) -> FakeResult<>
{
    auto const pending =
        std::ranges::find_if(_pending, [request_id](PendingRequest const& entry) { return entry.id == request_id; });
    if (pending == _pending.end()) {
        return FakeResult<>::failure(inactive_request(request_id, _completed_ids));
    }
    if (pending->cancellation_requested && (result || result.error().kind != net::HttpErrorKind::Cancelled)) {
        return FakeResult<>::failure(cancellation_pending());
    }

    auto handler = std::move(pending->completion);
    _pending.erase(pending);
    _completed_ids.push_back(request_id);
    handler(reported_id.value_or(request_id), std::move(result));
    return FakeResult<>::success();
}

} // namespace jb::test
