#pragma once

#include "attempt.hpp"
#include "attribute.hpp"
#include "error.hpp"
#include "result.hpp"
#include "time_source.hpp"
#include "uuid.hpp"

#include <cstdint>
#include <optional>

namespace jb::jobu::detail {

enum class RetryStrategy : std::uint8_t {
    Fixed,
    Exponential,
};

enum class RetryMode : std::uint8_t {
    Blocking,
    Reschedule,
};

struct RetryPolicy {
    std::uint64_t      max_attempts{1};
    RetryStrategy      strategy{RetryStrategy::Fixed};
    jb::core::Duration initial_delay{};
    jb::core::Duration max_delay{};
    double             multiplier{2.0};
    double             jitter{0.0};
    RetryMode          mode{RetryMode::Reschedule};

    auto operator==(RetryPolicy const&) const -> bool = default;
};

struct RetryCompletion {
    jb::core::Uuid                        run_id;
    AttemptNumber                         attempt_number{1};
    AttemptOutcome                        outcome{AttemptOutcome::Failed};
    bool                                  retryable{false};
    bool                                  retry_allowed{true};
    jb::core::UtcTimePoint                completed_at;
    std::optional<jb::core::UtcTimePoint> retry_not_before;
};

struct RetrySchedule {
    jb::core::UtcTimePoint due_at;
    RetryMode              mode{RetryMode::Reschedule};

    auto operator==(RetrySchedule const&) const -> bool = default;
};

struct RetryDecision {
    std::optional<RetrySchedule> retry;

    auto operator==(RetryDecision const&) const -> bool = default;
};

[[nodiscard]] auto retry_policy_from_attributes(AttributeSet const& attributes)
    -> jb::core::Result<RetryPolicy, jb::core::Error>;

[[nodiscard]] auto retry_delay(RetryPolicy const& policy, jb::core::Uuid const& run_id, AttemptNumber next_attempt)
    -> jb::core::Result<jb::core::Duration, jb::core::Error>;

[[nodiscard]] auto retry_decision(AttributeSet const& attributes, RetryCompletion const& completion)
    -> jb::core::Result<RetryDecision, jb::core::Error>;

} // namespace jb::jobu::detail
