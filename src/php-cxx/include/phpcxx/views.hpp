#pragma once

#include <phpcxx/zend.hpp> // IWYU pragma: export

#include <cstddef>
#include <string_view>

namespace php {

/// Borrowed, read-only PHP string. Copying a view does not change reference counts.
///
/// The string must remain alive on its owning request/thread. Destruction or mutation
/// of its backing storage invalidates the view, including any returned bytes().
class StringView {
public:
    explicit StringView(zend_string* string = nullptr) noexcept
        : _string(string)
    {}

    /// Whether a PHP string is present; a valid zero-length string is present.
    explicit operator bool() const noexcept { return _string != nullptr; }

    auto get() const noexcept -> zend_string const* { return _string; }

    /// Binary-safe bytes; an absent view returns an empty string view.
    auto bytes() const noexcept -> std::string_view
    {
        return _string ? std::string_view(ZSTR_VAL(_string), ZSTR_LEN(_string)) : std::string_view{};
    }

private:
    friend class String;
    zend_string* _string;
};

/// Borrowed, read-only regular PHP array (not an arbitrary or persistent HashTable).
///
/// Copying a view does not retain the array. Keep its backing owner/request alive
/// on the same thread. Reacquire views after separation or mutation; element views
/// can also be invalidated by HashTable reallocation or replacement.
class ArrayView {
public:
    explicit ArrayView(zend_array* array = nullptr) noexcept
        : _array(array)
    {}

    explicit operator bool() const noexcept { return _array != nullptr; }

    auto get() const noexcept -> zend_array const* { return _array; }

    auto size() const noexcept -> std::size_t { return _array ? zend_hash_num_elements(_array) : 0; }

private:
    friend class Array;
    zend_array* _array;
};

/// Borrowed, read-only zval slot. Copying a view never retains or dereferences it.
///
/// The slot and its value must remain alive on their owning request/thread.
/// Moving, releasing, replacing, or destroying the owning Value invalidates its
/// views. Array element views additionally depend on the array's mutation rules.
class ValueView {
public:
    explicit ValueView(zval const* value = nullptr) noexcept
        : _value(value)
    {}

    auto get() const noexcept -> zval const* { return _value; }

    /// An absent view and an IS_UNDEF slot both report IS_UNDEF.
    auto type() const noexcept -> int { return _value ? Z_TYPE_P(_value) : IS_UNDEF; }

    auto is_initialized() const noexcept -> bool { return type() != IS_UNDEF; }

    auto is_null() const noexcept -> bool { return type() == IS_NULL; }

private:
    zval const* _value;
};

} // namespace php
