#pragma once

#include "event_loop_types.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace jb::core {

/// Result of converting a value to a typed value without exceptions.
///
/// @tparam T The type produced by the conversion.
template <typename T>
struct ValueResult {
    /// The converted value, or `std::nullopt` when conversion failed.
    std::optional<T> value;

    /// A descriptive conversion error, or an empty string on success.
    std::string error;

    /// Tests whether conversion produced a value.
    ///
    /// @return `true` when @ref value contains a converted value.
    explicit operator bool() const noexcept { return value.has_value(); }
};

/// Returns a view with leading and trailing ASCII whitespace removed.
///
/// @param[in] value Text to trim.
/// @return A view into `value` without leading or trailing ASCII whitespace.
auto trim_ascii_whitespace(std::string_view value) -> std::string_view;

/// Parses a full string as a base-10 signed integer.
///
/// Leading and trailing ASCII whitespace is ignored, but the entire remaining
/// string must contain a valid integer.
///
/// @param[in] value Text to parse.
/// @return A successful result containing the integer, or an error result.
auto parse_integer(std::string_view value) -> ValueResult<long long>;

/// Parses a full string locale-independently as a finite floating point value.
///
/// Leading and trailing ASCII whitespace is ignored, but the entire remaining
/// string must contain a finite floating point value.
///
/// @param[in] value Text to parse.
/// @return A successful result containing the floating point value, or an error result.
auto parse_floating_point(std::string_view value) -> ValueResult<double>;

/// Parses permissive boolean text. y, yes, true, on and 1 are true; all other values are false.
///
/// Matching is case-insensitive and ignores leading and trailing ASCII
/// whitespace.
///
/// @param[in] value Text to parse.
/// @return `true` for an enabled boolean spelling, otherwise `false`.
auto parse_boolean(std::string_view value) -> bool;

/// Parses a non-negative duration with a single s, m, h or d suffix.
///
/// Leading and trailing ASCII whitespace is ignored. The numeric portion must
/// be a non-negative integer, and compound values such as `1h30m` are not
/// accepted.
///
/// @param[in] value Text to parse.
/// @return A successful result containing the duration, or an error result.
auto parse_duration(std::string_view value) -> ValueResult<Duration>;

/// Returns true when `path` contains wildcard characters used by glob patterns.
///
/// @param[in] path Path or pattern to inspect.
/// @return `true` when `path` contains glob wildcard syntax, otherwise `false`.
auto has_glob_pattern(std::string_view path) -> bool;

/// Expands a glob pattern into sorted filesystem paths.
///
/// On supported platforms, an unmatched pattern returns an empty vector. On unsupported
/// platforms, the result contains an error instead of paths. Platform-specific globbing
/// is hidden behind this API so callers do not depend on POSIX-only headers or functions.
///
/// @param[in] pattern Filesystem path or glob pattern to expand.
/// @return A successful result containing sorted matching paths, or an error result.
auto expand_glob_paths(std::filesystem::path const& pattern) -> ValueResult<std::vector<std::filesystem::path>>;

} // namespace jb::core
