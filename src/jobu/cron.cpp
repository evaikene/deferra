#include "cron.hpp"

#include "cron_expression_priv.hpp"
#include "cron_timezone_priv.hpp"

#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace jb::jobu {

namespace {

using TimezonePointerResult = jb::core::Result<TimezoneData const*, jb::core::Error>;
using OccurrenceResult      = jb::core::Result<jb::core::UtcTimePoint, jb::core::Error>;

auto invalid_count() -> jb::core::Error
{
    return {
        .category = jb::core::ErrorCategory::InvalidArgument,
        .code     = "jobu.schedule.invalid_count",
        .message  = "Cron occurrence count must be from 1 through 200",
    };
}

auto no_future_occurrence() -> jb::core::Error
{
    return {
        .category = jb::core::ErrorCategory::InvalidArgument,
        .code     = "jobu.schedule.no_future_occurrence",
        .message  = "Cron expression has no future occurrence",
    };
}

auto occurrence_out_of_range() -> jb::core::Error
{
    return {
        .category = jb::core::ErrorCategory::ResourceExhausted,
        .code     = "jobu.schedule.out_of_range",
        .message  = "Cron occurrence is outside the representable range",
    };
}

auto fixed_utc_data() -> TimezoneData
{
    return {
        .transitions             = {},
        .local_time_types        = {{.utc_offset_seconds = 0, .daylight = false}},
        .default_local_time_type = 0,
        .future_rule             = std::nullopt,
    };
}

auto local_year(TimezoneLocalTimePoint value) -> std::optional<std::chrono::year>
{
    using namespace std::chrono;

    static constexpr auto kMinimumCalendarDay = local_days{year::min() / January / day{1}};
    static constexpr auto kMaximumCalendarDay = local_days{year::max() / December / last};

    auto const local_day = floor<days>(value);
    if (local_day < kMinimumCalendarDay || local_day > kMaximumCalendarDay) {
        return std::nullopt;
    }
    return year_month_day{local_day}.year();
}

auto timezone_local_candidate(LocalMinute value) -> jb::core::Result<TimezoneLocalTimePoint, jb::core::Error>
{
    using Result = jb::core::Result<TimezoneLocalTimePoint, jb::core::Error>;

    auto const minimum = std::chrono::ceil<std::chrono::minutes>(jb::core::UtcClock::duration::min()).count();
    auto const maximum = std::chrono::floor<std::chrono::minutes>(jb::core::UtcClock::duration::max()).count();
    auto const count   = value.time_since_epoch().count();
    if (count < minimum || count > maximum) {
        return Result::failure(occurrence_out_of_range());
    }
    return Result::success(
        TimezoneLocalTimePoint{std::chrono::duration_cast<jb::core::UtcClock::duration>(value.time_since_epoch())});
}

} // namespace

struct SystemCronEngine::Private {
    TimezoneData                                     utc{fixed_utc_data()};
    std::map<std::string, TimezoneData, std::less<>> timezones;

    auto find_timezone(std::string_view name) -> TimezonePointerResult
    {
        if (name == "UTC") {
            return TimezonePointerResult::success(&utc);
        }

        auto const cached = timezones.find(name);
        if (cached != timezones.end()) {
            return TimezonePointerResult::success(&cached->second);
        }

        auto loaded = load_timezone_data(name);
        if (!loaded) {
            return TimezonePointerResult::failure(std::move(loaded).error());
        }
        auto const [stored, inserted] = timezones.emplace(std::string{name}, std::move(loaded).value());
        static_cast<void>(inserted);
        return TimezonePointerResult::success(&stored->second);
    }
};

SystemCronEngine::SystemCronEngine()
    : _data{std::make_unique<Private>()}
{}

SystemCronEngine::~SystemCronEngine() = default;

SystemCronEngine::SystemCronEngine(SystemCronEngine&&) noexcept = default;

auto SystemCronEngine::operator=(SystemCronEngine&&) noexcept -> SystemCronEngine& = default;

auto SystemCronEngine::validate(CronSchedule const& schedule) const -> jb::core::Result<void, jb::core::Error>
{
    auto expression = parse_cron_expression(schedule.expression);
    if (!expression) {
        return jb::core::Result<void, jb::core::Error>::failure(std::move(expression).error());
    }

    auto timezone = _data->find_timezone(schedule.timezone);
    if (!timezone) {
        return jb::core::Result<void, jb::core::Error>::failure(std::move(timezone).error());
    }
    return jb::core::Result<void, jb::core::Error>::success();
}

auto SystemCronEngine::next_after(CronSchedule const& schedule, jb::core::UtcTimePoint exclusive_lower_bound) const
    -> jb::core::Result<jb::core::UtcTimePoint, jb::core::Error>
{
    auto expression = parse_cron_expression(schedule.expression);
    if (!expression) {
        return OccurrenceResult::failure(std::move(expression).error());
    }

    auto timezone = _data->find_timezone(schedule.timezone);
    if (!timezone) {
        return OccurrenceResult::failure(std::move(timezone).error());
    }

    auto local_lower = timezone_local_time(**timezone, exclusive_lower_bound);
    if (!local_lower) {
        return OccurrenceResult::failure(std::move(local_lower).error());
    }
    auto local_cursor = *local_lower;

    auto const first_year = local_year(local_cursor);
    if (!first_year || static_cast<int>(*first_year) > static_cast<int>(std::chrono::year::max()) - 399) {
        return OccurrenceResult::failure(occurrence_out_of_range());
    }
    auto const last_year = static_cast<int>(*first_year) + 399;

    while (true) {
        auto local_candidate = next_local_occurrence(*expression, local_cursor);
        if (!local_candidate) {
            return OccurrenceResult::failure(std::move(local_candidate).error());
        }

        auto candidate = timezone_local_candidate(*local_candidate);
        if (!candidate) {
            return OccurrenceResult::failure(std::move(candidate).error());
        }
        auto const candidate_year = local_year(*candidate);
        if (!candidate_year) {
            return OccurrenceResult::failure(occurrence_out_of_range());
        }
        if (static_cast<int>(*candidate_year) > last_year) {
            return OccurrenceResult::failure(no_future_occurrence());
        }

        auto utc_candidate = timezone_utc_time(**timezone, *candidate);
        if (!utc_candidate) {
            return OccurrenceResult::failure(std::move(utc_candidate).error());
        }
        if (*utc_candidate > exclusive_lower_bound) {
            return utc_candidate;
        }

        local_cursor = *candidate;
    }
}

auto next_cron_occurrences(CronEngine const&      engine,
                           CronSchedule const&    schedule,
                           jb::core::UtcTimePoint exclusive_lower_bound,
                           std::size_t count) -> jb::core::Result<std::vector<jb::core::UtcTimePoint>, jb::core::Error>
{
    using Result = jb::core::Result<std::vector<jb::core::UtcTimePoint>, jb::core::Error>;

    if (count == 0 || count > 200) {
        return Result::failure(invalid_count());
    }

    std::vector<jb::core::UtcTimePoint> occurrences;
    occurrences.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        auto occurrence = engine.next_after(schedule, exclusive_lower_bound);
        if (!occurrence) {
            return Result::failure(std::move(occurrence).error());
        }
        exclusive_lower_bound = *occurrence;
        occurrences.push_back(*occurrence);
    }
    return Result::success(std::move(occurrences));
}

} // namespace jb::jobu
