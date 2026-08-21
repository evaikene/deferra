#include "cron_expression_priv.hpp"

#include "utc_timestamp.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <string>
#include <string_view>
#include <utility>

using namespace jb::core;
using namespace jb::jobu;

namespace {

auto parsed_expression(std::string_view text) -> CronExpression
{
    auto parsed = parse_cron_expression(text);
    REQUIRE(parsed);
    return std::move(parsed).value();
}

auto parsed_time(std::string_view text) -> UtcTimePoint
{
    auto parsed = parse_utc_timestamp(text);
    REQUIRE(parsed);
    return *parsed;
}

auto fixed_utc_next(std::string_view expression, std::string_view lower_bound)
    -> jb::core::Result<LocalMinute, jb::core::Error>
{
    auto parsed = parse_cron_expression(expression);
    REQUIRE(parsed);
    auto const lower = parsed_time(lower_bound);
    return next_local_occurrence(*parsed, LocalTimePoint{lower.time_since_epoch()});
}

auto formatted_fixed_utc(LocalMinute value) -> std::string
{
    auto formatted =
        format_utc_timestamp(UtcTimePoint{std::chrono::duration_cast<UtcClock::duration>(value.time_since_epoch())});
    REQUIRE(formatted);
    return std::move(formatted).value();
}

void check_next(std::string_view expression, std::string_view lower_bound, std::string_view expected)
{
    auto next = fixed_utc_next(expression, lower_bound);
    REQUIRE(next);
    CHECK(formatted_fixed_utc(*next) == expected);
}

} // namespace

TEST_CASE("Cron fields accept their boundaries, lists, ranges, and steps", "[jobu][cron]")
{
    auto const lower = parsed_expression("0 0 1 1 0");
    CHECK(lower.minutes.count() == 1);
    CHECK(lower.minutes.test(0));
    CHECK(lower.hours.test(0));
    CHECK(lower.days_of_month.test(0));
    CHECK(lower.months.test(0));
    CHECK(lower.days_of_week.test(0));

    auto const upper = parsed_expression("59 23 31 12 7");
    CHECK(upper.minutes.test(59));
    CHECK(upper.hours.test(23));
    CHECK(upper.days_of_month.test(30));
    CHECK(upper.months.test(11));
    CHECK(upper.days_of_week.test(0));

    auto const combined = parsed_expression("0,15,30,45 1-5/2 1,15-20/5 */2 MON-FRI");
    CHECK(combined.minutes.count() == 4);
    CHECK(combined.minutes.test(0));
    CHECK(combined.minutes.test(15));
    CHECK(combined.minutes.test(30));
    CHECK(combined.minutes.test(45));
    CHECK(combined.hours.count() == 3);
    CHECK(combined.hours.test(1));
    CHECK(combined.hours.test(3));
    CHECK(combined.hours.test(5));
    CHECK(combined.days_of_month.count() == 3);
    CHECK(combined.days_of_month.test(0));
    CHECK(combined.days_of_month.test(14));
    CHECK(combined.days_of_month.test(19));
    CHECK(combined.months.count() == 6);
    CHECK(combined.months.test(0));
    CHECK(combined.months.test(10));
    CHECK(combined.days_of_week.count() == 5);
}

TEST_CASE("Cron aliases, names, and ASCII field whitespace normalize deterministically", "[jobu][cron]")
{
    CHECK(parsed_expression("@hourly") == parsed_expression("0 * * * *"));
    CHECK(parsed_expression("@daily") == parsed_expression("0 0 * * *"));
    CHECK(parsed_expression("@weekly") == parsed_expression("0 0 * * SUN"));
    CHECK(parsed_expression("\t 0\t12\t1\tjan\tmon \t") == parsed_expression("0 12 1 JAN MON"));
    CHECK(parsed_expression("0 0 1 jan-mar/2 sun,tUe") == parsed_expression("0 0 1 1,3 SUN,TUE"));
}

