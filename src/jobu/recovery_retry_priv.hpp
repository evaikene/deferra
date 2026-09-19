#pragma once

#include "job.hpp"
#include "queue.hpp"
#include "retry_policy_priv.hpp"

namespace jb::jobu::detail {

/// Current recovery policy/ownership, paired with the interrupted run's immutable retry attributes.
struct RecoveryRetryContext {
    jb::core::Uuid         run_id;
    AttemptNumber          attempt_number{1};
    RecoveryPolicy         policy{RecoveryPolicy::FailInterrupted};
    JobState               job_state{JobState::Active};
    QueueState             queue_state{QueueState::Active};
    jb::core::UtcTimePoint recovery_time;
};

/// Chooses retry eligibility for an unknown outcome, independently of ordinary Failed completion.
/// Suspension preserves the retry; normal dispatch gates determine when it may run. Deleted owners
/// forbid retry, but callers must separately reject persisted ownership shapes that are unsupported.
/// Invalid policy/state, unrepresentable current/next attempt numbers, and due-time overflow fail.
/// No attempt is created and no retry allowance is consumed by this calculation.
[[nodiscard]] auto recovery_retry_decision(AttributeSet const& attributes, RecoveryRetryContext const& context)
    -> jb::core::Result<RetryDecision, jb::core::Error>;

} // namespace jb::jobu::detail
