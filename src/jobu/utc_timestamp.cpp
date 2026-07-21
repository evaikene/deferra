#include "utc_timestamp.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <utility>

namespace jb::jobu {

namespace {

using Microseconds = std::chrono::microseconds;

constexpr std::int64_t kMicrosecondsPerSecond{1000000};
constexpr std::int64_t kSecondsPerDay{86400};
constexpr std::int64_t kMicrosecondsPerDay{kSecondsPerDay * kMicrosecondsPerSecond};

struct CivilDate {
    std::int64_t year;
    unsigned     month;
    unsigned     day;
};

auto time_error(std::string code, std::string message) -> jb::core::Error
{
    return {
        .category = jb::core::ErrorCategory::InvalidArgument,
        .code     = std::move(code),
        .message  = std::move(message),
    };
}

auto invalid_format() -> jb::core::Result<jb::core::UtcTimePoint, jb::core::Error>
{
    return jb::core::Result<jb::core::UtcTimePoint, jb::core::Error>::failure(
        time_error("jobu.time.invalid_format", "UTC timestamp text is invalid"));
}

// NOLINTBEGIN(readability-magic-numbers) they make sense here

constexpr auto is_leap_year(std::int64_t year) noexcept -> bool
{
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

constexpr auto days_in_month(std::int64_t year, unsigned month) noexcept -> unsigned
{
    constexpr unsigned days[]{0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2 && is_leap_year(year)) {
        return 29;
    }
    return month <= 12 ? days[month] : 0;
}

// Howard Hinnant's civil calendar algorithms, shifted so day zero is 1970-01-01.
constexpr auto days_from_civil(std::int64_t year, unsigned month, unsigned day) noexcept -> std::int64_t
{
    year                      -= (month <= 2) ? 1 : 0;
    auto const era             = (year >= 0 ? year : year - 399) / 400;
    auto const year_of_era     = static_cast<unsigned>(year - (era * 400));
    auto const adjusted_month  = month > 2 ? month - 3U : month + 9U;
    auto const day_of_year     = (((153U * adjusted_month) + 2U) / 5U) + day - 1U;
    auto const day_of_era      = (year_of_era * 365U) + (year_of_era / 4U) - (year_of_era / 100U) + day_of_year;
    return (era * 146097) + static_cast<std::int64_t>(day_of_era) - 719468;
}

constexpr auto civil_from_days(std::int64_t days) noexcept -> CivilDate
{
    days                  += 719468;
    auto const era         = (days >= 0 ? days : days - 146096) / 146097;
    auto const day_of_era  = static_cast<unsigned>(days - (era * 146097));
    auto const year_of_era =
        (day_of_era - (day_of_era / 1460U) + (day_of_era / 36524U) - (day_of_era / 146096U)) / 365U;
    auto       year         = static_cast<std::int64_t>(year_of_era) + (era * 400);
    auto const day_of_year  = day_of_era - ((365U * year_of_era) + (year_of_era / 4U) - (year_of_era / 100U));
    auto const month_prime  = ((5U * day_of_year) + 2U) / 153U;
    auto const day          = day_of_year - (((153U * month_prime) + 2U) / 5U) + 1U;
    auto const month        = month_prime < 10U ? month_prime + 3U : month_prime - 9U;
    year                   += (month <= 2U) ? 1 : 0;
    return {.year = year, .month = month, .day = day};
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

void append_fixed(std::string& output, std::uint64_t value, unsigned width)
{
    auto const offset = output.size();
    output.resize(offset + width, '0');
    for (auto index = width; index > 0; --index) {
        output[offset + index - 1]  = static_cast<char>('0' + (value % 10U));
        value                      /= 10U;
    }
}

} // anonymous namespace

auto format_utc_timestamp(jb::core::UtcTimePoint value) -> jb::core::Result<std::string, jb::core::Error>
{
    using Result = jb::core::Result<std::string, jb::core::Error>;

    auto const microseconds = std::chrono::floor<Microseconds>(value.time_since_epoch()).count();
    auto       days         = microseconds / kMicrosecondsPerDay;
    auto       within_day   = microseconds % kMicrosecondsPerDay;
    if (within_day < 0) {
        within_day += kMicrosecondsPerDay;
        --days;
    }

    auto const date = civil_from_days(days);
    if (date.year < 0 || date.year > 9999) {
        return Result::failure(time_error("jobu.time.out_of_range", "UTC timestamp is outside the supported range"));
    }

    auto const hour      = static_cast<unsigned>(within_day / (3600 * kMicrosecondsPerSecond));
    within_day          %= 3600 * kMicrosecondsPerSecond;
    auto const minute    = static_cast<unsigned>(within_day / (60 * kMicrosecondsPerSecond));
    within_day          %= 60 * kMicrosecondsPerSecond;
    auto const second    = static_cast<unsigned>(within_day / kMicrosecondsPerSecond);
    auto const fraction  = static_cast<unsigned>(within_day % kMicrosecondsPerSecond);

    std::string output;
    output.reserve(27);
    append_fixed(output, static_cast<std::uint64_t>(date.year), 4);
    output.push_back('-');
    append_fixed(output, date.month, 2);
    output.push_back('-');
    append_fixed(output, date.day, 2);
    output.push_back('T');
    append_fixed(output, hour, 2);
    output.push_back(':');
    append_fixed(output, minute, 2);
    output.push_back(':');
    append_fixed(output, second, 2);
    output.push_back('.');
    append_fixed(output, fraction, 6);
    output.push_back('Z');
    return Result::success(std::move(output));
}

auto parse_utc_timestamp(std::string_view value) -> jb::core::Result<jb::core::UtcTimePoint, jb::core::Error>
{
    using Result = jb::core::Result<jb::core::UtcTimePoint, jb::core::Error>;

    if (value.size() < 20 || value[4] != '-' || value[7] != '-' || value[10] != 'T' || value[13] != ':' ||
        value[16] != ':') {
        return invalid_format();
    }

    unsigned year{0};
    unsigned month{0};
    unsigned day{0};
    unsigned hour{0};
    unsigned minute{0};
    unsigned second{0};
    if (!parse_digits(value, 0, 4, year) || !parse_digits(value, 5, 2, month) || !parse_digits(value, 8, 2, day) ||
        !parse_digits(value, 11, 2, hour) || !parse_digits(value, 14, 2, minute) ||
        !parse_digits(value, 17, 2, second) || month < 1 || month > 12 || day < 1 || day > days_in_month(year, month) ||
        hour > 23 || minute > 59 || second > 59) {
        return invalid_format();
    }

    unsigned fraction{0};
    if (value[19] == 'Z') {
        if (value.size() != 20) {
            return invalid_format();
        }
    }
    else {
        if (value[19] != '.' || value.back() != 'Z') {
            return invalid_format();
        }
        auto const fraction_digits = value.size() - 21;
        if (fraction_digits < 1 || fraction_digits > 6 || !parse_digits(value, 20, fraction_digits, fraction)) {
            return invalid_format();
        }
        for (auto count = fraction_digits; count < 6; ++count) {
            fraction *= 10U;
        }
    }

    auto const days    = days_from_civil(year, month, day);
    auto const seconds = (days * kSecondsPerDay) + (static_cast<std::int64_t>(hour) * 3600) +
                         (static_cast<std::int64_t>(minute) * 60) + second;
    auto const count   = (seconds * kMicrosecondsPerSecond) + fraction;

    auto const minimum = std::chrono::ceil<Microseconds>(jb::core::UtcTimePoint::min().time_since_epoch()).count();
    auto const maximum = std::chrono::floor<Microseconds>(jb::core::UtcTimePoint::max().time_since_epoch()).count();
    if (count < minimum || count > maximum) {
        return Result::failure(time_error("jobu.time.out_of_range", "UTC timestamp is outside the supported range"));
    }

    return Result::success(
        jb::core::UtcTimePoint{std::chrono::duration_cast<jb::core::UtcClock::duration>(Microseconds{count})});
}

// NOLINTEND(readability-magic-numbers)

} // namespace jb::jobu
