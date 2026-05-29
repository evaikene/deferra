#include "ini_file.hpp"

#include <fstream>

namespace jb::core {

namespace {

auto make_error(std::size_t line_number, std::string_view message) -> std::string
{
    return "line " + std::to_string(line_number) + ": " + std::string{message};
}

auto parse_line(std::string_view line, std::size_t line_number, IniFile::map_type& values, std::string& error) -> bool
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

    values[std::string{key}].emplace_back(raw_value);
    return true;
}

auto conversion_error(std::string_view key, std::string_view type, std::string_view value) -> std::string
{
    return "value for '" + std::string{key} + "' cannot be converted to " + std::string{type} + ": '" +
           std::string{value} + "'";
}

template <typename T>
auto missing_error(std::string_view key) -> IniValueResult<T>
{
    return {.value = std::nullopt, .error = "missing key: '" + std::string{key} + "'"};
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
    parse(path);
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

auto IniFile::parse(std::filesystem::path const& path) -> bool
{
    std::ifstream file{path};
    if (!file) {
        _error = "failed to open INI file: " + path.string();
        return false;
    }

    std::string line;
    std::size_t line_number = 0;
    while (std::getline(file, line)) {
        ++line_number;
        if (!parse_line(line, line_number, _values, _error)) {
            return false;
        }
    }

    if (file.bad()) {
        _error = "failed to read INI file: " + path.string();
        return false;
    }

    return true;
}

} // namespace jb::core
