#include "cron_expression_priv.hpp"

#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace jb::jobu {

namespace {

using CronResult  = jb::core::Result<CronExpression, jb::core::Error>;
using LocalResult = jb::core::Result<LocalMinute, jb::core::Error>;

struct FieldName {
    std::string_view text;
    unsigned         value;
};

struct WeekdayValue {
    unsigned value;
    bool     numeric_zero{false};
    bool     numeric_seven{false};
};

constexpr auto kMonthNames = std::array{
    FieldName{.text = "JAN", .value = 1 },
    FieldName{.text = "FEB", .value = 2 },
    FieldName{.text = "MAR", .value = 3 },
    FieldName{.text = "APR", .value = 4 },
    FieldName{.text = "MAY", .value = 5 },
    FieldName{.text = "JUN", .value = 6 },
    FieldName{.text = "JUL", .value = 7 },
    FieldName{.text = "AUG", .value = 8 },
    FieldName{.text = "SEP", .value = 9 },
    FieldName{.text = "OCT", .value = 10},
    FieldName{.text = "NOV", .value = 11},
    FieldName{.text = "DEC", .value = 12},
};

constexpr auto kWeekdayNames = std::array{
    FieldName{.text = "SUN", .value = 0},
    FieldName{.text = "MON", .value = 1},
    FieldName{.text = "TUE", .value = 2},
    FieldName{.text = "WED", .value = 3},
    FieldName{.text = "THU", .value = 4},
    FieldName{.text = "FRI", .value = 5},
    FieldName{.text = "SAT", .value = 6},
};

auto schedule_error(jb::core::ErrorCategory category, std::string code, std::string message) -> jb::core::Error
{
    return {
        .category = category,
        .code     = std::move(code),
        .message  = std::move(message),
    };
}

auto invalid_expression() -> jb::core::Error
{
    return schedule_error(jb::core::ErrorCategory::InvalidArgument,
                          "jobu.schedule.invalid_expression",
                          "Cron expression is invalid");
}

auto no_future_occurrence() -> jb::core::Error
{
    return schedule_error(jb::core::ErrorCategory::InvalidArgument,
                          "jobu.schedule.no_future_occurrence",
                          "Cron expression has no future occurrence");
}

auto occurrence_out_of_range() -> jb::core::Error
{
    return schedule_error(jb::core::ErrorCategory::ResourceExhausted,
                          "jobu.schedule.out_of_range",
                          "Cron occurrence is outside the representable range");
}

constexpr auto is_field_separator(char value) noexcept -> bool
{
    return value == ' ' || value == '\t';
}

auto trim_field_separators(std::string_view value) -> std::string_view
{
    while (!value.empty() && is_field_separator(value.front())) {
        value.remove_prefix(1);
    }
    while (!value.empty() && is_field_separator(value.back())) {
        value.remove_suffix(1);
    }
    return value;
}

constexpr auto ascii_upper(char value) noexcept -> char
{
    return value >= 'a' && value <= 'z' ? static_cast<char>(value - ('a' - 'A')) : value;
}

auto equals_name(std::string_view value, std::string_view name) -> bool
{
    if (value.size() != name.size()) {
        return false;
    }
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (ascii_upper(value[index]) != name[index]) {
            return false;
        }
    }
    return true;
}