TEST_CASE("Cron weekday ranges are cyclic and retain an explicit step anchor", "[jobu][cron]")
{
    for (auto const* const expression : {
             "* * * * SUN-SAT",
             "* * * * MON-SUN",
             "* * * * 0-6",
             "* * * * 1-7",
         }) {
        CHECK(parsed_expression(expression).days_of_week.count() == 7);
    }

    CHECK(parsed_expression("* * * * 0") == parsed_expression("* * * * 7"));
    CHECK(parsed_expression("* * * * FRI-0") == parsed_expression("* * * * FRI-7"));

    auto const wrapped = parsed_expression("* * * * FRI-MON");
    CHECK(wrapped.days_of_week.count() == 4);
    CHECK(wrapped.days_of_week.test(5));
    CHECK(wrapped.days_of_week.test(6));
    CHECK(wrapped.days_of_week.test(0));
    CHECK(wrapped.days_of_week.test(1));

    auto const equal = parsed_expression("* * * * SUN-SUN");
    CHECK(equal.days_of_week.count() == 1);
    CHECK(equal.days_of_week.test(0));

    auto const stepped = parsed_expression("* * * * FRI-MON/2");
    CHECK(stepped.days_of_week.count() == 2);
    CHECK(stepped.days_of_week.test(5));
    CHECK(stepped.days_of_week.test(0));

    auto const sunday_anchor = parsed_expression("* * * * SUN-SAT/2");
    auto const monday_anchor = parsed_expression("* * * * MON-SUN/2");
    auto const zero_anchor   = parsed_expression("* * * * 0-6/2");
    auto const one_anchor    = parsed_expression("* * * * 1-7/2");
    CHECK(sunday_anchor.days_of_week.test(0));
    CHECK_FALSE(sunday_anchor.days_of_week.test(1));
    CHECK(monday_anchor.days_of_week.test(1));
    CHECK_FALSE(monday_anchor.days_of_week.test(2));
    CHECK(sunday_anchor != monday_anchor);
    CHECK(zero_anchor == sunday_anchor);
    CHECK(one_anchor == monday_anchor);
}

TEST_CASE("Cron parsing rejects unsupported and ambiguous syntax", "[jobu][cron]")
{
    constexpr auto rejected = std::array{
        std::string_view{""},
        std::string_view{"* * * *"},
        std::string_view{"* * * * * *"},
        std::string_view{"60 * * * *"},
        std::string_view{"* 24 * * *"},
        std::string_view{"* * 0 * *"},
        std::string_view{"* * 32 * *"},
        std::string_view{"* * * 0 *"},
        std::string_view{"* * * 13 *"},
        std::string_view{"* * * * 8"},
        std::string_view{"5-1 * * * *"},
        std::string_view{"* 10-2 * * *"},
        std::string_view{"* * 20-10 * *"},
        std::string_view{"* * * DEC-JAN *"},
        std::string_view{"*/0 * * * *"},
        std::string_view{"5/2 * * * *"},
        std::string_view{"1//2 * * * *"},
        std::string_view{"1,,2 * * * *"},
        std::string_view{"1, * * * *"},
        std::string_view{"* * * * */1"},
        std::string_view{"* * * * SUN/2"},
        std::string_view{"* * * * 0-7"},
        std::string_view{"* * * * 7-0"},
        std::string_view{"* * * * 0,7"},
        std::string_view{"* * * * 0-3,5-7"},
        std::string_view{"* * * FOO *"},
        std::string_view{"1x * * * *"},
        std::string_view{"* * ? * *"},
        std::string_view{"* * L * *"},
        std::string_view{"* * W * *"},
        std::string_view{"* * * * MON#2"},
        std::string_view{"@monthly"},
        std::string_view{"@HOURLY"},
        std::string_view{"* * * * *\n"},
    };

    for (auto const expression : rejected) {
        auto const parsed = parse_cron_expression(expression);
        REQUIRE_FALSE(parsed);
        CHECK(parsed.error().category == ErrorCategory::InvalidArgument);
        CHECK(parsed.error().code == "jobu.schedule.invalid_expression");
    }

    auto embedded_nul = std::string{"* * * * *"};
    embedded_nul[4]   = '\0';
    CHECK(parse_cron_expression(embedded_nul).error().code == "jobu.schedule.invalid_expression");
    CHECK(parse_cron_expression(std::string(513, ' ')).error().code == "jobu.schedule.invalid_expression");
}

