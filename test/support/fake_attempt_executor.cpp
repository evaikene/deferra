#include "fake_attempt_executor.hpp"

#include "json.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace jb::test {

namespace {

constexpr std::size_t kMaximumResultBytes = 256 * 1024;

auto test_error(core::ErrorCategory category, std::string code, std::string message, std::string detail = {})
    -> core::Error
{
    return {
        .category = category,
        .code     = std::move(code),
        .message  = std::move(message),
        .detail   = std::move(detail),
    };
}

auto invalid_completion(std::string_view reason) -> core::Error
{
    return test_error(core::ErrorCategory::InvalidArgument,
                      "test.executor.invalid_completion",
                      "The fake executor completion violates the attempt contract",
                      std::string{reason});
}

auto validate_completion(jobu::AttemptCompletion const& completion) -> core::Result<void, core::Error>
{
    using Result = core::Result<void, core::Error>;

    switch (completion.outcome) {
        case jobu::AttemptOutcome::Succeeded:
            if (completion.failure_disposition || completion.retry_not_before) {
                return Result::failure(invalid_completion("succeeded_fields"));
            }
            break;
        case jobu::AttemptOutcome::Failed:
            if (!completion.failure_disposition) {
                return Result::failure(invalid_completion("missing_failure_disposition"));
            }
            if (*completion.failure_disposition != jobu::FailureDisposition::Terminal &&
                *completion.failure_disposition != jobu::FailureDisposition::Retryable) {
                return Result::failure(invalid_completion("unknown_failure_disposition"));
            }
            if (completion.retry_not_before && *completion.failure_disposition != jobu::FailureDisposition::Retryable) {
                return Result::failure(invalid_completion("terminal_retry_deadline"));
            }
            break;
        case jobu::AttemptOutcome::Cancelled:
            if (completion.failure_disposition || completion.retry_not_before) {
                return Result::failure(invalid_completion("cancelled_fields"));
            }
            break;
        case jobu::AttemptOutcome::Interrupted:
            return Result::failure(invalid_completion("interrupted_reserved"));
        default:
            return Result::failure(invalid_completion("unknown_outcome"));
    }

    if (!completion.result.is_object()) {
        return Result::failure(invalid_completion("result_not_object"));
    }
    auto serialized = rpc::serialize_json(completion.result);
    if (!serialized) {
        return Result::failure(invalid_completion("result_not_serializable"));
    }
    if (serialized->size() > kMaximumResultBytes) {
        return Result::failure(invalid_completion("result_too_large"));
    }
    return Result::success();
}

} // anonymous namespace

void FakeAttemptExecutor::set_available(jobu::JobType type, bool available) noexcept
{
    switch (type) {
        case jobu::JobType::Cli:
            _cli_available = available;
            break;
        case jobu::JobType::Http:
            _http_available = available;
            break;
    }
}

void FakeAttemptExecutor::set_start_error(std::optional<core::Error> error)
{
    _start_error = std::move(error);
}

void FakeAttemptExecutor::set_cancel_error(std::optional<core::Error> error)
{
    _cancel_error = std::move(error);
}

auto FakeAttemptExecutor::start_requests() const noexcept -> std::vector<jobu::AttemptStartRequest> const&
{
    return _start_requests;
}

auto FakeAttemptExecutor::pending_keys() const -> std::vector<jobu::AttemptKey>
{
    auto keys = std::vector<jobu::AttemptKey>{};
    keys.reserve(_pending.size());
    for (auto const& pending : _pending) {
        keys.push_back(pending.key);
    }
    return keys;
}

auto FakeAttemptExecutor::cancel_calls() const noexcept -> std::vector<jobu::AttemptKey> const&
{
    return _cancel_calls;
}

auto FakeAttemptExecutor::complete(jobu::AttemptKey const& key, jobu::AttemptCompletion completion)
    -> core::Result<void, core::Error>
{
    using Result = core::Result<void, core::Error>;

    auto const pending =
        std::find_if(_pending.begin(), _pending.end(), [&](PendingAttempt const& entry) { return entry.key == key; });
    if (pending == _pending.end()) {
        auto const completed = std::find(_completed_keys.begin(), _completed_keys.end(), key);
        if (completed != _completed_keys.end()) {
            return Result::failure(test_error(core::ErrorCategory::Conflict,
                                              "test.executor.duplicate_completion",
                                              "The selected fake attempt has already completed"));
        }
        return Result::failure(test_error(core::ErrorCategory::NotFound,
                                          "test.executor.unknown_key",
                                          "The selected fake attempt is not pending"));
    }
    if (completion.key != key) {
        return Result::failure(test_error(core::ErrorCategory::InvalidArgument,
                                          "test.executor.completion_key_mismatch",
                                          "The fake completion key does not match the selected attempt"));
    }
    auto validated = validate_completion(completion);
    if (!validated) {
        return Result::failure(std::move(validated).error());
    }

    auto handler = std::move(pending->completion);
    _pending.erase(pending);
    _completed_keys.push_back(key);
    handler(std::move(completion));
    return Result::success();
}

auto FakeAttemptExecutor::is_available(jobu::JobType type) const noexcept -> bool
{
    switch (type) {
        case jobu::JobType::Cli:
            return _cli_available;
        case jobu::JobType::Http:
            return _http_available;
    }
    return false;
}

auto FakeAttemptExecutor::start(jobu::AttemptStartRequest request, jobu::AttemptCompletionHandler completion)
    -> core::Result<void, core::Error>
{
    using Result = core::Result<void, core::Error>;

    auto const key = request.key;
    _start_requests.push_back(std::move(request));
    if (_start_error) {
        return Result::failure(*_start_error);
    }
    if (!completion) {
        return Result::failure(test_error(core::ErrorCategory::InvalidArgument,
                                          "test.executor.empty_completion_handler",
                                          "The fake executor requires a completion handler"));
    }
    if (key.attempt_number == 0) {
        return Result::failure(test_error(core::ErrorCategory::InvalidArgument,
                                          "test.executor.invalid_key",
                                          "The fake executor requires a positive attempt number"));
    }
    auto const is_pending =
        std::any_of(_pending.begin(), _pending.end(), [&](PendingAttempt const& entry) { return entry.key == key; });
    auto const was_completed = std::find(_completed_keys.begin(), _completed_keys.end(), key) != _completed_keys.end();
    if (is_pending || was_completed) {
        return Result::failure(test_error(core::ErrorCategory::Conflict,
                                          "test.executor.duplicate_start",
                                          "The fake executor has already observed this attempt key"));
    }

    _pending.push_back({.key = key, .completion = std::move(completion)});
    return Result::success();
}

auto FakeAttemptExecutor::cancel(jobu::AttemptKey const& key) -> core::Result<void, core::Error>
{
    _cancel_calls.push_back(key);
    if (_cancel_error) {
        return core::Result<void, core::Error>::failure(*_cancel_error);
    }
    return core::Result<void, core::Error>::success();
}

} // namespace jb::test
