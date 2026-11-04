#include <phpcxx/value.hpp>

namespace php {

Value::~Value() noexcept
{
    zval_ptr_dtor(&_value);
}

Value::Value(Value&& other) noexcept
    : _value(other.release())
{}

auto Value::operator=(Value&& other) noexcept -> Value&
{
    if (this != &other) {
        // Install the new value before old-value destruction can reenter PHP code.
        zval previous = release();
        _value        = other.release();
        zval_ptr_dtor(&previous);
    }
    return *this;
}

auto Value::null() noexcept -> Value
{
    Value result;
    ZVAL_NULL(&result._value);
    return result;
}

auto Value::adopt(zval& value) noexcept -> Value
{
    Value result;
    ZVAL_COPY_VALUE(&result._value, &value);
    ZVAL_UNDEF(&value);
    return result;
}

auto Value::retain(ValueView value) noexcept -> Value
{
    Value result;
    if (value.get()) {
        ZVAL_COPY(&result._value, value.get());
    }
    return result;
}

auto Value::duplicate() const -> Value
{
    Value result;
    ZVAL_DUP(&result._value, &_value);
    return result;
}

auto Value::release() noexcept -> zval
{
    zval result{};
    ZVAL_COPY_VALUE(&result, &_value);
    ZVAL_UNDEF(&_value);
    return result;
}

} // namespace php
