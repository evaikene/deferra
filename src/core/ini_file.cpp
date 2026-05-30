#include "ini_file.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <fstream>
#include <system_error>

namespace jb::core {

namespace {

auto make_error(std::size_t line_number, std::string_view message) -> std::string
{
    return fmt::format("line {}: {}", line_number, message);
}

struct ParsedLine {
    std::string_view key;
    std::string_view value;
};

auto file_path(std::filesystem::path const& path) -> std::filesystem::path
{
    std::error_code error;
    auto const      absolute_path = std::filesystem::absolute(path, error);
    auto const&     base          = error ? path : absolute_path;
    return base.lexically_normal();
}

auto identity_path(std::filesystem::path const& path) -> std::filesystem::path
{
    std::error_code error;
    auto const      canonical_path = std::filesystem::weakly_canonical(path, error);
    return error ? path.lexically_normal() : canonical_path;
}

auto parse_line(std::string_view line, std::size_t line_number, ParsedLine& parsed, std::string& error) -> bool
{
    line = trim_ascii_whitespace(line);
    if (line.empty() || line.front() == '#' || line.front() == ';') {
        return true;
    }

    auto const separator = line.find('=');
    if (separator == std::string_view::npos) {
        error = make_error(line_number, "expected key=value");
        return false;
    }

    auto const key = trim_ascii_whitespace(line.substr(0, separator));
    if (key.empty()) {
        error = make_error(line_number, "empty key");
        return false;
    }

    auto raw_value = trim_ascii_whitespace(line.substr(separator + 1));
    if (!raw_value.empty() && (raw_value.front() == '\'' || raw_value.front() == '"')) {
        auto const quote = raw_value.front();
        if (raw_value.size() < 2 || raw_value.back() != quote) {
            error = make_error(line_number, "unterminated quoted value");
            return false;
        }
        raw_value.remove_prefix(1);
        raw_value.remove_suffix(1);
    }

    parsed.key   = key;
    parsed.value = raw_value;
    return true;
}

auto include_error(std::filesystem::path const& file, std::string_view error) -> std::string
{
    return fmt::format("{}: {}", file.string(), error);
}

auto include_paths(std::filesystem::path const&        current_file,
                   std::string_view                    include_value,
                   std::vector<std::filesystem::path>& paths,
                   std::string&                        error) -> bool
{
    auto include_path = std::filesystem::path{include_value};
    if (include_path.is_relative()) {
        include_path = current_file.parent_path() / include_path;
    }

    if (has_glob_pattern(include_value)) {
        auto expanded = expand_glob_paths(include_path);
        if (!expanded) {
            error = std::move(expanded.error);
            return false;
        }

        paths = std::move(*expanded.value);
        return true;
    }

    paths.emplace_back(std::move(include_path));
    return true;
}

auto conversion_error(std::string_view key, std::string_view type, std::string_view value) -> std::string
{
    return fmt::format("value for '{}' cannot be converted to {}: '{}'", key, type, value);
}

template <typename T>
auto missing_error(std::string_view key) -> IniValueResult<T>
{
    return {.value = std::nullopt, .error = fmt::format("missing key: '{}'", key)};
}

template <typename T>
auto ini_conversion_result(std::string_view key, std::string_view type, std::string_view value, ValueResult<T> result)
    -> IniValueResult<T>
{
    if (!result) {
        return {.value = std::nullopt, .error = conversion_error(key, type, value)};
    }
    return result;
}

} // anonymous namespace

IniFile::IniFile(std::filesystem::path const& path)
{
    std::vector<std::filesystem::path> include_stack;
    parse(path, include_stack);
}

auto IniFile::contains(std::string_view key) const -> bool
{
    return find(key) != end(); // NOLINT(readability-container-contains) this is not a container
}

auto IniFile::find(std::string_view key) const -> const_iterator
{
    return _values.find(std::string{key});
}

auto IniFile::values(std::string_view key) const -> std::vector<std::string> const*
{
    auto const it = find(key);
    return it == end() ? nullptr : &it->second;
}

auto IniFile::value(std::string_view key) const -> std::optional<std::string_view>
{
    auto const* vals = values(key);
    if (!vals || vals->empty()) {
        return std::nullopt;
    }
    return vals->back();
}

auto IniFile::value_or(std::string_view key, std::string_view default_value) const -> std::string
{
    auto const val = value(key);
    return val ? std::string{*val} : std::string{default_value};
}

auto IniFile::integer(std::string_view key) const -> IniValueResult<long long>
{
    auto const val = value(key);
    if (!val) {
        return missing_error<long long>(key);
    }
    return ini_conversion_result(key, "integer", *val, parse_integer(*val));
}

auto IniFile::integer_or(std::string_view key, long long default_value) const -> IniValueResult<long long>
{
    auto const val = value(key);
    return val ? ini_conversion_result(key, "integer", *val, parse_integer(*val))
               : IniValueResult<long long>{.value = default_value, .error = {}};
}

auto IniFile::floating_point(std::string_view key) const -> IniValueResult<double>
{
    auto const val = value(key);
    if (!val) {
        return missing_error<double>(key);
    }
    return ini_conversion_result(key, "floating point", *val, parse_floating_point(*val));
}

auto IniFile::floating_point_or(std::string_view key, double default_value) const -> IniValueResult<double>
{
    auto const val = value(key);
    return val ? ini_conversion_result(key, "floating point", *val, parse_floating_point(*val))
               : IniValueResult<double>{.value = default_value, .error = {}};
}

auto IniFile::boolean(std::string_view key) const -> IniValueResult<bool>
{
    auto const val = value(key);
    if (!val) {
        return missing_error<bool>(key);
    }
    return {.value = parse_boolean(*val), .error = {}};
}

auto IniFile::boolean_or(std::string_view key, bool default_value) const -> IniValueResult<bool>
{
    auto const val = value(key);
    return {.value = val ? parse_boolean(*val) : default_value, .error = {}};
}

auto IniFile::interval(std::string_view key) const -> IniValueResult<Duration>
{
    auto const val = value(key);
    if (!val) {
        return missing_error<Duration>(key);
    }
    return ini_conversion_result(key, "time interval", *val, parse_duration(*val));
}

auto IniFile::interval_or(std::string_view key, Duration default_value) const -> IniValueResult<Duration>
{
    auto const val = value(key);
    return val ? ini_conversion_result(key, "time interval", *val, parse_duration(*val))
               : IniValueResult<Duration>{.value = default_value, .error = {}};
}

auto IniFile::parse(std::filesystem::path const& path, std::vector<std::filesystem::path>& include_stack) -> bool
{
    auto const current_file = file_path(path);
    auto const current_id   = identity_path(current_file);
    if (std::ranges::find(include_stack, current_id) != include_stack.end()) {
        _error = fmt::format("recursive INI include: {}", current_file.string());
        return false;
    }

    std::ifstream file{current_file};
    if (!file) {
        _error = fmt::format("failed to open INI file: {}", current_file.string());
        return false;
    }

    include_stack.push_back(current_id);

    std::string line;
    std::size_t line_number = 0;
    while (std::getline(file, line)) {
        ++line_number;
        ParsedLine parsed;
        if (!parse_line(line, line_number, parsed, _error)) {
            _error = include_error(current_file, _error);
            include_stack.pop_back();
            return false;
        }
        if (parsed.key.empty()) {
            continue;
        }
        if (parsed.key == "include") {
            if (parsed.value.empty()) {
                continue;
            }

            std::vector<std::filesystem::path> includes;
            if (!include_paths(current_file, parsed.value, includes, _error)) {
                _error = include_error(current_file, _error);
                include_stack.pop_back();
                return false;
            }
            for (auto const& include_path : includes) {
                if (!parse(include_path, include_stack)) {
                    _error = include_error(current_file, _error);
                    include_stack.pop_back();
                    return false;
                }
            }
            continue;
        }

        _values[std::string{parsed.key}].emplace_back(parsed.value);
    }

    if (file.bad()) {
        _error = fmt::format("failed to read INI file: {}", current_file.string());
        include_stack.pop_back();
        return false;
    }

    include_stack.pop_back();
    return true;
}

} // namespace jb::core
