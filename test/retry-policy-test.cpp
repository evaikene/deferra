#include "retry_policy_priv.hpp"

#include "attribute_registry.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace jb::core;
using namespace jb::jobu;
using namespace jb::jobu::detail;
using namespace std::chrono_literals;

namespace {

auto run_id(std::uint8_t offset = 0) -> Uuid
{
    auto bytes = Uuid::Storage{};
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        bytes[index] = static_cast<std::byte>(index + offset);
    }
    return Uuid{bytes};
}

auto retry_attributes(std::int64_t max_attempts  = 5,
                      std::string  strategy      = "fixed",
                      Duration     initial_delay = 1s,
                      Duration     max_delay     = 30s,
                      double       multiplier    = 2.0,
                      double       jitter        = 0.0,
                      std::string  mode          = "reschedule") -> AttributeSet
{
    return {
        {"retry.initial_delay", {.data = initial_delay}      },
        {"retry.jitter",        {.data = jitter}             },
        {"retry.max_attempts",  {.data = max_attempts}       },
        {"retry.max_delay",     {.data = max_delay}          },
        {"retry.mode",          {.data = std::move(mode)}    },
        {"retry.multiplier",    {.data = multiplier}         },
        {"retry.strategy",      {.data = std::move(strategy)}},
    };
}

void check_invalid_policy(auto const& result)
{
    REQUIRE_FALSE(result);
    CHECK(result.error().category == ErrorCategory::Internal);
    CHECK(result.error().code == "jobu.retry.invalid_policy");
}

auto fixed_policy(Duration initial_delay = 1s, Duration max_delay = 30s) -> RetryPolicy
{
    return {
        .max_attempts  = 100,
        .strategy      = RetryStrategy::Fixed,
        .initial_delay = initial_delay,
        .max_delay     = max_delay,
        .multiplier    = 2.0,
        .jitter        = 0.0,
        .mode          = RetryMode::Reschedule,
    };
}

} // anonymous namespace

TEST_CASE("Retry policy decoding accepts complete materialized policy attributes", "[jobu][retry]")
{
    StandardAttributeRegistry registry;
    auto                      attributes =
        materialize_attributes(registry, {}, {}, retry_attributes(7, "exponential", 3s, 2min, 2.5, 0.25, "blocking"));
    REQUIRE(attributes);
    REQUIRE(attributes->size() == registry.definitions().size());

    auto policy = retry_policy_from_attributes(*attributes);
    REQUIRE(policy);
    CHECK(policy->max_attempts == 7);
    CHECK(policy->strategy == RetryStrategy::Exponential);
    CHECK(policy->initial_delay == 3s);
    CHECK(policy->max_delay == 2min);
    CHECK(policy->multiplier == 2.5);
    CHECK(policy->jitter == 0.25);
    CHECK(policy->mode == RetryMode::Blocking);
}

TEST_CASE("Retry policy decoding rejects missing fields and wrong storage alternatives", "[jobu][retry]")
{
    constexpr std::array<std::string_view, 7> names{
        "retry.initial_delay",
        "retry.jitter",
        "retry.max_attempts",
        "retry.max_delay",
        "retry.mode",
        "retry.multiplier",
        "retry.strategy",
    };

    for (auto const name : names) {
        CAPTURE(name);
        auto missing = retry_attributes();
        REQUIRE(missing.erase(std::string{name}) == 1U);
        check_invalid_policy(retry_policy_from_attributes(missing));

        auto wrong_type                       = retry_attributes();
        wrong_type.at(std::string{name}).data = true;
        check_invalid_policy(retry_policy_from_attributes(wrong_type));
    }
}

TEST_CASE("Retry policy decoding independently enforces every registry constraint", "[jobu][retry]")
{
    auto invalid_sets = std::vector<AttributeSet>{};
    auto add_invalid  = [&invalid_sets](std::string_view name, AttributeValue value) {
        auto attributes                  = retry_attributes();
        attributes.at(std::string{name}) = std::move(value);
        invalid_sets.push_back(std::move(attributes));
    };

    add_invalid("retry.max_attempts", {.data = std::int64_t{0}});
    add_invalid("retry.max_attempts", {.data = std::int64_t{101}});
    add_invalid("retry.strategy", {.data = std::string{"linear"}});
    add_invalid("retry.initial_delay", {.data = -1ns});
    add_invalid("retry.initial_delay", {.data = 24h + 1ns});
    add_invalid("retry.max_delay", {.data = -1ns});
    add_invalid("retry.max_delay", {.data = std::chrono::days{30} + 1ns});
    add_invalid("retry.mode", {.data = std::string{"immediate"}});
    add_invalid("retry.multiplier", {.data = 0.99});
    add_invalid("retry.multiplier", {.data = 100.01});
    add_invalid("retry.multiplier", {.data = std::numeric_limits<double>::infinity()});
    add_invalid("retry.multiplier", {.data = std::numeric_limits<double>::quiet_NaN()});
    add_invalid("retry.jitter", {.data = -0.01});
    add_invalid("retry.jitter", {.data = 1.01});
    add_invalid("retry.jitter", {.data = std::numeric_limits<double>::infinity()});
    add_invalid("retry.jitter", {.data = std::numeric_limits<double>::quiet_NaN()});

    auto crossed = retry_attributes(5, "fixed", 2s, 1s);
    invalid_sets.push_back(std::move(crossed));

    for (auto const& attributes : invalid_sets) {
        check_invalid_policy(retry_policy_from_attributes(attributes));
    }
}

