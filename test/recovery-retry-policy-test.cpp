#include "recovery_retry_priv.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>

using namespace jb::core;
using namespace jb::jobu;
using namespace jb::jobu::detail;
using namespace std::chrono_literals;

namespace {

auto attributes() -> AttributeSet
{
    return {
        {"retry.max_attempts",  {.data = std::int64_t{5}}          },
        {"retry.strategy",      {.data = std::string{"fixed"}}     },
        {"retry.initial_delay", {.data = Duration{10s}}            },
        {"retry.max_delay",     {.data = Duration{30s}}            },
        {"retry.multiplier",    {.data = 2.0}                      },
        {"retry.jitter",        {.data = 0.0}                      },
        {"retry.mode",          {.data = std::string{"reschedule"}}}
    };
}

auto context() -> RecoveryRetryContext
{
    auto bytes = Uuid::Storage{};
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        bytes[index] = static_cast<std::byte>(index);
    }
    return {.run_id = Uuid{bytes}, .policy = RecoveryPolicy::RetryInterrupted, .recovery_time = UtcTimePoint{100s}};
}

} // namespace

TEST_CASE("Recovery eligibility separates queue policy, owner gates and exhaustion", "[jobu][recovery][retry]")
{
    auto const policy = GENERATE(RecoveryPolicy::FailInterrupted, RecoveryPolicy::RetryInterrupted);
    auto const job    = GENERATE(JobState::Active, JobState::Suspending, JobState::Suspended, JobState::Deleted);
    auto const queue = GENERATE(QueueState::Active, QueueState::Suspending, QueueState::Suspended, QueueState::Deleted);
    auto const attempt     = GENERATE(AttemptNumber{1}, AttemptNumber{4}, AttemptNumber{5}, AttemptNumber{100});
    auto       current     = context();
    current.policy         = policy;
    current.job_state      = job;
    current.queue_state    = queue;
    current.attempt_number = attempt;

    auto result = recovery_retry_decision(attributes(), current);
    REQUIRE(result);
    CHECK(result->retry.has_value() == (policy == RecoveryPolicy::RetryInterrupted && job != JobState::Deleted &&
                                        queue != QueueState::Deleted && attempt < 5));
    if (result->retry) {
        CHECK(result->retry->due_at == UtcTimePoint{110s});
    }
}

TEST_CASE("Recovery shares fixed and capped exponential scheduling with normal failures", "[jobu][recovery][retry]")
{
    auto const strategy              = GENERATE(std::string{"fixed"}, std::string{"exponential"});
    auto const mode                  = GENERATE(std::string{"blocking"}, std::string{"reschedule"});
    auto const attempt               = GENERATE(AttemptNumber{1}, AttemptNumber{2}, AttemptNumber{4});
    auto const time                  = GENERATE(UtcTimePoint{-100s}, UtcTimePoint{100s});
    auto       values                = attributes();
    values.at("retry.strategy").data = strategy;
    values.at("retry.mode").data     = mode;
    auto current                     = context();
    current.attempt_number           = attempt;
    current.recovery_time            = time;

    auto recovered = recovery_retry_decision(values, current);
    REQUIRE(recovered);
    REQUIRE(recovered->retry);
    auto delay = 10s;
    if (strategy == "exponential" && attempt > 1) {
        delay = attempt == 2 ? 20s : 30s;
    }
    CHECK(recovered->retry->due_at == time + delay);
    CHECK(recovered->retry->mode == (mode == "blocking" ? RetryMode::Blocking : RetryMode::Reschedule));

    auto completion = RetryCompletion{.run_id         = current.run_id,
                                      .attempt_number = attempt,
                                      .outcome        = AttemptOutcome::Failed,
                                      .retryable      = true,
                                      .completed_at   = time};
    auto normal     = retry_decision(values, completion);
    REQUIRE(normal);
    CHECK(*normal == *recovered);

    // Recovery does not widen the normal completion contract, including HTTP's separate lower bound.
    completion.retry_not_before = time + 60s;
    normal                      = retry_decision(values, completion);
    REQUIRE(normal);
    REQUIRE(normal->retry);
    CHECK(normal->retry->due_at == time + 60s);
    completion.outcome = AttemptOutcome::Interrupted;
    normal             = retry_decision(values, completion);
    REQUIRE(normal);
    CHECK_FALSE(normal->retry);
}