auto parse_unsigned(std::string_view value) -> std::optional<unsigned>
{
    if (value.empty()) {
        return std::nullopt;
    }

    unsigned parsed{};
    auto const [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (error != std::errc{} || end != value.data() + value.size()) {
        return std::nullopt;
    }
    return parsed;
}

auto parse_linear_value(std::string_view value, unsigned minimum, unsigned maximum, std::span<FieldName const> names)
    -> std::optional<unsigned>
{
    for (auto const& name : names) {
        if (equals_name(value, name.text)) {
            return name.value;
        }
    }

    auto const parsed = parse_unsigned(value);
    if (!parsed || *parsed < minimum || *parsed > maximum) {
        return std::nullopt;
    }
    return parsed;
}

template <std::size_t Size>
auto parse_linear_member(std::string_view           member,
                         unsigned                   minimum,
                         unsigned                   maximum,
                         std::span<FieldName const> names,
                         std::bitset<Size>&         bits) -> bool
{
    auto const slash = member.find('/');
    if (slash != std::string_view::npos && member.find('/', slash + 1) != std::string_view::npos) {
        return false;
    }

    auto const base = member.substr(0, slash);
    auto const step =
        slash == std::string_view::npos ? std::optional<unsigned>{1} : parse_unsigned(member.substr(slash + 1));
    if (base.empty() || !step || *step == 0) {
        return false;
    }

    unsigned first{};
    unsigned last{};
    if (base == "*") {
        first = minimum;
        last  = maximum;
    }
    else {
        auto const dash = base.find('-');
        if (dash == std::string_view::npos) {
            if (slash != std::string_view::npos) {
                return false;
            }
            auto const parsed = parse_linear_value(base, minimum, maximum, names);
            if (!parsed) {
                return false;
            }
            bits.set(*parsed - minimum);
            return true;
        }
        if (base.find('-', dash + 1) != std::string_view::npos) {
            return false;
        }

        auto const parsed_first = parse_linear_value(base.substr(0, dash), minimum, maximum, names);
        auto const parsed_last  = parse_linear_value(base.substr(dash + 1), minimum, maximum, names);
        if (!parsed_first || !parsed_last || *parsed_first > *parsed_last) {
            return false;
        }
        first = *parsed_first;
        last  = *parsed_last;
    }

    for (auto value = first; value <= last; value += *step) {
        bits.set(value - minimum);
        if (*step > last - value) {
            break;
        }
    }
    return true;
}

template <std::size_t Size>
auto parse_linear_field(std::string_view           field,
                        unsigned                   minimum,
                        unsigned                   maximum,
                        std::span<FieldName const> names,
                        std::bitset<Size>&         bits) -> bool
{
    std::size_t begin{};
    while (begin <= field.size()) {
        auto const comma  = field.find(',', begin);
        auto const member = field.substr(begin, comma == std::string_view::npos ? comma : comma - begin);
        if (member.empty() || !parse_linear_member(member, minimum, maximum, names, bits)) {
            return false;
        }
        if (comma == std::string_view::npos) {
            break;
        }
        begin = comma + 1;
    }
    return true;
}

auto parse_weekday_value(std::string_view value) -> std::optional<WeekdayValue>
{
    for (auto const& name : kWeekdayNames) {
        if (equals_name(value, name.text)) {
            return WeekdayValue{.value = name.value};
        }
    }

    auto const parsed = parse_unsigned(value);
    if (!parsed || *parsed > 7) {
        return std::nullopt;
    }
    // Preserve the written numeric convention while normalizing both forms of
    // Sunday so the complete field can reject mixed 0/7 usage.
    return WeekdayValue{
        .value         = *parsed == 7 ? 0U : *parsed,
        .numeric_zero  = *parsed == 0,
        .numeric_seven = *parsed == 7,
    };
}

void record_sunday_convention(WeekdayValue const& value, bool& uses_zero, bool& uses_seven)
{
    uses_zero  = uses_zero || value.numeric_zero;
    uses_seven = uses_seven || value.numeric_seven;
}

auto parse_weekday_member(std::string_view member, std::bitset<7>& bits, bool& uses_zero, bool& uses_seven) -> bool
{
    auto const slash = member.find('/');
    if (slash != std::string_view::npos && member.find('/', slash + 1) != std::string_view::npos) {
        return false;
    }

    auto const base = member.substr(0, slash);
    auto const step =
        slash == std::string_view::npos ? std::optional<unsigned>{1} : parse_unsigned(member.substr(slash + 1));
    if (base.empty() || !step || *step == 0) {
        return false;
    }
    if (base == "*") {
        // A weekday step needs an explicit range start to anchor its cyclic
        // sequence; choosing one implicitly for a wildcard would be ambiguous.
        if (slash != std::string_view::npos) {
            return false;
        }
        bits.set();
        return true;
    }

    auto const dash = base.find('-');
    if (dash == std::string_view::npos) {
        if (slash != std::string_view::npos) {
            return false;
        }
        auto const parsed = parse_weekday_value(base);
        if (!parsed) {
            return false;
        }
        record_sunday_convention(*parsed, uses_zero, uses_seven);
        bits.set(parsed->value);
        return true;
    }
    if (base.find('-', dash + 1) != std::string_view::npos) {
        return false;
    }

    auto const first = parse_weekday_value(base.substr(0, dash));
    auto const last  = parse_weekday_value(base.substr(dash + 1));
    if (!first || !last) {
        return false;
    }
    record_sunday_convention(*first, uses_zero, uses_seven);
    record_sunday_convention(*last, uses_zero, uses_seven);

    // Expand forward modulo seven, anchoring the step at the range's written
    // first value even when the range wraps through Sunday.
    auto value = first->value;
    for (unsigned offset = 0; offset < 7; ++offset) {
        if (offset % *step == 0) {
            bits.set(value);
        }
        if (value == last->value) {
            return true;
        }
        value = (value + 1U) % 7U;
    }
    return false;
}

auto parse_weekday_field(std::string_view field, std::bitset<7>& bits) -> bool
{
    bool        uses_zero{};
    bool        uses_seven{};
    std::size_t begin{};
    while (begin <= field.size()) {
        auto const comma  = field.find(',', begin);
        auto const member = field.substr(begin, comma == std::string_view::npos ? comma : comma - begin);
        if (member.empty() || !parse_weekday_member(member, bits, uses_zero, uses_seven)) {
            return false;
        }
        if (comma == std::string_view::npos) {
            break;
        }
        begin = comma + 1;
    }
    return !(uses_zero && uses_seven);
}

auto split_fields(std::string_view expression) -> std::optional<std::array<std::string_view, 5>>
{
    std::array<std::string_view, 5> fields;
    std::size_t                     count{};
    std::size_t                     position{};

    while (position < expression.size()) {
        while (position < expression.size() && is_field_separator(expression[position])) {
            ++position;
        }
        if (position == expression.size()) {
            break;
        }

        auto end = position;
        while (end < expression.size() && !is_field_separator(expression[end])) {
            ++end;
        }
        if (count == fields.size()) {
            return std::nullopt;
        }
        fields[count++] = expression.substr(position, end - position);
        position        = end;
    }

    if (count != fields.size()) {
        return std::nullopt;
    }
    return fields;
}

auto expanded_alias(std::string_view expression) -> std::string_view
{
    if (expression == "@hourly") {
        return "0 * * * *";
    }
    if (expression == "@daily") {
        return "0 0 * * *";
    }
    if (expression == "@weekly") {
        return "0 0 * * SUN";
    }
    return expression;
}

} // namespace

