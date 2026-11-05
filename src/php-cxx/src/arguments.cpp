#include <phpcxx/arguments.hpp>
#include <phpcxx/conversion.hpp>
#include <phpcxx/exception.hpp>

#include <cassert>
#include <string>

namespace php {

auto Arguments::size() const noexcept -> std::uint32_t
{
    return ZEND_CALL_NUM_ARGS(_frame);
}

auto Arguments::get(std::uint32_t index) const noexcept -> ValueView
{
    return ValueView(index > 0 && index <= size() ? ZEND_CALL_ARG(_frame, index) : nullptr);
}

auto Arguments::count(std::uint32_t minimum, std::uint32_t maximum) const -> Result<void>
{
    assert(minimum <= maximum);
    if (has_exception()) {
        return pending_exception();
    }
    if (size() < minimum || size() > maximum) {
        return Error{.kind    = ErrorKind::ArgumentCount,
                     .message = "Expected " + std::to_string(minimum) + " to " + std::to_string(maximum) +
                                " arguments, " + std::to_string(size()) + " given"};
    }
    return {};
}

auto Arguments::slot(std::uint32_t index) const -> Result<zval*>
{
    assert(_frame == EG(current_execute_data));
    if (has_exception()) {
        return pending_exception();
    }
    if (index == 0 || index > size() || get(index).is_undef()) {
        return Error{.kind = ErrorKind::ArgumentCount, .message = "Required argument is missing", .argument = index};
    }
    auto* value = ZEND_CALL_ARG(_frame, index);
    ZVAL_DEREF(value);
    if (Z_TYPE_P(value) == IS_NULL) {
        return Error{.kind = ErrorKind::Type, .message = "must not be null", .argument = index};
    }
    return value;
}

auto Arguments::string(std::uint32_t index) const -> Result<StringView>
{
    auto value = slot(index);
    if (!value) {
        return value.error();
    }
    zend_string* result = nullptr;
    auto const   parsed = zend_parse_arg_str(value.value(), &result, false, index);
    if (has_exception()) {
        return pending_exception();
    }
    if (!parsed) {
        return Error{.kind = ErrorKind::Type, .message = "must be of type string", .argument = index};
    }
    return StringView(result);
}

auto Arguments::boolean(std::uint32_t index) const -> Result<bool>
{
    auto value = slot(index);
    if (!value) {
        return value.error();
    }
    bool       result = false;
    auto const parsed = zend_parse_arg_bool(value.value(), &result, nullptr, false, index);
    if (has_exception()) {
        return pending_exception();
    }
    if (!parsed) {
        return Error{.kind = ErrorKind::Type, .message = "must be of type bool", .argument = index};
    }
    return result;
}

auto Arguments::integer(std::uint32_t index) const -> Result<zend_long>
{
    auto value = slot(index);
    if (!value) {
        return value.error();
    }
    zend_long  result = 0;
    auto const parsed = zend_parse_arg_long(value.value(), &result, nullptr, false, index);
    if (has_exception()) {
        return pending_exception();
    }
    if (!parsed) {
        return Error{.kind = ErrorKind::Type, .message = "must be of type int", .argument = index};
    }
    return result;
}

auto Arguments::floating(std::uint32_t index) const -> Result<double>
{
    auto value = slot(index);
    if (!value) {
        return value.error();
    }
    double     result = 0;
    auto const parsed = zend_parse_arg_double(value.value(), &result, nullptr, false, index);
    if (has_exception()) {
        return pending_exception();
    }
    if (!parsed) {
        return Error{.kind = ErrorKind::Type, .message = "must be of type float", .argument = index};
    }
    return result;
}

auto Arguments::array(std::uint32_t index) const -> Result<ArrayView>
{
    auto value = slot(index);
    if (!value) {
        return value.error();
    }
    auto result = as_array(ValueView(value.value()));
    if (!result) {
        auto error     = result.error();
        error.argument = index;
        return error;
    }
    return result;
}

auto Arguments::object(std::uint32_t index, zend_class_entry const* expected) const -> Result<zend_object const*>
{
    auto value = slot(index);
    if (!value) {
        return value.error();
    }
    auto result = as_object(ValueView(value.value()), expected);
    if (!result) {
        auto error     = result.error();
        error.argument = index;
        return error;
    }
    return result;
}

} // namespace php
