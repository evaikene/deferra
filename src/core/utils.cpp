#include "utils.hpp"

#include <cctype>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdlib>

namespace jb::core {

namespace {

constexpr int kSecsInMin = 60;
constexpr int kSecsInHour = 60 * kSecsInMin;
constexpr int kSecsInDay = 24 * kSecsInHour;

auto conversion_error(std::string_view type, std::string_view value) -> std::string
{
    return "invalid " + std::string{type} + ": '" + std::string{value} + "'";
}

} // anonymous namespace

auto trim_ascii_whitespace(std::string_view value) -> std::string_view
{
    auto const is_space = [](char c) -> bool { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };

    while (!value.empty() && is_space(value.front())) {
        value.remove_prefix(1);
    }
    while (!value.empty() && is_space(value.back())) {
        value.remove_suffix(1);
    }
    return value;
}

auto parse_integer(std::string_view value) -> ValueResult<long long>
{
    value = trim_ascii_whitespace(value);
    long long   parsed{};
    auto const* first  = value.data();
    auto const* last   = value.data() + value.size();
    auto const  result = std::from_chars(first, last, parsed);
    if (result.ec != std::errc{} || result.ptr != last) {
        return {.value = std::nullopt, .error = conversion_error("integer", value)};
    }
    return {.value = parsed, .error = {}};
}

auto parse_floating_point(std::string_view value) -> ValueResult<double>
{
    value = trim_ascii_whitespace(value);
    std::string text{value};

    char* end   = nullptr;
    errno       = 0;
    auto parsed = std::strtod(text.c_str(), &end);
    if (end == text.c_str() || errno == ERANGE || end != text.c_str() + text.size() || !std::isfinite(parsed)) {
        return {.value = std::nullopt, .error = conversion_error("floating point", value)};
    }
    return {.value = parsed, .error = {}};
}

auto parse_boolean(std::string_view value) -> bool
{
    value = trim_ascii_whitespace(value);
    if (value.size() == 1 && value.front() == '1') {
        return true;
    }

    std::string lower;
    lower.reserve(value.size());
    for (auto c : value) {
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }

    return lower == "y" || lower == "yes" || lower == "true" || lower == "on";
}

auto parse_duration(std::string_view value) -> ValueResult<Duration>
{
    value = trim_ascii_whitespace(value);
    if (value.size() < 2) {
        return {.value = std::nullopt, .error = conversion_error("duration", value)};
    }

    auto const unit = value.back();
    if (unit != 's' && unit != 'm' && unit != 'h' && unit != 'd') {
        return {.value = std::nullopt, .error = conversion_error("duration", value)};
    }

    auto number = value.substr(0, value.size() - 1);
    auto parsed = parse_integer(number);
    if (!parsed || *parsed.value < 0) {
        return {.value = std::nullopt, .error = conversion_error("duration", value)};
    }

    auto const  count   = *parsed.value;
    long double seconds = 0;
    if (unit == 's') {
        seconds = static_cast<long double>(count);
    }
    else if (unit == 'm') {
        seconds = static_cast<long double>(count) * kSecsInMin;
    }
    else if (unit == 'h') {
        seconds = static_cast<long double>(count) * kSecsInHour;
    }
    else if (unit == 'd') {
        seconds = static_cast<long double>(count) * kSecsInDay;
    }

    auto const duration = std::chrono::duration<long double>{seconds};
    if (duration > std::chrono::duration<long double>{Duration::max()}) {
        return {.value = std::nullopt, .error = conversion_error("duration", value)};
    }

    return {.value = std::chrono::duration_cast<Duration>(duration), .error = {}};
}

} // namespace jb::core
