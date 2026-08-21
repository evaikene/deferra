#include "retry_policy_priv.hpp"

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace jb::jobu::detail {

namespace {

template <typename T>
using RetryResult = jb::core::Result<T, jb::core::Error>;

auto invalid_policy(std::string detail) -> jb::core::Error
{
    return {
        .category = jb::core::ErrorCategory::Internal,
        .code     = "jobu.retry.invalid_policy",
        .message  = "Materialized retry attributes are invalid",
        .detail   = std::move(detail),
    };
}

auto retry_time_out_of_range() -> jb::core::Error
{
    return {
        .category = jb::core::ErrorCategory::ResourceExhausted,
        .code     = "jobu.retry.out_of_range",
        .message  = "Retry due time is outside the representable range",
    };
}

template <typename T>
auto policy_attribute(AttributeSet const& attributes, std::string_view name) -> RetryResult<T const*>
{
    auto const entry = attributes.find(name);
    if (entry == attributes.end()) {
        return RetryResult<T const*>::failure(invalid_policy("Required retry field is missing: " + std::string{name}));
    }
    auto const* value = std::get_if<T>(&entry->second.data);
    if (value == nullptr) {
        return RetryResult<T const*>::failure(
            invalid_policy("Retry field has an unexpected type: " + std::string{name}));
    }
    return RetryResult<T const*>::success(value);
}

auto validate_policy(RetryPolicy const& policy) -> RetryResult<void>
{
    using namespace std::chrono;

    auto const maximum_initial_delay = duration_cast<jb::core::Duration>(hours{24});
    auto const maximum_delay         = duration_cast<jb::core::Duration>(days{30});
    if (policy.max_attempts < 1 || policy.max_attempts > 100) {
        return RetryResult<void>::failure(invalid_policy("retry.max_attempts is outside its accepted range"));
    }
    if (policy.strategy != RetryStrategy::Fixed && policy.strategy != RetryStrategy::Exponential) {
        return RetryResult<void>::failure(invalid_policy("retry.strategy is not recognized"));
    }
    if (policy.initial_delay < jb::core::Duration::zero() || policy.initial_delay > maximum_initial_delay) {
        return RetryResult<void>::failure(invalid_policy("retry.initial_delay is outside its accepted range"));
    }
    if (policy.max_delay < jb::core::Duration::zero() || policy.max_delay > maximum_delay) {
        return RetryResult<void>::failure(invalid_policy("retry.max_delay is outside its accepted range"));
    }
    if (policy.max_delay < policy.initial_delay) {
        return RetryResult<void>::failure(invalid_policy("retry.max_delay is below retry.initial_delay"));
    }
    if (!std::isfinite(policy.multiplier) || policy.multiplier < 1.0 || policy.multiplier > 100.0) {
        return RetryResult<void>::failure(invalid_policy("retry.multiplier is outside its accepted range"));
    }
    if (!std::isfinite(policy.jitter) || policy.jitter < 0.0 || policy.jitter > 1.0) {
        return RetryResult<void>::failure(invalid_policy("retry.jitter is outside its accepted range"));
    }
    if (policy.mode != RetryMode::Blocking && policy.mode != RetryMode::Reschedule) {
        return RetryResult<void>::failure(invalid_policy("retry.mode is not recognized"));
    }
    return RetryResult<void>::success();
}

auto capped_product(double value, double factor, double limit) -> double
{
    if (value == 0.0 || factor <= 0.0 || limit == 0.0) {
        return 0.0;
    }
    if (!std::isfinite(factor) || factor >= limit / value) {
        return limit;
    }
    return value * factor;
}

auto stable_fraction(jb::core::Uuid const& run_id, AttemptNumber attempt) noexcept -> double
{
    // FNV-1a absorbs UUID bytes and the attempt in a fixed big-endian order.
    // A SplitMix64 finalizer then avalanches the state before the top 53 bits
    // are mapped exactly into [0, 1). These fixed-width steps are independent
    // of std::hash, process state, byte order, and word size.
    constexpr std::uint64_t offset_basis{14695981039346656037ULL};
    constexpr std::uint64_t prime{1099511628211ULL};
    auto                    state  = offset_basis;
    auto                    absorb = [&state](std::uint8_t byte) {
        state ^= byte;
        state *= prime;
    };

    for (auto const byte : run_id.bytes()) {
        absorb(std::to_integer<std::uint8_t>(byte));
    }
    for (unsigned int index = 0; index < sizeof(AttemptNumber); ++index) {
        auto const shift = 56U - (index * 8U);
        absorb(static_cast<std::uint8_t>((attempt >> shift) & 0xffU));
    }

    state ^= state >> 30U;
    state *= 0xbf58476d1ce4e5b9ULL;
    state ^= state >> 27U;
    state *= 0x94d049bb133111ebULL;
    state ^= state >> 31U;

    return static_cast<double>(state >> 11U) * 0x1.0p-53;
}

auto checked_retry_due_time(jb::core::UtcTimePoint completed_at, jb::core::Duration delay)
    -> RetryResult<jb::core::UtcTimePoint>
{
    using UtcDuration = jb::core::UtcTimePoint::duration;

    auto const converted = std::chrono::duration_cast<UtcDuration>(delay);
    if (converted < UtcDuration::zero() || std::chrono::duration_cast<jb::core::Duration>(converted) != delay ||
        completed_at > jb::core::UtcTimePoint::max() - converted) {
        return RetryResult<jb::core::UtcTimePoint>::failure(retry_time_out_of_range());
    }
    return RetryResult<jb::core::UtcTimePoint>::success(completed_at + converted);
}

} // anonymous namespace

auto retry_policy_from_attributes(AttributeSet const& attributes) -> RetryResult<RetryPolicy>
{
    auto max_attempts = policy_attribute<std::int64_t>(attributes, "retry.max_attempts");
    if (!max_attempts) {
        return RetryResult<RetryPolicy>::failure(std::move(max_attempts).error());
    }
    auto strategy = policy_attribute<std::string>(attributes, "retry.strategy");
    if (!strategy) {
        return RetryResult<RetryPolicy>::failure(std::move(strategy).error());
    }
    auto initial_delay = policy_attribute<jb::core::Duration>(attributes, "retry.initial_delay");
    if (!initial_delay) {
        return RetryResult<RetryPolicy>::failure(std::move(initial_delay).error());
    }
    auto max_delay = policy_attribute<jb::core::Duration>(attributes, "retry.max_delay");
    if (!max_delay) {
        return RetryResult<RetryPolicy>::failure(std::move(max_delay).error());
    }
    auto multiplier = policy_attribute<double>(attributes, "retry.multiplier");
    if (!multiplier) {
        return RetryResult<RetryPolicy>::failure(std::move(multiplier).error());
    }
    auto jitter = policy_attribute<double>(attributes, "retry.jitter");
    if (!jitter) {
        return RetryResult<RetryPolicy>::failure(std::move(jitter).error());
    }
    auto mode = policy_attribute<std::string>(attributes, "retry.mode");
    if (!mode) {
        return RetryResult<RetryPolicy>::failure(std::move(mode).error());
    }

    if (**max_attempts < 1 || **max_attempts > 100) {
        return RetryResult<RetryPolicy>::failure(invalid_policy("retry.max_attempts is outside its accepted range"));
    }

    auto policy         = RetryPolicy{};
    policy.max_attempts = static_cast<std::uint64_t>(**max_attempts);
    if (**strategy == "fixed") {
        policy.strategy = RetryStrategy::Fixed;
    }
    else if (**strategy == "exponential") {
        policy.strategy = RetryStrategy::Exponential;
    }
    else {
        return RetryResult<RetryPolicy>::failure(invalid_policy("retry.strategy is not recognized"));
    }
    policy.initial_delay = **initial_delay;
    policy.max_delay     = **max_delay;
    policy.multiplier    = **multiplier;
    policy.jitter        = **jitter;
    if (**mode == "blocking") {
        policy.mode = RetryMode::Blocking;
    }
    else if (**mode == "reschedule") {
        policy.mode = RetryMode::Reschedule;
    }
    else {
        return RetryResult<RetryPolicy>::failure(invalid_policy("retry.mode is not recognized"));
    }

    auto validated = validate_policy(policy);
    if (!validated) {
        return RetryResult<RetryPolicy>::failure(std::move(validated).error());
    }
    return RetryResult<RetryPolicy>::success(policy);
}

auto retry_delay(RetryPolicy const& policy, jb::core::Uuid const& run_id, AttemptNumber next_attempt)
    -> RetryResult<jb::core::Duration>
{
    auto validated = validate_policy(policy);
    if (!validated) {
        return RetryResult<jb::core::Duration>::failure(std::move(validated).error());
    }
    if (next_attempt < 2 || next_attempt > policy.max_attempts) {
        return RetryResult<jb::core::Duration>::failure(
            invalid_policy("Next retry attempt is outside the policy boundary"));
    }
    if (policy.initial_delay == jb::core::Duration::zero()) {
        return RetryResult<jb::core::Duration>::success(jb::core::Duration::zero());
    }

    // The accepted 30-day nanosecond limit is below 2^53, so every valid
    // duration count converts to double exactly. Keep the exponential value
    // in that representation and narrow only once after applying jitter.
    auto const limit_count = static_cast<double>(policy.max_delay.count());
    auto       base_count  = static_cast<double>(policy.initial_delay.count());
    if (policy.strategy == RetryStrategy::Exponential) {
        for (AttemptNumber attempt = 2; attempt < next_attempt && base_count < limit_count; ++attempt) {
            base_count = capped_product(base_count, policy.multiplier, limit_count);
        }
    }

    auto const fraction = stable_fraction(run_id, next_attempt);
    auto const factor   = 1.0 - policy.jitter + (2.0 * policy.jitter * fraction);
    auto const count    = capped_product(base_count, factor, limit_count);
    return RetryResult<jb::core::Duration>::success(jb::core::Duration{static_cast<jb::core::Duration::rep>(count)});
}

auto retry_decision(AttributeSet const& attributes, RetryCompletion const& completion) -> RetryResult<RetryDecision>
{
    auto policy = retry_policy_from_attributes(attributes);
    if (!policy) {
        return RetryResult<RetryDecision>::failure(std::move(policy).error());
    }
    if (completion.attempt_number == 0) {
        return RetryResult<RetryDecision>::failure(invalid_policy("Current attempt number must be positive"));
    }
    if (completion.outcome != AttemptOutcome::Failed || !completion.retryable || !completion.retry_allowed ||
        completion.attempt_number >= policy->max_attempts) {
        return RetryResult<RetryDecision>::success({});
    }

    auto const next_attempt = completion.attempt_number + 1U;
    auto       delay        = retry_delay(*policy, completion.run_id, next_attempt);
    if (!delay) {
        return RetryResult<RetryDecision>::failure(std::move(delay).error());
    }
    auto due_at = checked_retry_due_time(completion.completed_at, *delay);
    if (!due_at) {
        return RetryResult<RetryDecision>::failure(std::move(due_at).error());
    }
    if (completion.retry_not_before && *due_at < *completion.retry_not_before) {
        *due_at = *completion.retry_not_before;
    }

    auto schedule = RetrySchedule{
        .due_at = *due_at,
        .mode   = policy->mode,
    };
    return RetryResult<RetryDecision>::success({.retry = schedule});
}

} // namespace jb::jobu::detail
