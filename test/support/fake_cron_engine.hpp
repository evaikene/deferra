/** @file fake_cron_engine.hpp
 * @brief Defines a deterministic cron engine for owner-thread tests.
 */
#pragma once

#include "cron.hpp"

#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace jb::test {

/** Test-only CronEngine with configured occurrence sequences and call recording.
 *
 * Tests configure exact expression/timezone pairs. Validation fails for an unconfigured pair, and next_after()
 * returns the first configured occurrence strictly after its supplied lower bound. Optional injected errors override
 * those behaviors.
 */
class FakeCronEngine final : public jobu::CronEngine {
public:
    struct NextCall {
        jobu::CronSchedule schedule;
        core::UtcTimePoint exclusive_lower_bound;
    };

    /// Replaces the strictly increasing occurrence sequence for one exact schedule.
    void set_occurrences(jobu::CronSchedule const& schedule, std::vector<core::UtcTimePoint> occurrences)
    {
        std::ranges::sort(occurrences);
        auto const duplicates = std::ranges::unique(occurrences);
        occurrences.erase(duplicates.begin(), duplicates.end());
        _occurrences.insert_or_assign(key(schedule), std::move(occurrences));
    }

    /// Injects or clears the error returned by validate().
    void set_validation_error(std::optional<core::Error> error) { _validation_error = std::move(error); }

    /// Injects or clears the error returned by next_after().
    void set_next_error(std::optional<core::Error> error) { _next_error = std::move(error); }

    /// Returns validation calls in observation order.
    [[nodiscard]] auto validation_calls() const noexcept -> std::vector<jobu::CronSchedule> const&
    {
        return _validation_calls;
    }

    /// Returns next-occurrence calls in observation order.
    [[nodiscard]] auto next_calls() const noexcept -> std::vector<NextCall> const& { return _next_calls; }

    [[nodiscard]] auto validate(jobu::CronSchedule const& schedule) const -> core::Result<void, core::Error> override
    {
        _validation_calls.push_back(schedule);
        if (_validation_error) {
            return core::Result<void, core::Error>::failure(*_validation_error);
        }
        if (!_occurrences.contains(key(schedule))) {
            return core::Result<void, core::Error>::failure(not_configured());
        }
        return core::Result<void, core::Error>::success();
    }

    [[nodiscard]] auto next_after(jobu::CronSchedule const& schedule, core::UtcTimePoint exclusive_lower_bound) const
        -> core::Result<core::UtcTimePoint, core::Error> override
    {
        _next_calls.push_back({.schedule = schedule, .exclusive_lower_bound = exclusive_lower_bound});
        if (_next_error) {
            return core::Result<core::UtcTimePoint, core::Error>::failure(*_next_error);
        }
        auto found = _occurrences.find(key(schedule));
        if (found == _occurrences.end()) {
            return core::Result<core::UtcTimePoint, core::Error>::failure(not_configured());
        }
        auto const occurrence = std::upper_bound(found->second.begin(), found->second.end(), exclusive_lower_bound);
        if (occurrence == found->second.end()) {
            return core::Result<core::UtcTimePoint, core::Error>::failure(sequence_exhausted());
        }
        return core::Result<core::UtcTimePoint, core::Error>::success(*occurrence);
    }

private:
    using ScheduleKey = std::pair<std::string, std::string>;

    [[nodiscard]] static auto key(jobu::CronSchedule const& schedule) -> ScheduleKey
    {
        return {schedule.expression, schedule.timezone};
    }

    [[nodiscard]] static auto not_configured() -> core::Error
    {
        return {
            .category = core::ErrorCategory::InvalidArgument,
            .code     = "test.cron.schedule_not_configured",
            .message  = "The cron schedule was not configured in the test engine",
        };
    }

    [[nodiscard]] static auto sequence_exhausted() -> core::Error
    {
        return {
            .category = core::ErrorCategory::ResourceExhausted,
            .code     = "test.cron.sequence_exhausted",
            .message  = "The configured cron occurrence sequence has no later value",
        };
    }

    std::map<ScheduleKey, std::vector<core::UtcTimePoint>> _occurrences;
    std::optional<core::Error>                             _validation_error;
    std::optional<core::Error>                             _next_error;
    mutable std::vector<jobu::CronSchedule>                _validation_calls;
    mutable std::vector<NextCall>                          _next_calls;
};

} // namespace jb::test
