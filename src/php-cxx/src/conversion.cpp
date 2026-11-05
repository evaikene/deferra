#include <phpcxx/conversion.hpp>
#include <phpcxx/exception.hpp>

extern "C" {
#include <ext/date/php_date.h>
}

#include <cmath>
#include <string>

namespace php {

auto as_string(ValueView value) -> Result<StringView>
{
    value = value.dereference();
    if (!value.is_string()) {
        return Error{.kind = ErrorKind::Type, .message = "must be a string"};
    }
    return value.get_string();
}

auto as_bool(ValueView value) -> Result<bool>
{
    value = value.dereference();
    if (!value.is_bool()) {
        return Error{.kind = ErrorKind::Type, .message = "must be a boolean"};
    }
    return value.get_bool();
}

auto as_integer(ValueView value) -> Result<zend_long>
{
    value = value.dereference();
    if (!value.is_long()) {
        return Error{.kind = ErrorKind::Type, .message = "must be an integer"};
    }
    return value.get_long();
}

auto as_double(ValueView value) -> Result<double>
{
    value = value.dereference();
    if (!value.is_double()) {
        return Error{.kind = ErrorKind::Type, .message = "must be a float"};
    }
    return value.get_double();
}

auto as_array(ValueView value) -> Result<ArrayView>
{
    value = value.dereference();
    if (!value.is_array()) {
        return Error{.kind = ErrorKind::Type, .message = "must be an array"};
    }
    return value.get_array();
}

auto as_object(ValueView value, zend_class_entry const* expected) -> Result<zend_object const*>
{
    value = value.dereference();
    if (!value.is_object() || !instanceof_function(value.get_object()->ce, expected)) {
        return Error{.kind = ErrorKind::Type,
                     .message =
                         "must be an instance of " + std::string(ZSTR_VAL(expected->name), ZSTR_LEN(expected->name))};
    }
    return value.get_object();
}

auto integer_from_double(double value) -> Result<zend_long>
{
    // The rounded double representation of INT64_MAX is already 2^63. Use an
    // exclusive upper bound so the cast never invokes out-of-range behavior.
    constexpr double lower = -9223372036854775808.0;
    constexpr double upper = 9223372036854775808.0;
    if (!std::isfinite(value) || value < lower || value >= upper || std::trunc(value) != value) {
        return Error{.kind = ErrorKind::Value, .message = "must be a finite whole number within the PHP integer range"};
    }
    return static_cast<zend_long>(value);
}

auto double_from_integer(zend_long value) noexcept -> double
{
    return static_cast<double>(value);
}

auto as_timestamp(ValueView value) -> Result<Timestamp>
{
    if (has_exception()) {
        return pending_exception();
    }
    auto object = as_object(value, php_date_get_interface_ce());
    if (!object) {
        return object.error();
    }

    auto*       native = const_cast<zend_object*>(object.value());
    auto const* date   = php_date_obj_from_obj(native);
    if (!date->time) {
        return Error{.kind = ErrorKind::Value, .message = "date object must be initialized"};
    }

    // Invoke the native base implementation, not a subclass override. It updates
    // Zend's cached epoch seconds when necessary and reports invalid native state.
    auto* base   = instanceof_function(native->ce, php_date_get_immutable_ce()) ? php_date_get_immutable_ce()
                                                                                : php_date_get_date_ce();
    auto* method = static_cast<zend_function*>(zend_hash_str_find_ptr(&base->function_table, "gettimestamp", 12));
    zval  seconds{};
    ZVAL_UNDEF(&seconds);
    zend_call_known_instance_method_with_0_params(method, native, &seconds);
    auto owner = Value::adopt(seconds);
    if (has_exception()) {
        return pending_exception();
    }
    auto epoch = as_integer(owner.view());
    if (!epoch) {
        return epoch.error();
    }
    return Timestamp{.seconds = epoch.value(), .microseconds = static_cast<std::int32_t>(date->time->us)};
}

auto datetime_immutable(Timestamp value) -> Result<Value>
{
    if (has_exception()) {
        return pending_exception();
    }
    if (value.microseconds < 0 || value.microseconds >= 1000000) {
        return Error{.kind = ErrorKind::Value, .message = "microseconds must be between 0 and 999999"};
    }

    // U.u uses floor epoch seconds and a positive fractional field, even before
    // the epoch. Explicit UTC avoids inheriting the request's default timezone.
    auto fraction = std::to_string(value.microseconds);
    auto text     = std::to_string(value.seconds) + "." + std::string(6 - fraction.size(), '0') + fraction + " UTC";
    zval slot{};
    ZVAL_UNDEF(&slot);
    php_date_instantiate(php_date_get_immutable_ce(), &slot);
    auto owner = Value::adopt(slot);
    if (has_exception()) {
        return pending_exception();
    }
    auto*      date = php_date_obj_from_obj(const_cast<zend_object*>(owner.view().get_object()));
    auto const initialized =
        php_date_initialize(date, text.data(), text.size(), "U.u e", nullptr, PHP_DATE_INIT_FORMAT);
    if (has_exception()) {
        return pending_exception();
    }
    if (!initialized) {
        return Error{.kind = ErrorKind::Value, .message = "timestamp is outside PHP's supported date range"};
    }

    // Detect parser normalization/overflow rather than returning a different instant.
    auto roundtrip = as_timestamp(owner.view());
    if (!roundtrip) {
        return roundtrip.error();
    }
    if (roundtrip.value().seconds != value.seconds || roundtrip.value().microseconds != value.microseconds) {
        return Error{.kind = ErrorKind::Value, .message = "timestamp is outside PHP's supported date range"};
    }
    return owner;
}

auto unix_microseconds(Timestamp value) -> Result<std::int64_t>
{
    if (value.microseconds < 0 || value.microseconds >= 1000000) {
        return Error{.kind = ErrorKind::Value, .message = "microseconds must be between 0 and 999999"};
    }
    constexpr auto minimum = std::numeric_limits<std::int64_t>::min();
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    auto const     lower   = timestamp_from_microseconds(minimum);
    auto const     upper   = timestamp_from_microseconds(maximum);
    if (value.seconds < lower.seconds || value.seconds > upper.seconds ||
        (value.seconds == lower.seconds && value.microseconds < lower.microseconds) ||
        (value.seconds == upper.seconds && value.microseconds > upper.microseconds)) {
        return Error{.kind = ErrorKind::Value, .message = "timestamp is outside the signed microsecond range"};
    }

    // At the negative boundary, seconds*1e6 alone would underflow. Shift one
    // second toward zero before multiplication, then add the negative remainder.
    if (value.seconds < 0) {
        return ((value.seconds + 1) * 1000000) + (value.microseconds - 1000000);
    }
    return (value.seconds * 1000000) + value.microseconds;
}

auto timestamp_from_microseconds(std::int64_t value) noexcept -> Timestamp
{
    auto seconds  = value / 1000000;
    auto fraction = value % 1000000;
    if (fraction < 0) {
        --seconds;
        fraction += 1000000;
    }
    return {.seconds = seconds, .microseconds = static_cast<std::int32_t>(fraction)};
}

} // namespace php
