#include "utils.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>

#if defined(__unix__) || defined(__APPLE__)
#  include <glob.h>
#endif

namespace jb::core {

namespace {

constexpr int kSecsInMin  = 60;
constexpr int kSecsInHour = 60 * kSecsInMin;
constexpr int kSecsInDay  = 24 * kSecsInHour;

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
    double      parsed{};
    auto const* first  = value.data();
    auto const* last   = value.data() + value.size();
    auto const  result = std::from_chars(first, last, parsed);
    if (result.ec != std::errc{} || result.ptr != last || !std::isfinite(parsed)) {
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

auto has_glob_pattern(std::string_view path) -> bool
{
    return path.find_first_of("*?[") != std::string_view::npos;
}

auto expand_glob_paths(std::filesystem::path const& pattern) -> ValueResult<std::vector<std::filesystem::path>>
{
#if defined(__unix__) || defined(__APPLE__)
    glob_t     glob_result{};
    auto const result = glob(pattern.c_str(), 0, nullptr, &glob_result);
    if (result == GLOB_NOMATCH) {
        globfree(&glob_result);
        return {.value = std::vector<std::filesystem::path>{}, .error = {}};
    }
    if (result != 0) {
        globfree(&glob_result);
        return {.value = std::nullopt, .error = "failed to expand glob pattern: " + pattern.string()};
    }

    std::vector<std::filesystem::path> paths;
    paths.reserve(glob_result.gl_pathc);
    for (std::size_t i = 0; i < glob_result.gl_pathc; ++i) {
        paths.emplace_back(glob_result.gl_pathv[i]);
    }
    globfree(&glob_result);

    std::ranges::sort(paths);
    return {.value = std::move(paths), .error = {}};
#else
    return {.value = std::nullopt, .error = "glob expansion is not implemented on this platform: " + pattern.string()};
#endif
}

} // namespace jb::core