TEST_CASE("Recovery jitter retains deterministic run and next-attempt identity", "[jobu][recovery][retry]")
{
    auto values                    = attributes();
    values.at("retry.jitter").data = 0.25;
    auto current                   = context();
    current.attempt_number         = GENERATE(AttemptNumber{1}, AttemptNumber{2});

    auto policy = retry_policy_from_attributes(values);
    REQUIRE(policy);
    auto delay = retry_delay(*policy, current.run_id, current.attempt_number + 1U);
    REQUIRE(delay);
    if (current.attempt_number == 1) {
        // Pin the same nanosecond delay vector as the ordinary retry tests, independently of the host clock.
        CHECK(delay->count() == 8'293'882'753);
    }
    else {
        CHECK(delay->count() != 8'293'882'753);
    }
    CHECK(*delay >= 7500ms);
    CHECK(*delay <= 12500ms);

    auto first  = recovery_retry_decision(values, current);
    auto repeat = recovery_retry_decision(values, current);

    // Recovery must reject precision loss on coarser clocks (microseconds on macOS), not round jitter.
    auto const clock_delay = std::chrono::duration_cast<UtcTimePoint::duration>(*delay);
    if (clock_delay != *delay) {
        REQUIRE_FALSE(first);
        REQUIRE_FALSE(repeat);
        CHECK(first.error().code == "jobu.retry.out_of_range");
        CHECK(repeat.error().code == "jobu.retry.out_of_range");
        return;
    }

    REQUIRE(first);
    REQUIRE(repeat);
    REQUIRE(first->retry);
    CHECK(*repeat == *first);
    CHECK(first->retry->due_at == current.recovery_time + clock_delay);
}

TEST_CASE("Recovery rejects invalid contexts and policy even when no retry would be chosen", "[jobu][recovery][retry]")
{
    auto const fail_policy = GENERATE(false, true);
    auto const scenario    = GENERATE(0, 1, 2, 3, 4, 5);
    auto       current     = context();
    current.policy         = fail_policy ? RecoveryPolicy::FailInterrupted : RecoveryPolicy::RetryInterrupted;
    auto values            = attributes();
    switch (scenario) {
        case 0:
            current.attempt_number = 0;
            break;
        case 1:
            current.policy = static_cast<RecoveryPolicy>(255);
            break;
        case 2:
            current.job_state = static_cast<JobState>(255);
            break;
        case 3:
            current.queue_state = static_cast<QueueState>(255);
            break;
        case 4:
            values.erase("retry.max_attempts");
            break;
        case 5:
            values.at("retry.jitter").data = std::numeric_limits<double>::quiet_NaN();
            break;
        default:
            FAIL("Unexpected invalid-context scenario");
    }
    auto result = recovery_retry_decision(values, current);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == (scenario < 4 ? "jobu.recovery.invariant" : "jobu.retry.invalid_policy"));
}

TEST_CASE("Recovery checks storage attempt boundaries before incrementing", "[jobu][recovery][retry]")
{
    auto const maximum     = static_cast<AttemptNumber>(std::numeric_limits<std::int64_t>::max());
    auto       current     = context();
    current.attempt_number = GENERATE_COPY(maximum, maximum + 1U, std::numeric_limits<AttemptNumber>::max());
    auto result            = recovery_retry_decision(attributes(), current);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == "jobu.recovery.attempt_number_exhausted");

    current.attempt_number = maximum - 1U;
    result                 = recovery_retry_decision(attributes(), current);
    REQUIRE(result);
    CHECK_FALSE(result->retry);
}

TEST_CASE("Recovery due times fail on overflow and allow zero delay at the clock boundary", "[jobu][recovery][retry]")
{
    auto current          = context();
    current.recovery_time = UtcTimePoint::max() - 5s;
    auto values           = attributes();
    auto result           = recovery_retry_decision(values, current);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == "jobu.retry.out_of_range");

    values.at("retry.initial_delay").data = Duration::zero();
    values.at("retry.max_delay").data     = Duration::zero();
    current.recovery_time                 = UtcTimePoint::max();
    result                                = recovery_retry_decision(values, current);
    REQUIRE(result);
    REQUIRE(result->retry);
    CHECK(result->retry->due_at == UtcTimePoint::max());

    CHECK_FALSE(checked_retry_due_time(UtcTimePoint{}, -1s));
    auto earliest = checked_retry_due_time(UtcTimePoint::min(), 10s);
    REQUIRE(earliest);
    CHECK(*earliest == UtcTimePoint::min() + 10s);
}
