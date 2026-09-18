#include "cli_exit_policy_priv.hpp"

#include "json.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace jb::jobu::detail {

namespace {

constexpr std::size_t   kMaximumRetryExitCodeSelectors{64};
constexpr std::uint16_t kMaximumExitCode{255};
constexpr std::size_t   kMaximumCompletionResultBytes{std::size_t{256} * 1024U};

template <typename T>
using PolicyResult = jb::core::Result<T, jb::core::Error>;

auto invalid_result(std::string detail) -> jb::core::Error
{
    return {
        .category = jb::core::ErrorCategory::Internal,
        .code     = "jobu.cli.invalid_result",
        .message  = "CLI completion result could not be represented safely",
        .detail   = std::move(detail),
    };
}

auto json_string(std::string_view value) -> jb::core::JsonValue
{
    return jb::core::JsonValue{.data = std::string{value}};
}

auto capture_result(AttemptOutputChannel const& capture, bool discard) -> jb::core::JsonValue
{
    auto const captured_bytes = discard ? std::uint64_t{0} : static_cast<std::uint64_t>(capture.bytes.size());
    auto const truncated      = capture.total_bytes > captured_bytes;
    return jb::core::JsonValue{
        .data = jb::core::JsonValue::Object{
                                            {"captured_bytes", {.data = captured_bytes}},
                                            {"total_bytes", {.data = capture.total_bytes}},
                                            {"truncated", {.data = truncated}},
                                            }
    };
}

auto finish_result(jb::core::JsonValue::Object object) -> PolicyResult<jb::core::JsonValue>
{
    auto result     = jb::core::JsonValue{.data = std::move(object)};
    auto serialized = jb::core::serialize_json(result);
    if (!serialized) {
        return PolicyResult<jb::core::JsonValue>::failure(invalid_result("serialization_failed"));
    }
    if (serialized->size() > kMaximumCompletionResultBytes) {
        return PolicyResult<jb::core::JsonValue>::failure(invalid_result("serialized_size_exceeded"));
    }
    return PolicyResult<jb::core::JsonValue>::success(std::move(result));
}

auto valid_exit_code(std::optional<int> const& value) noexcept -> bool
{
    return value && *value >= 0 && *value <= kMaximumExitCode;
}

auto valid_signal(std::optional<int> const& value) noexcept -> bool
{
    return value && *value > 0;
}

auto valid_start_error(std::optional<jb::core::Error> const& error) noexcept -> bool
{
    return error && std::string_view{error->code}.starts_with("core.process.");
}

auto validate_process_exit(jb::core::ProcessExit const& process_exit) -> PolicyResult<void>
{
    auto const has_exit_code = process_exit.exit_code.has_value();
    auto const has_signal    = process_exit.signal_number.has_value();
    auto const has_error     = process_exit.start_error.has_value();

    switch (process_exit.kind) {
        case jb::core::ProcessExitKind::Exited:
            if (!valid_exit_code(process_exit.exit_code) || has_signal || has_error) {
                return PolicyResult<void>::failure(invalid_result("invalid_exit_observation"));
            }
            break;
        case jb::core::ProcessExitKind::Signaled:
            if (has_exit_code || !valid_signal(process_exit.signal_number) || has_error) {
                return PolicyResult<void>::failure(invalid_result("invalid_signal_observation"));
            }
            break;
        case jb::core::ProcessExitKind::TimedOut:
        case jb::core::ProcessExitKind::Cancelled:
            if (has_exit_code == has_signal || has_error ||
                (has_exit_code && !valid_exit_code(process_exit.exit_code)) ||
                (has_signal && !valid_signal(process_exit.signal_number))) {
                return PolicyResult<void>::failure(invalid_result("invalid_stop_observation"));
            }
            break;
        case jb::core::ProcessExitKind::StartFailed:
            if (has_exit_code || has_signal || !valid_start_error(process_exit.start_error)) {
                return PolicyResult<void>::failure(invalid_result("invalid_start_failure"));
            }
            break;
        case jb::core::ProcessExitKind::Interrupted:
            return PolicyResult<void>::failure(invalid_result("interrupted_reserved"));
    }
    return PolicyResult<void>::success();
}

void add_observed_terminal(jb::core::JsonValue::Object& result, jb::core::ProcessExit const& process_exit)
{
    if (process_exit.exit_code) {
        result.emplace("exit_code", jb::core::JsonValue{.data = static_cast<std::uint64_t>(*process_exit.exit_code)});
    }
    else if (process_exit.signal_number) {
        result.emplace("signal", jb::core::JsonValue{.data = static_cast<std::uint64_t>(*process_exit.signal_number)});
    }
}

auto parse_exit_code(std::string_view text) noexcept -> std::optional<std::uint16_t>
{
    if (text.empty() || (text.size() > 1U && text.front() == '0')) {
        return std::nullopt;
    }

    auto value = std::uint16_t{0};
    for (char const character : text) {
        if (character < '0' || character > '9') {
            return std::nullopt;
        }

        value = static_cast<std::uint16_t>((value * 10U) + static_cast<unsigned int>(character - '0'));
        if (value > kMaximumExitCode) {
            return std::nullopt;
        }
    }
    return value;
}

auto parse_selector(std::string_view selector) noexcept -> std::optional<CliExitCodeRange>
{
    auto const separator = selector.find('-');
    if (separator == std::string_view::npos) {
        auto code = parse_exit_code(selector);
        if (!code) {
            return std::nullopt;
        }
        return CliExitCodeRange{.first = *code, .last = *code};
    }
    if (selector.find('-', separator + 1U) != std::string_view::npos) {
        return std::nullopt;
    }

    auto first = parse_exit_code(selector.substr(0, separator));
    auto last  = parse_exit_code(selector.substr(separator + 1U));
    if (!first || !last || *first >= *last) {
        return std::nullopt;
    }
    return CliExitCodeRange{.first = *first, .last = *last};
}

} // anonymous namespace

