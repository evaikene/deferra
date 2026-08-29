#include "http_retry_priv.hpp"

#include "retry_policy_priv.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace jb::jobu::detail {

namespace {

template <typename T>
using PolicyResult = jb::core::Result<T, jb::core::Error>;

constexpr std::size_t kMaximumCompletionResultBytes{std::size_t{256} * 1024U};
constexpr std::size_t kMaximumRetryErrors{16U};
constexpr std::size_t kMaximumRetryStatuses{64U};

struct HttpRetryPolicy {
    std::vector<std::string_view> error_categories;
    std::vector<HttpStatusRange>  status_ranges;
    jb::core::Duration            max_delay{};
};

struct RetryAfterPolicy {
    std::optional<jb::core::UtcTimePoint> retry_not_before;
    bool                                  accepted{false};
    bool                                  clamped{false};
};

struct AddedTime {
    jb::core::UtcTimePoint value;
    bool                   saturated{false};
};

auto invalid_policy(std::string detail) -> jb::core::Error
{
    return {
        .category = jb::core::ErrorCategory::Internal,
        .code     = "jobu.http.invalid_retry_policy",
        .message  = "Materialized HTTP retry attributes are invalid",
        .detail   = std::move(detail),
    };
}

auto invalid_result(std::string detail) -> jb::core::Error
{
    return {
        .category = jb::core::ErrorCategory::Internal,
        .code     = "jobu.http.invalid_result",
        .message  = "HTTP completion result could not be represented safely",
        .detail   = std::move(detail),
    };
}

constexpr auto ascii_lower(unsigned char value) noexcept -> unsigned char
{
    if (value >= static_cast<unsigned char>('A') && value <= static_cast<unsigned char>('Z')) {
        return static_cast<unsigned char>(value + ('a' - 'A'));
    }
    return value;
}

auto ascii_equal(std::string_view lhs, std::string_view rhs) noexcept -> bool
{
    return lhs.size() == rhs.size() && std::ranges::equal(lhs, rhs, [](char left, char right) {
               return ascii_lower(static_cast<unsigned char>(left)) == ascii_lower(static_cast<unsigned char>(right));
           });
}

constexpr auto configurable_error_category(std::string_view category) noexcept -> bool
{
    return category == "resolve" || category == "connect" || category == "tls_handshake" ||
           category == "tls_verification" || category == "timeout" || category == "send" || category == "receive" ||
           category == "redirect" || category == "protocol";
}

auto error_category(jb::net::HttpErrorKind kind) noexcept -> std::string_view
{
    switch (kind) {
        case jb::net::HttpErrorKind::InvalidRequest:
            return "invalid_request";
        case jb::net::HttpErrorKind::Resolve:
            return "resolve";
        case jb::net::HttpErrorKind::Connect:
            return "connect";
        case jb::net::HttpErrorKind::TlsVerification:
            return "tls_verification";
        case jb::net::HttpErrorKind::TlsHandshake:
            return "tls_handshake";
        case jb::net::HttpErrorKind::Timeout:
            return "timeout";
        case jb::net::HttpErrorKind::Send:
            return "send";
        case jb::net::HttpErrorKind::Receive:
            return "receive";
        case jb::net::HttpErrorKind::Redirect:
            return "redirect";
        case jb::net::HttpErrorKind::Protocol:
            return "protocol";
        case jb::net::HttpErrorKind::Cancelled:
            return "cancelled";
        case jb::net::HttpErrorKind::Internal:
            return "internal";
    }
    return "internal";
}

auto attribute_list(AttributeSet const& attributes, std::string_view name) -> PolicyResult<AttributeValue::List const*>
{
    auto const entry = attributes.find(name);
    if (entry == attributes.end()) {
        return PolicyResult<AttributeValue::List const*>::failure(
            invalid_policy("Required HTTP retry field is missing: " + std::string{name}));
    }
    auto const* list = std::get_if<AttributeValue::List>(&entry->second.data);
    if (list == nullptr) {
        return PolicyResult<AttributeValue::List const*>::failure(
            invalid_policy("HTTP retry field has an unexpected type: " + std::string{name}));
    }
    return PolicyResult<AttributeValue::List const*>::success(list);
}

auto decode_error_categories(AttributeValue::List const& values) -> PolicyResult<std::vector<std::string_view>>
{
    if (values.size() > kMaximumRetryErrors) {
        return PolicyResult<std::vector<std::string_view>>::failure(
            invalid_policy("http.retry_errors exceeds its accepted size"));
    }

    auto categories = std::vector<std::string_view>{};
    categories.reserve(values.size());
    for (auto const& value : values) {
        auto const* category = std::get_if<std::string>(&value.data);
        if (category == nullptr || !configurable_error_category(*category) ||
            std::ranges::find(categories, *category) != categories.end()) {
            return PolicyResult<std::vector<std::string_view>>::failure(
                invalid_policy("http.retry_errors contains an invalid category"));
        }
        categories.emplace_back(*category);
    }
    return PolicyResult<std::vector<std::string_view>>::success(std::move(categories));
}

auto decode_status_ranges(AttributeValue::List const& values) -> PolicyResult<std::vector<HttpStatusRange>>
{
    if (values.size() > kMaximumRetryStatuses) {
        return PolicyResult<std::vector<HttpStatusRange>>::failure(
            invalid_policy("http.retry_statuses exceeds its accepted size"));
    }

    auto ranges    = std::vector<HttpStatusRange>{};
    auto selectors = std::vector<std::string_view>{};
    ranges.reserve(values.size());
    selectors.reserve(values.size());
    for (auto const& value : values) {
        auto const* selector = std::get_if<std::string>(&value.data);
        if (selector == nullptr || std::ranges::find(selectors, *selector) != selectors.end()) {
            return PolicyResult<std::vector<HttpStatusRange>>::failure(
                invalid_policy("http.retry_statuses contains an invalid selector"));
        }
        auto parsed = parse_http_status_selector(*selector);
        if (!parsed) {
            return PolicyResult<std::vector<HttpStatusRange>>::failure(
                invalid_policy("http.retry_statuses contains an invalid selector"));
        }
        selectors.emplace_back(*selector);
        ranges.push_back(*parsed);
    }
    return PolicyResult<std::vector<HttpStatusRange>>::success(std::move(ranges));
}

auto decode_http_retry_policy(AttributeSet const& attributes) -> PolicyResult<HttpRetryPolicy>
{
    // Treat the materialized snapshot as persisted input: validate both the
    // generic retry policy and recursive HTTP list alternatives without throwing.
    auto retry = retry_policy_from_attributes(attributes);
    if (!retry) {
        return PolicyResult<HttpRetryPolicy>::failure(std::move(retry).error());
    }
    auto errors = attribute_list(attributes, "http.retry_errors");
    if (!errors) {
        return PolicyResult<HttpRetryPolicy>::failure(std::move(errors).error());
    }
    auto statuses = attribute_list(attributes, "http.retry_statuses");
    if (!statuses) {
        return PolicyResult<HttpRetryPolicy>::failure(std::move(statuses).error());
    }
    auto categories = decode_error_categories(**errors);
    if (!categories) {
        return PolicyResult<HttpRetryPolicy>::failure(std::move(categories).error());
    }
    auto ranges = decode_status_ranges(**statuses);
    if (!ranges) {
        return PolicyResult<HttpRetryPolicy>::failure(std::move(ranges).error());
    }
    return PolicyResult<HttpRetryPolicy>::success({
        .error_categories = std::move(categories).value(),
        .status_ranges    = std::move(ranges).value(),
        .max_delay        = retry->max_delay,
    });
}

auto contains_status(std::vector<HttpStatusRange> const& ranges, std::uint16_t status) noexcept -> bool
{
    return std::ranges::any_of(ranges, [status](HttpStatusRange const& range) {
        return status >= range.first && status <= range.last;
    });
}

auto contains_category(std::vector<std::string_view> const& categories, std::string_view category) noexcept -> bool
{
    return std::ranges::find(categories, category) != categories.end();
}

auto parse_unsigned_decimal(std::string_view value) noexcept -> std::optional<std::uint64_t>
{
    if (value.empty()) {
        return std::nullopt;
    }
    auto result = std::uint64_t{0};
    for (auto const character : value) {
        if (character < '0' || character > '9') {
            return std::nullopt;
        }
        auto const digit = static_cast<std::uint64_t>(character - '0');
        if (result > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U) {
            return std::nullopt;
        }
        result = (result * 10U) + digit;
    }
    return result;
}

auto parse_digits(std::string_view value, std::size_t offset, std::size_t count, unsigned& result) noexcept -> bool
{
    result = 0;
    for (std::size_t index = 0; index < count; ++index) {
        auto const character = value[offset + index];
        if (character < '0' || character > '9') {
            return false;
        }
        result = (result * 10U) + static_cast<unsigned>(character - '0');
    }
    return true;
}

auto parse_imf_fixdate(std::string_view value) noexcept -> std::optional<jb::core::UtcTimePoint>
{
    using namespace std::chrono;

    // Parse the fixed, case-sensitive wire form directly so process locale
    // and timezone state cannot change retry scheduling.
    if (value.size() != 29U || value[3] != ',' || value[4] != ' ' || value[7] != ' ' || value[11] != ' ' ||
        value[16] != ' ' || value[19] != ':' || value[22] != ':' || value[25] != ' ' || value.substr(26) != "GMT") {
        return std::nullopt;
    }

    constexpr std::array<std::string_view, 7> weekdays{"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    constexpr std::array<std::string_view, 12>
        months{"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

    auto const* const weekday_iterator = std::ranges::find(weekdays, value.substr(0, 3U));
    auto const* const month_iterator   = std::ranges::find(months, value.substr(8, 3U));
    if (weekday_iterator == weekdays.end() || month_iterator == months.end()) {
        return std::nullopt;
    }

    unsigned day_value{0};
    unsigned year_value{0};
    unsigned hour_value{0};
    unsigned minute_value{0};
    unsigned second_value{0};
    if (!parse_digits(value, 5, 2, day_value) || !parse_digits(value, 12, 4, year_value) ||
        !parse_digits(value, 17, 2, hour_value) || !parse_digits(value, 20, 2, minute_value) ||
        !parse_digits(value, 23, 2, second_value) || hour_value > 23U || minute_value > 59U || second_value > 60U) {
        return std::nullopt;
    }

    auto const month_value = static_cast<unsigned>(std::distance(months.begin(), month_iterator)) + 1U;
    auto const date        = year_month_day{year{static_cast<int>(year_value)}, month{month_value}, day{day_value}};
    if (!date.ok()) {
        return std::nullopt;
    }
    // A syntactically valid date with a mismatched weekday is not IMF-fixdate.
    auto const parsed_weekday = static_cast<unsigned>(std::distance(weekdays.begin(), weekday_iterator));
    if (weekday{sys_days{date}}.c_encoding() != parsed_weekday) {
        return std::nullopt;
    }

    auto const instant = sys_days{date} + hours{hour_value} + minutes{minute_value} + seconds{second_value};
    auto const count   = instant.time_since_epoch().count();
    auto const minimum = ceil<seconds>(jb::core::UtcTimePoint::min().time_since_epoch()).count();
    auto const maximum = floor<seconds>(jb::core::UtcTimePoint::max().time_since_epoch()).count();
    if (count < minimum || count > maximum) {
        return std::nullopt;
    }
    return jb::core::UtcTimePoint{duration_cast<jb::core::UtcTimePoint::duration>(instant.time_since_epoch())};
}

auto add_time_saturating(jb::core::UtcTimePoint completed_at, jb::core::Duration delay) noexcept
    -> std::optional<AddedTime>
{
    using UtcDuration = jb::core::UtcTimePoint::duration;

    // Retry-After is optional policy input. Reject lossy clock conversions and
    // saturate the upper endpoint instead of turning overflow into scheduler failure.
    auto const converted = std::chrono::duration_cast<UtcDuration>(delay);
    if (converted < UtcDuration::zero() || std::chrono::duration_cast<jb::core::Duration>(converted) != delay) {
        return std::nullopt;
    }
    if (completed_at > jb::core::UtcTimePoint::max() - converted) {
        return AddedTime{.value = jb::core::UtcTimePoint::max(), .saturated = true};
    }
    return AddedTime{.value = completed_at + converted};
}

auto retry_after_header(std::vector<jb::net::HttpHeader> const& headers) noexcept -> std::optional<std::string_view>
{
    auto value = std::optional<std::string_view>{};
    for (auto const& header : headers) {
        if (!ascii_equal(header.name, "Retry-After")) {
            continue;
        }
        if (value) {
            return std::nullopt;
        }
        value = header.value;
    }
    return value;
}

auto retry_after_policy(std::vector<jb::net::HttpHeader> const& headers,
                        jb::core::UtcTimePoint                  completed_at,
                        jb::core::Duration                      max_delay) noexcept -> RetryAfterPolicy
{
    auto const header = retry_after_header(headers);
    if (!header) {
        return {};
    }

    auto const endpoint = add_time_saturating(completed_at, max_delay);
    if (!endpoint) {
        return {};
    }

    if (std::ranges::all_of(*header, [](char value) { return value >= '0' && value <= '9'; })) {
        auto const seconds_value = parse_unsigned_decimal(*header);
        if (!seconds_value) {
            return {};
        }

        // Compare before converting so a valid but enormous decimal can clamp
        // without overflowing chrono's signed duration representation.
        auto const maximum_seconds = std::chrono::duration_cast<std::chrono::seconds>(max_delay).count();
        auto const exceeds_maximum = *seconds_value > static_cast<std::uint64_t>(maximum_seconds);
        auto const effective_delay = exceeds_maximum
                                       ? max_delay
                                       : std::chrono::duration_cast<jb::core::Duration>(
                                             std::chrono::seconds{static_cast<std::int64_t>(*seconds_value)});
        auto const candidate       = add_time_saturating(completed_at, effective_delay);
        if (!candidate) {
            return {};
        }
        return {
            .retry_not_before = candidate->value,
            .accepted         = true,
            .clamped          = exceeds_maximum || candidate->saturated,
        };
    }

    auto const candidate = parse_imf_fixdate(*header);
    if (!candidate || *candidate <= completed_at) {
        return {};
    }
    if (*candidate > endpoint->value) {
        return {
            .retry_not_before = endpoint->value,
            .accepted         = true,
            .clamped          = true,
        };
    }
    return {
        .retry_not_before = candidate,
        .accepted         = true,
        .clamped          = false,
    };
}

auto json_string(std::string_view value) -> jb::core::JsonValue
{
    return jb::core::JsonValue{.data = std::string{value}};
}

auto capture_result(jb::net::HttpCapturedData const& capture) -> jb::core::JsonValue
{
    // Persist only bounded observation counts; retained response bytes stay
    // outside result JSON and are owned by the later attempt-output stage.
    return jb::core::JsonValue{
        .data = jb::core::JsonValue::Object{
                                            {"captured_bytes", {.data = static_cast<std::uint64_t>(capture.bytes.size())}},
                                            {"total_bytes", {.data = capture.total_bytes}},
                                            {"truncated", {.data = capture.truncated}},
                                            }
    };
}

auto retry_after_result(RetryAfterPolicy const& policy) -> jb::core::JsonValue
{
    return jb::core::JsonValue{
        .data = jb::core::JsonValue::Object{
                                            {"accepted", {.data = policy.accepted}},
                                            {"clamped", {.data = policy.clamped}},
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

auto base_result(std::string_view                 outcome,
                 jb::net::HttpCapturedData const& body,
                 jb::net::HttpCapturedData const& headers,
                 std::uint32_t                    redirects,
                 jb::core::Duration               elapsed,
                 std::optional<bool>              tls_verified) -> jb::core::JsonValue::Object
{
    auto object = jb::core::JsonValue::Object{
        {"body",        capture_result(body)                                                            },
        {"duration_ms", {.data = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()}},
        {"headers",     capture_result(headers)                                                         },
        {"outcome",     json_string(outcome)                                                            },
        {"redirects",   {.data = static_cast<std::uint64_t>(redirects)}                                 },
        {"type",        json_string("http")                                                             },
    };
    if (tls_verified) {
        object.emplace("tls_verified", jb::core::JsonValue{.data = *tls_verified});
    }
    return object;
}

auto map_response(jb::net::HttpResponse const& response,
                  HttpStatusSet const&         expected_statuses,
                  HttpRetryPolicy const&       policy,
                  jb::core::UtcTimePoint       completed_at) -> PolicyResult<HttpCompletionPolicy>
{
    auto const expected  = expected_statuses.contains(response.status_code);
    auto const retryable = !expected && contains_status(policy.status_ranges, response.status_code);
    auto       result    = base_result(expected ? "expected_status" : "unexpected_status",
                                       response.body,
                                       response.raw_headers,
                                       response.redirect_count,
                                       response.elapsed,
                                       response.tls_verified);
    result.emplace("status", jb::core::JsonValue{.data = static_cast<std::uint64_t>(response.status_code)});

    auto retry_after = RetryAfterPolicy{};
    if (retryable && (response.status_code == 429U || response.status_code == 503U)) {
        retry_after = retry_after_policy(response.headers, completed_at, policy.max_delay);
        result.emplace("retry_after", retry_after_result(retry_after));
    }

    auto safe_result = finish_result(std::move(result));
    if (!safe_result) {
        return PolicyResult<HttpCompletionPolicy>::failure(std::move(safe_result).error());
    }
    return PolicyResult<HttpCompletionPolicy>::success({
        .outcome = expected ? AttemptOutcome::Succeeded : AttemptOutcome::Failed,
        .failure_disposition =
            expected ? std::nullopt
                     : std::optional{retryable ? FailureDisposition::Retryable : FailureDisposition::Terminal},
        .retry_not_before = retry_after.retry_not_before,
        .result           = std::move(safe_result).value(),
    });
}

auto map_error(jb::net::HttpError const& error, HttpRetryPolicy const& policy) -> PolicyResult<HttpCompletionPolicy>
{
    auto const category  = error_category(error.kind);
    auto const cancelled = error.kind == jb::net::HttpErrorKind::Cancelled;
    auto const retryable =
        configurable_error_category(category) && contains_category(policy.error_categories, category);
    auto result = base_result(cancelled ? "cancelled" : "transport_error",
                              error.body,
                              error.raw_headers,
                              error.redirect_count,
                              error.elapsed,
                              error.tls_verified);
    result.emplace("error_category", json_string(category));
    result.emplace("error_code", json_string(error.error.code));
    if (error.status_code) {
        result.emplace("status", jb::core::JsonValue{.data = static_cast<std::uint64_t>(*error.status_code)});
    }

    auto safe_result = finish_result(std::move(result));
    if (!safe_result) {
        return PolicyResult<HttpCompletionPolicy>::failure(std::move(safe_result).error());
    }
    return PolicyResult<HttpCompletionPolicy>::success({
        .outcome = cancelled ? AttemptOutcome::Cancelled : AttemptOutcome::Failed,
        .failure_disposition =
            cancelled ? std::nullopt
                      : std::optional{retryable ? FailureDisposition::Retryable : FailureDisposition::Terminal},
        .retry_not_before = std::nullopt,
        .result           = std::move(safe_result).value(),
    });
}

} // anonymous namespace

auto map_http_completion(jb::net::HttpCompletionResult const& transfer,
                         HttpStatusSet const&                 expected_statuses,
                         AttributeSet const&                  attributes,
                         jb::core::UtcTimePoint               completed_at) -> PolicyResult<HttpCompletionPolicy>
{
    auto policy = decode_http_retry_policy(attributes);
    if (!policy) {
        return PolicyResult<HttpCompletionPolicy>::failure(std::move(policy).error());
    }
    if (transfer) {
        return map_response(*transfer, expected_statuses, *policy, completed_at);
    }
    return map_error(transfer.error(), *policy);
}

} // namespace jb::jobu::detail
