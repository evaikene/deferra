#pragma once

#include "event_loop_types.hpp"
#include "utils.hpp"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace jb::core {

/// Result type returned by typed INI accessors.
template <typename T>
using IniValueResult = ValueResult<T>;

/// INI-style configuration file without section support.
///
/// The parser accepts `key = value` lines, `#` and `;` full-line comments, and repeated
/// keys. Repeated keys preserve insertion order; single-value accessors return the last
/// value. Section headers are intentionally unsupported; callers should use dot-namespaced
/// keys such as `queue.priority`.
class IniFile {
public:
    using map_type       = std::map<std::string, std::vector<std::string>>;
    using iterator       = map_type::const_iterator;
    using const_iterator = map_type::const_iterator;

    /// Reads and parses an INI file from `path`.
    ///
    /// File I/O and parse errors are stored in `error()` and reported by `ok()`.
    explicit IniFile(std::filesystem::path const& path);

    /// Returns true when the file was opened and parsed successfully.
    auto ok() const noexcept -> bool { return _error.empty(); }

    /// Returns the file I/O or parse error, or an empty string on success.
    auto error() const noexcept -> std::string_view { return _error; }

    /// Returns true when no keys were parsed.
    auto empty() const noexcept -> bool { return _values.empty(); }

    /// Returns the number of unique parsed keys.
    auto size() const noexcept -> std::size_t { return _values.size(); }

    /// Returns true when `key` exists.
    auto contains(std::string_view key) const -> bool;

    /// Returns an iterator to the first key.
    auto begin() const noexcept -> const_iterator { return _values.begin(); }

    /// Returns the past-the-end iterator.
    auto end() const noexcept -> const_iterator { return _values.end(); }

    /// Finds `key` and returns `end()` when it does not exist.
    auto find(std::string_view key) const -> const_iterator;

    /// Returns all values for `key` in file order, or nullptr when the key is missing.
    auto values(std::string_view key) const -> std::vector<std::string> const*;

    /// Returns the last value for `key`, or std::nullopt when the key is missing.
    auto value(std::string_view key) const -> std::optional<std::string_view>;

    /// Returns the last value for `key`, or `default_value` when the key is missing.
    auto value_or(std::string_view key, std::string_view default_value) const -> std::string;

    /// Returns the last value for `key` converted to an integer.
    auto integer(std::string_view key) const -> IniValueResult<long long>;

    /// Returns the converted integer value, or `default_value` when the key is missing.
    auto integer_or(std::string_view key, long long default_value) const -> IniValueResult<long long>;

    /// Returns the last value for `key` converted to a finite floating point value.
    auto floating_point(std::string_view key) const -> IniValueResult<double>;

    /// Returns the converted floating point value, or `default_value` when the key is missing.
    auto floating_point_or(std::string_view key, double default_value) const -> IniValueResult<double>;

    /// Returns the last value for `key` converted to a boolean.
    ///
    /// y, yes, true, on and 1 are true; all other values are false.
    auto boolean(std::string_view key) const -> IniValueResult<bool>;

    /// Returns the converted boolean value, or `default_value` when the key is missing.
    auto boolean_or(std::string_view key, bool default_value) const -> IniValueResult<bool>;

    /// Returns the last value for `key` converted to a time interval.
    ///
    /// Intervals are non-negative integer values with one suffix: s, m, h or d.
    auto interval(std::string_view key) const -> IniValueResult<Duration>;

    /// Returns the converted interval value, or `default_value` when the key is missing.
    auto interval_or(std::string_view key, Duration default_value) const -> IniValueResult<Duration>;

private:
    auto parse(std::filesystem::path const& path) -> bool;

    map_type    _values;
    std::string _error;
};

} // namespace jb::core