TEST_CASE("Retry policy errors identify fields without echoing supplied values", "[jobu][retry]")
{
    auto attributes                      = retry_attributes();
    attributes.at("retry.strategy").data = std::string{"private-policy-value"};

    auto policy = retry_policy_from_attributes(attributes);
    check_invalid_policy(policy);
    CHECK(policy.error().message.find("private-policy-value") == std::string::npos);
    CHECK(policy.error().detail.find("private-policy-value") == std::string::npos);
    CHECK(policy.error().detail.find("retry.strategy") != std::string::npos);
}

TEST_CASE("Fixed and exponential retry delays use the next attempt number and cap safely", "[jobu][retry]")
{
    auto fixed = fixed_policy(1500ms, 20s);
    for (auto const attempt : {AttemptNumber{2}, AttemptNumber{3}, AttemptNumber{100}}) {
        CAPTURE(attempt);
        auto delay = retry_delay(fixed, run_id(), attempt);
        REQUIRE(delay);
        CHECK(*delay == 1500ms);
    }

    auto exponential       = fixed_policy(1s, 10s);
    exponential.strategy   = RetryStrategy::Exponential;
    exponential.multiplier = 2.0;
    constexpr std::array expected{
        std::pair{AttemptNumber{2},   Duration{1s} },
        std::pair{AttemptNumber{3},   Duration{2s} },
        std::pair{AttemptNumber{4},   Duration{4s} },
        std::pair{AttemptNumber{5},   Duration{8s} },
        std::pair{AttemptNumber{6},   Duration{10s}},
        std::pair{AttemptNumber{100}, Duration{10s}},
    };
    for (auto const& [attempt, expected_delay] : expected) {
        CAPTURE(attempt);
        auto delay = retry_delay(exponential, run_id(), attempt);
        REQUIRE(delay);
        CHECK(*delay == expected_delay);
    }

    exponential.initial_delay = 2s;
    exponential.max_delay     = 20s;
    exponential.multiplier    = 1.5;
    auto fractional           = retry_delay(exponential, run_id(), 5);
    REQUIRE(fractional);
    CHECK(*fractional == 6750ms);

    exponential.initial_delay = 1ns;
    exponential.max_delay     = 1s;
    auto single_nanos         = retry_delay(exponential, run_id(), 4);
    REQUIRE(single_nanos);
    CHECK(*single_nanos == 2ns);

    exponential.initial_delay = 24h;
    exponential.max_delay     = std::chrono::days{30};
    exponential.multiplier    = 100.0;
    auto saturated            = retry_delay(exponential, run_id(), 100);
    REQUIRE(saturated);
    CHECK(*saturated == std::chrono::days{30});
}

