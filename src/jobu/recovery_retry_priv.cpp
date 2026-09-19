#include "recovery_retry_priv.hpp"

#include <cstdint>
#include <limits>
#include <utility>

namespace jb::jobu::detail {

auto recovery_retry_decision(AttributeSet const& attributes, RecoveryRetryContext const& context)
    -> jb::core::Result<RetryDecision, jb::core::Error>
{
    using DecisionResult = jb::core::Result<RetryDecision, jb::core::Error>;

    // Validate even terminal decisions: corrupt policy or numbering must not be hidden by exhaustion.
    auto policy = retry_policy_from_attributes(attributes);
    if (!policy) {
        return DecisionResult::failure(std::move(policy).error());
    }
    if (context.attempt_number == 0 ||
        (context.policy != RecoveryPolicy::FailInterrupted && context.policy != RecoveryPolicy::RetryInterrupted) ||
        (context.job_state != JobState::Active && context.job_state != JobState::Suspending &&
         context.job_state != JobState::Suspended && context.job_state != JobState::Deleted) ||
        (context.queue_state != QueueState::Active && context.queue_state != QueueState::Suspending &&
         context.queue_state != QueueState::Suspended && context.queue_state != QueueState::Deleted)) {
        return DecisionResult::failure({.category = jb::core::ErrorCategory::Internal,
                                        .code     = "jobu.recovery.invariant",
                                        .message  = "Persisted recovery data violates a JobU invariant",
                                        .detail   = "reason=retry_context"});
    }
    // Storage uses signed 64-bit attempt numbers. Check before addition, even though current
    // supported retry policies have much smaller limits; malformed histories must never wrap.
    if (context.attempt_number >= static_cast<AttemptNumber>(std::numeric_limits<std::int64_t>::max())) {
        return DecisionResult::failure({.category = jb::core::ErrorCategory::ResourceExhausted,
                                        .code     = "jobu.recovery.attempt_number_exhausted",
                                        .message  = "Recovery attempt number is outside the representable range"});
    }

    if (context.policy == RecoveryPolicy::FailInterrupted || context.job_state == JobState::Deleted ||
        context.queue_state == QueueState::Deleted || context.attempt_number >= policy->max_attempts) {
        return DecisionResult::success({});
    }

    // Unknown outcome is not an observed failure. Share only deterministic delay arithmetic,
    // using the next attempt's identity and the invocation's single recovery timestamp.
    auto delay = retry_delay(*policy, context.run_id, context.attempt_number + 1U);
    if (!delay) {
        return DecisionResult::failure(std::move(delay).error());
    }
    auto due = checked_retry_due_time(context.recovery_time, *delay);
    if (!due) {
        return DecisionResult::failure(std::move(due).error());
    }
    return DecisionResult::success({
        .retry = RetrySchedule{.due_at = *due, .mode = policy->mode}
    });
}

} // namespace jb::jobu::detail