CliExitCodeSet::CliExitCodeSet(std::vector<CliExitCodeRange> ranges) noexcept
    : _ranges{std::move(ranges)}
{}

auto CliExitCodeSet::contains(int exit_code) const noexcept -> bool
{
    if (exit_code < 0 || exit_code > kMaximumExitCode) {
        return false;
    }

    auto const code = static_cast<std::uint16_t>(exit_code);
    for (auto const range : _ranges) {
        if (code < range.first) {
            return false;
        }
        if (code <= range.last) {
            return true;
        }
    }
    return false;
}

auto decode_cli_retry_exit_codes(AttributeValue::List const& selectors) -> std::optional<CliExitCodeSet>
{
    if (selectors.size() > kMaximumRetryExitCodeSelectors) {
        return std::nullopt;
    }

    auto ranges = std::vector<CliExitCodeRange>{};
    ranges.reserve(selectors.size());
    for (auto const& value : selectors) {
        auto const* selector = std::get_if<std::string>(&value.data);
        if (selector == nullptr) {
            return std::nullopt;
        }

        auto range = parse_selector(*selector);
        if (!range) {
            return std::nullopt;
        }
        ranges.push_back(*range);
    }

    std::ranges::sort(ranges, {}, &CliExitCodeRange::first);
    auto normalized = std::vector<CliExitCodeRange>{};
    normalized.reserve(ranges.size());
    for (auto const range : ranges) {
        if (normalized.empty()) {
            normalized.push_back(range);
            continue;
        }

        auto& previous = normalized.back();

        // Reject aliases before merging adjacency so duplicate or overlapping
        // policy cannot disappear inside the normalized membership set.
        if (range.first <= previous.last) {
            return std::nullopt;
        }
        if (range.first == previous.last + 1U) {
            previous.last = range.last;
            continue;
        }
        normalized.push_back(range);
    }
    return CliExitCodeSet{std::move(normalized)};
}