TEST_CASE("Cron local search is strictly exclusive and advances calendar components", "[jobu][cron]")
{
    check_next("* * * * *", "2024-05-17T12:34:00Z", "2024-05-17T12:35:00.000000Z");
    check_next("* * * * *", "2024-05-17T12:34:00.000001Z", "2024-05-17T12:35:00.000000Z");
    check_next("* * * * *", "2024-05-17T12:34:59.999999Z", "2024-05-17T12:35:00.000000Z");
    check_next("0 * * * *", "2024-05-17T12:59:59.999999Z", "2024-05-17T13:00:00.000000Z");
    check_next("0 0 * * *", "2024-05-31T23:59:59.999999Z", "2024-06-01T00:00:00.000000Z");
    check_next("0 0 1 JAN *", "2024-12-31T23:59:59.999999Z", "2025-01-01T00:00:00.000000Z");
    check_next("@weekly", "2024-05-17T12:00:00Z", "2024-05-19T00:00:00.000000Z");
}

TEST_CASE("Cron local search applies day-of-month and weekday with AND semantics", "[jobu][cron]")
{
    check_next("0 9 13 * FRI", "2024-01-01T00:00:00Z", "2024-09-13T09:00:00.000000Z");
    check_next("0 0 29 FEB MON", "2015-01-01T00:00:00Z", "2016-02-29T00:00:00.000000Z");
    check_next("0 0 31 * *", "2024-04-30T00:00:00Z", "2024-05-31T00:00:00.000000Z");
}

TEST_CASE("Cron local search honors Gregorian leap-year rules", "[jobu][cron]")
{
    check_next("0 0 29 FEB *", "2023-01-01T00:00:00Z", "2024-02-29T00:00:00.000000Z");
    check_next("0 0 29 FEB *", "1996-03-01T00:00:00Z", "2000-02-29T00:00:00.000000Z");
    check_next("0 0 29 FEB *", "2096-03-01T00:00:00Z", "2104-02-29T00:00:00.000000Z");
}

TEST_CASE("Cron local search reports bounded horizon exhaustion", "[jobu][cron]")
{
    auto const next = fixed_utc_next("0 0 31 FEB *", "2024-01-01T00:00:00Z");
    REQUIRE_FALSE(next);
    CHECK(next.error().category == ErrorCategory::InvalidArgument);
    CHECK(next.error().code == "jobu.schedule.no_future_occurrence");
}

TEST_CASE("Cron local search rejects clock values beyond the calendar range", "[jobu][cron]")
{
    using namespace std::chrono;

    auto const maximum_calendar_day = local_days{year::max() / December / last};
    if (floor<days>(LocalTimePoint::max()) <= maximum_calendar_day) {
        SUCCEED("The platform clock does not extend beyond the calendar range");
        return;
    }

    auto const out_of_range_day = maximum_calendar_day + days{1};
    auto const lower_bound = LocalTimePoint{duration_cast<UtcClock::duration>(out_of_range_day.time_since_epoch())};
    auto const next        = next_local_occurrence(parsed_expression("* * * * *"), lower_bound);

    REQUIRE_FALSE(next);
    CHECK(next.error().category == ErrorCategory::ResourceExhausted);
    CHECK(next.error().code == "jobu.schedule.out_of_range");
}