auto parse_cron_expression(std::string_view expression) -> jb::core::Result<CronExpression, jb::core::Error>
{
    if (expression.size() > 512 || expression.find('\0') != std::string_view::npos) {
        return CronResult::failure(invalid_expression());
    }

    // Expand aliases first so shorthand follows the same five-field parsing
    // and validation path as an explicit expression.
    expression        = expanded_alias(trim_field_separators(expression));
    auto const fields = split_fields(expression);
    if (!fields) {
        return CronResult::failure(invalid_expression());
    }

    CronExpression parsed;
    if (!parse_linear_field((*fields)[0], 0, 59, {}, parsed.minutes) ||
        !parse_linear_field((*fields)[1], 0, 23, {}, parsed.hours) ||
        !parse_linear_field((*fields)[2], 1, 31, {}, parsed.days_of_month) ||
        !parse_linear_field((*fields)[3], 1, 12, kMonthNames, parsed.months) ||
        !parse_weekday_field((*fields)[4], parsed.days_of_week)) {
        return CronResult::failure(invalid_expression());
    }
    return CronResult::success(parsed);
}

auto next_local_occurrence(CronExpression const& expression, LocalTimePoint exclusive_lower_bound)
    -> jb::core::Result<LocalMinute, jb::core::Error>
{
    using namespace std::chrono;

    auto const lower_minute = floor<minutes>(exclusive_lower_bound);
    if (lower_minute.time_since_epoch().count() == std::numeric_limits<minutes::rep>::max()) {
        return LocalResult::failure(occurrence_out_of_range());
    }
    auto const first_candidate = lower_minute + minutes{1};

    static constexpr auto kMinimumCalendarDay = local_days{year::min() / January / day{1}};
    static constexpr auto kMaximumCalendarDay = local_days{year::max() / December / last};

    auto const lower_day = floor<days>(exclusive_lower_bound);
    auto const first_day = floor<days>(first_candidate);
    if (lower_day < kMinimumCalendarDay || lower_day > kMaximumCalendarDay || first_day < kMinimumCalendarDay ||
        first_day > kMaximumCalendarDay) {
        return LocalResult::failure(occurrence_out_of_range());
    }

    auto const lower_date = year_month_day{lower_day};
    auto const first_date = year_month_day{first_day};
    auto const first_year = static_cast<int>(first_date.year());
    // Bound the exhaustive search to the Gregorian calendar's 400-year cycle
    // so an impossible date/weekday combination terminates deterministically.
    auto const last_year  = static_cast<int>(lower_date.year()) + 399;
    if (!year{last_year}.ok()) {
        return LocalResult::failure(occurrence_out_of_range());
    }

    for (auto current_year = first_year; current_year <= last_year; ++current_year) {
        for (unsigned current_month = 1; current_month <= 12; ++current_month) {
            if (!expression.months.test(current_month - 1)) {
                continue;
            }

            auto const month = std::chrono::month{current_month};
            auto const last  = year_month_day_last{year{current_year}, month_day_last{month}};
            for (unsigned current_day = 1; current_day <= static_cast<unsigned>(last.day()); ++current_day) {
                if (!expression.days_of_month.test(current_day - 1)) {
                    continue;
                }

                // Day-of-month and weekday are conjunctive: a candidate date
                // must satisfy both fields rather than traditional cron OR semantics.
                auto const date = year{current_year} / month / day{current_day};
                auto const dow  = weekday{local_days{date}}.c_encoding();
                if (!expression.days_of_week.test(dow)) {
                    continue;
                }

                auto const date_start = time_point_cast<minutes>(local_days{date});
                for (unsigned current_hour = 0; current_hour < 24; ++current_hour) {
                    if (!expression.hours.test(current_hour)) {
                        continue;
                    }
                    for (unsigned current_minute = 0; current_minute < 60; ++current_minute) {
                        if (!expression.minutes.test(current_minute)) {
                            continue;
                        }

                        auto const candidate = date_start + hours{current_hour} + minutes{current_minute};
                        if (candidate >= first_candidate) {
                            return LocalResult::success(candidate);
                        }
                    }
                }
            }
        }
    }

    return LocalResult::failure(no_future_occurrence());
}

} // namespace jb::jobu