auto map_cli_completion(jb::core::ProcessExit const& process_exit,
                        CliExpectedExitCodes const&  expected_exit_codes,
                        CliExitCodeSet const&        retry_exit_codes,
                        CliCaptureMode               capture_mode,
                        CliCaptureSnapshot           capture) -> PolicyResult<CliCompletionPolicy>
{
    auto valid = validate_process_exit(process_exit);
    if (!valid) {
        return PolicyResult<CliCompletionPolicy>::failure(std::move(valid).error());
    }
    if (capture_mode != CliCaptureMode::None && capture_mode != CliCaptureMode::OnError &&
        capture_mode != CliCaptureMode::Always) {
        return PolicyResult<CliCompletionPolicy>::failure(invalid_result("invalid_capture_mode"));
    }

    auto const capture_lost    = capture.capture_lost || process_exit.stdout_lost || process_exit.stderr_lost;
    auto const discard_capture = capture_mode == CliCaptureMode::None;
    auto       result          = jb::core::JsonValue::Object{
        {"capture_lost", {.data = capture_lost}},
        {"stderr", capture_result(capture.standard_error, discard_capture)},
        {"stdout", capture_result(capture.standard_output, discard_capture)},
        {"type", json_string("cli")},
    };
    auto policy = CliCompletionPolicy{};

    switch (process_exit.kind) {
        case jb::core::ProcessExitKind::Exited: {
            auto const exit_code = *process_exit.exit_code;
            auto const expected  = expected_exit_codes.test(static_cast<std::size_t>(exit_code));
            auto const retryable = !expected && retry_exit_codes.contains(exit_code);

            policy.outcome = expected ? AttemptOutcome::Succeeded : AttemptOutcome::Failed;
            if (!expected) {
                policy.failure_disposition = retryable ? FailureDisposition::Retryable : FailureDisposition::Terminal;
            }
            result.emplace("exit_code", jb::core::JsonValue{.data = static_cast<std::uint64_t>(exit_code)});
            result.emplace("outcome", json_string(expected ? "success" : "unexpected_exit"));
            break;
        }
        case jb::core::ProcessExitKind::Signaled:
            policy.outcome             = AttemptOutcome::Failed;
            policy.failure_disposition = FailureDisposition::Retryable;
            result.emplace("outcome", json_string("signal"));
            add_observed_terminal(result, process_exit);
            break;
        case jb::core::ProcessExitKind::TimedOut:
            policy.outcome             = AttemptOutcome::Failed;
            policy.failure_disposition = FailureDisposition::Retryable;
            result.emplace("outcome", json_string("timeout"));
            add_observed_terminal(result, process_exit);
            break;
        case jb::core::ProcessExitKind::Cancelled:
            policy.outcome = AttemptOutcome::Cancelled;
            result.emplace("outcome", json_string("cancelled"));
            add_observed_terminal(result, process_exit);
            break;
        case jb::core::ProcessExitKind::StartFailed:
            policy.outcome             = AttemptOutcome::Failed;
            policy.failure_disposition = FailureDisposition::Terminal;
            result.emplace("error_code", json_string(process_exit.start_error->code));
            result.emplace("outcome", json_string("start_failure"));
            break;
        case jb::core::ProcessExitKind::Interrupted:
            return PolicyResult<CliCompletionPolicy>::failure(invalid_result("interrupted_reserved"));
    }

    auto safe_result = finish_result(std::move(result));
    if (!safe_result) {
        return PolicyResult<CliCompletionPolicy>::failure(std::move(safe_result).error());
    }
    policy.result = std::move(safe_result).value();

    // Result metadata is fixed before moving captured bytes. This preserves observation and loss evidence even when
    // capture policy deliberately omits the durable output row.
    auto const persist_output = capture_mode == CliCaptureMode::Always || (capture_mode == CliCaptureMode::OnError &&
                                                                           policy.outcome != AttemptOutcome::Succeeded);
    if (persist_output) {
        policy.output = AttemptOutput{
            .primary      = std::move(capture.standard_output),
            .diagnostic   = std::move(capture.standard_error),
            .capture_lost = capture_lost,
        };
    }
    return PolicyResult<CliCompletionPolicy>::success(std::move(policy));
}

} // namespace jb::jobu::detail
