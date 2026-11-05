#pragma once

#include <phpcxx/result.hpp>
#include <phpcxx/value.hpp>
#include <phpcxx/views.hpp>

#include <cstdint>
#include <limits>
#include <type_traits>

namespace php {

/// Checked exact-type extraction; references are dereferenced without modifying them.
/// Missing/undefined/null values fail. These operations do not execute PHP or alter
/// pending exceptions. Borrowed results share the backing value/request lifetime.
auto as_string(ValueView value) -> Result<StringView>;

/// Extract an exact PHP boolean, without truthiness conversion.
auto as_bool(ValueView value) -> Result<bool>;

/// Extract an exact PHP integer, without parsing numeric strings.
auto as_integer(ValueView value) -> Result<zend_long>;

/// Extract an exact PHP double, preserving NaN/infinity; does not widen integers.
auto as_double(ValueView value) -> Result<double>;

/// Borrow an exact PHP array; no separation, duplication, or retain.
auto as_array(ValueView value) -> Result<ArrayView>;

/// Borrow an object of the requested class/interface, accepting subclasses.
/// expected must be a live class entry in the current PHP runtime.
auto as_object(ValueView value, zend_class_entry const* expected) -> Result<zend_object const*>;

/// Convert a PHP integer to a non-bool integral type, checking range and signedness.
template <typename T>
auto checked_integer(zend_long value) -> Result<T>
{
    static_assert(std::is_integral_v<T> && !std::is_same_v<T, bool>);
    if constexpr (std::is_unsigned_v<T>) {
        if (value < 0 || static_cast<zend_ulong>(value) > std::numeric_limits<T>::max()) {
            return Error{.kind = ErrorKind::Value, .message = "must be within the target integer range"};
        }
    }
    else if constexpr (sizeof(T) < sizeof(zend_long)) {
        if (value < std::numeric_limits<T>::min() || value > std::numeric_limits<T>::max()) {
            return Error{.kind = ErrorKind::Value, .message = "must be within the target integer range"};
        }
    }
    return static_cast<T>(value);
}

/// Explicit numeric conversion; rejects fractional, non-finite, or out-of-range input.
auto integer_from_double(double value) -> Result<zend_long>;

/// Explicit widening, following PHP float precision (large integers may round).
auto double_from_integer(zend_long value) noexcept -> double;

/// An instant represented as floor Unix seconds plus microseconds in [0, 999999].
/// For example, {-1, 500000} is half a second before the Unix epoch.
struct Timestamp {
    std::int64_t seconds;
    std::int32_t microseconds;
};

/// Read the native state of DateTimeInterface, including DateTime/DateTimeImmutable
/// subclasses, without invoking overridden methods. Uninitialized objects fail.
/// Preserves existing PHP exceptions; failures from PHP remain pending.
auto as_timestamp(ValueView value) -> Result<Timestamp>;

/// Create an owned UTC DateTimeImmutable, preserving integer/microsecond precision.
/// Requires normalized microseconds and a PHP-representable date. May update PHP's
/// date parser last-error state. Preserves pending PHP exceptions and returns failure.
auto datetime_immutable(Timestamp value) -> Result<Value>;

/// Convert to signed Unix microseconds, rejecting normalization/range errors.
auto unix_microseconds(Timestamp value) -> Result<std::int64_t>;

/// Split signed Unix microseconds with floor-second normalization, including negatives.
auto timestamp_from_microseconds(std::int64_t value) noexcept -> Timestamp;

} // namespace php
