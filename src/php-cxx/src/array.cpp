#include <phpcxx/array.hpp>

#include <utility>

namespace php {

namespace {

auto release_array_reference(zend_array* array) noexcept -> void
{
    if (array && (GC_FLAGS(array) & IS_ARRAY_IMMUTABLE) == 0) {
        // A plain zend_array_release only decrements the count. The zval path
        // also nominates surviving cycles as possible roots for Zend's collector.
        zval value{};
        ZVAL_ARR(&value, array);
        zval_ptr_dtor(&value);
    }
}

} // namespace

Array::~Array() noexcept
{
    release_array_reference(_array);
}

Array::Array(Array&& other) noexcept
    : _array(other.release())
{}

auto Array::operator=(Array&& other) noexcept -> Array&
{
    if (this != &other) {
        // Publish the replacement before releasing elements that may invoke PHP cleanup.
        auto* previous = std::exchange(_array, other.release());
        release_array_reference(previous);
    }
    return *this;
}

auto Array::create() -> Array
{
    return adopt(zend_new_array(0));
}

auto Array::adopt(zend_array* array) noexcept -> Array
{
    Array result;
    result._array = array;
    return result;
}

auto Array::retain(ArrayView array) noexcept -> Array
{
    if (array._array) {
        GC_TRY_ADDREF(array._array);
    }
    return adopt(array._array);
}

auto Array::duplicate() const -> Array
{
    return adopt(_array ? zend_array_dup(_array) : nullptr);
}

auto Array::release() noexcept -> zend_array*
{
    return std::exchange(_array, nullptr);
}

auto Array::writable() & -> zend_array*
{
    if (_array && ((GC_FLAGS(_array) & IS_ARRAY_IMMUTABLE) != 0 || GC_REFCOUNT(_array) > 1)) {
        // Immutable arrays cannot be written even if their stored refcount looks unique.
        auto* previous = _array;
        _array         = zend_array_dup(previous);
        release_array_reference(previous);
    }
    return _array;
}

} // namespace php