TEST_CASE("Retry jitter is deterministic, bounded, and pinned to fixed-width inputs", "[jobu][retry]")
{
    auto policy   = fixed_policy(10s, 30s);
    policy.jitter = 0.25;

    auto first  = retry_delay(policy, run_id(), 2);
    auto repeat = retry_delay(policy, run_id(), 2);
    auto next   = retry_delay(policy, run_id(), 3);
    auto other  = retry_delay(policy, run_id(1), 2);
    REQUIRE(first);
    REQUIRE(repeat);
    REQUIRE(next);
    REQUIRE(other);
    CHECK(first->count() == 8'293'882'753);
    CHECK(*repeat == *first);
    CHECK(*next != *first);
    CHECK(*other != *first);
    CHECK(*first >= 7500ms);
    CHECK(*first <= 12500ms);
    CHECK(*first <= policy.max_delay);

    for (auto attempt = AttemptNumber{2}; attempt <= policy.max_attempts; ++attempt) {
        auto delay = retry_delay(policy, run_id(), attempt);
        REQUIRE(delay);
        CHECK(*delay >= 7500ms);
        CHECK(*delay <= 12500ms);
        CHECK(*delay <= policy.max_delay);
    }

    policy.initial_delay = Duration::zero();
    policy.jitter        = 1.0;
    auto zero            = retry_delay(policy, run_id(), 100);
    REQUIRE(zero);
    CHECK(*zero == Duration::zero());
}

TEST_CASE("Retry delay rejects invalid policies and attempt boundaries", "[jobu][retry]")
{
    auto policy = fixed_policy();
    check_invalid_policy(retry_delay(policy, run_id(), 0));
    check_invalid_policy(retry_delay(policy, run_id(), 1));
    check_invalid_policy(retry_delay(policy, run_id(), std::numeric_limits<AttemptNumber>::max()));

    policy.max_delay = 500ms;
    check_invalid_policy(retry_delay(policy, run_id(), 2));

    policy            = fixed_policy();
    policy.multiplier = std::numeric_limits<double>::infinity();
    check_invalid_policy(retry_delay(policy, run_id(), 2));

    policy          = fixed_policy();
    policy.strategy = static_cast<RetryStrategy>(255);
    check_invalid_policy(retry_delay(policy, run_id(), 2));
}

TEST_CASE("Retry decisions combine policy eligibility, delay, mode, and executor deadline", "[jobu][retry]")
{
    auto attributes = retry_attributes(3, "fixed", 5s, 10s, 2.0, 0.0, "blocking");
    auto completion = RetryCompletion{
        .run_id           = run_id(),
        .attempt_number   = 1,
        .outcome          = AttemptOutcome::Failed,
        .retryable        = true,
        .retry_allowed    = true,
        .completed_at     = UtcTimePoint{100s},
        .retry_not_before = std::nullopt,
    };

    auto decision = retry_decision(attributes, completion);
    REQUIRE(decision);
    REQUIRE(decision->retry);
    CHECK(decision->retry->due_at == UtcTimePoint{105s});
    CHECK(decision->retry->mode == RetryMode::Blocking);

    completion.retry_not_before = UtcTimePoint{110s};
    decision                    = retry_decision(attributes, completion);
    REQUIRE(decision);
    REQUIRE(decision->retry);
    CHECK(decision->retry->due_at == UtcTimePoint{110s});

    completion.retry_not_before = UtcTimePoint{103s};
    decision                    = retry_decision(attributes, completion);
    REQUIRE(decision);
    REQUIRE(decision->retry);
    CHECK(decision->retry->due_at == UtcTimePoint{105s});

    attributes.at("retry.mode").data = std::string{"reschedule"};
    decision                         = retry_decision(attributes, completion);
    REQUIRE(decision);
    REQUIRE(decision->retry);
    CHECK(decision->retry->mode == RetryMode::Reschedule);
}

TEST_CASE("Retry decisions terminalize ineligible completions and exhausted attempts", "[jobu][retry]")
{
    auto attributes = retry_attributes(3, "fixed", 1s, 10s);
    auto completion = RetryCompletion{
        .run_id           = run_id(),
        .attempt_number   = 1,
        .outcome          = AttemptOutcome::Failed,
        .retryable        = true,
        .retry_allowed    = true,
        .completed_at     = UtcTimePoint{100s},
        .retry_not_before = std::nullopt,
    };

    for (auto const outcome : {
             AttemptOutcome::Succeeded,
             AttemptOutcome::Cancelled,
             AttemptOutcome::Interrupted,
         }) {
        CAPTURE(outcome);
        completion.outcome = outcome;
        auto decision      = retry_decision(attributes, completion);
        REQUIRE(decision);
        CHECK_FALSE(decision->retry);
    }

    completion.outcome   = AttemptOutcome::Failed;
    completion.retryable = false;
    auto decision        = retry_decision(attributes, completion);
    REQUIRE(decision);
    CHECK_FALSE(decision->retry);

    completion.retryable     = true;
    completion.retry_allowed = false;
    decision                 = retry_decision(attributes, completion);
    REQUIRE(decision);
    CHECK_FALSE(decision->retry);

    completion.retry_allowed  = true;
    completion.attempt_number = 3;
    decision                  = retry_decision(attributes, completion);
    REQUIRE(decision);
    CHECK_FALSE(decision->retry);

    completion.attempt_number = std::numeric_limits<AttemptNumber>::max();
    decision                  = retry_decision(attributes, completion);
    REQUIRE(decision);
    CHECK_FALSE(decision->retry);
}

TEST_CASE("Retry decisions report invalid input and checked due-time overflow", "[jobu][retry]")
{
    auto attributes = retry_attributes(3, "fixed", 5s, 10s);
    auto completion = RetryCompletion{
        .run_id           = run_id(),
        .attempt_number   = 0,
        .outcome          = AttemptOutcome::Failed,
        .retryable        = true,
        .retry_allowed    = true,
        .completed_at     = UtcTimePoint{100s},
        .retry_not_before = std::nullopt,
    };
    check_invalid_policy(retry_decision(attributes, completion));

    completion.attempt_number = 1;
    completion.completed_at   = UtcTimePoint::max() - 2s;
    auto overflow             = retry_decision(attributes, completion);
    REQUIRE_FALSE(overflow);
    CHECK(overflow.error().category == ErrorCategory::ResourceExhausted);
    CHECK(overflow.error().code == "jobu.retry.out_of_range");

    auto zero_attributes    = retry_attributes(3, "fixed", Duration::zero(), Duration::zero());
    completion.completed_at = UtcTimePoint::max();
    auto zero               = retry_decision(zero_attributes, completion);
    REQUIRE(zero);
    REQUIRE(zero->retry);
    CHECK(zero->retry->due_at == UtcTimePoint::max());

    attributes.erase("retry.mode");
    completion.outcome = AttemptOutcome::Succeeded;
    check_invalid_policy(retry_decision(attributes, completion));
}
