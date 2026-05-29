#pragma once

#include "event_loop_types.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace jb::core {

/// Result of converting a string value to a typed value without exceptions.
template <typename T>
struct ValueResult {
    std::optional<T> value;
    std::string      error;

    explicit operator bool() const noexcept { return value.has_value(); }
};

/// Returns a view with leading and trailing ASCII whitespace removed.
auto trim_ascii_whitespace(std::string_view value) -> std::string_view;

/// Parses a full string as a base-10 signed integer.
auto parse_integer(std::string_view value) -> ValueResult<long long>;

/// Parses a full string locale-independently as a finite floating point value.
auto parse_floating_point(std::string_view value) -> ValueResult<double>;

/// Parses permissive boolean text. y, yes, true, on and 1 are true; all other values are false.
auto parse_boolean(std::string_view value) -> bool;

/// Parses a non-negative duration with a single s, m, h or d suffix.
auto parse_duration(std::string_view value) -> ValueResult<Duration>;

} // namespace jb::core
