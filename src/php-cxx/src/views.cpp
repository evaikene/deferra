#include <phpcxx/views.hpp>

#include <string_view>

namespace php {

auto StringView::icompare(std::string_view other) const noexcept -> int
{
    // Zend's binary comparison uses ASCII folding and lengths, including bytes after NUL.
    return zend_binary_strcasecmp(data(), size(), other.empty() ? "" : other.data(), other.size());
}

auto StringView::istarts_with(std::string_view prefix) const noexcept -> bool
{
    if (prefix.size() > size()) {
        return false;
    }
    return zend_binary_strcasecmp(data(), prefix.size(), prefix.empty() ? "" : prefix.data(), prefix.size()) == 0;
}

auto StringView::iends_with(std::string_view suffix) const noexcept -> bool
{
    // Check the length before subtracting it to locate the borrowed suffix bytes.
    if (suffix.size() > size()) {
        return false;
    }
    auto const* start = data() + (size() - suffix.size());
    return zend_binary_strcasecmp(start, suffix.size(), suffix.empty() ? "" : suffix.data(), suffix.size()) == 0;
}

auto ValueView::is_truthy() const -> bool
{
    if (!_value) {
        return false;
    }

    // Older Zend APIs take a mutable slot. This shallow argument copy borrows the
    // payload without retaining it and must not be passed to zval_ptr_dtor().
    zval borrowed{};
    ZVAL_COPY_VALUE(&borrowed, _value);
    return zend_is_true(&borrowed) != 0;
}

auto ArrayView::find(zend_long key) const noexcept -> ValueView
{
    return ValueView(_array ? zend_hash_index_find(_array, static_cast<zend_ulong>(key)) : nullptr);
}

auto ArrayView::find(std::string_view key) const noexcept -> ValueView
{
    // Use PHP's symtable lookup so canonical integer strings address integer keys.
    return ValueView(_array ? zend_symtable_str_find(_array, key.empty() ? "" : key.data(), key.size()) : nullptr);
}

auto ArrayView::operator[](zend_long key) const noexcept -> ValueView
{
    return find(key);
}

auto ArrayView::operator[](std::string_view key) const noexcept -> ValueView
{
    return find(key);
}

auto ArrayView::contains(zend_long key) const noexcept -> bool
{
    return find(key).get() != nullptr;
}

auto ArrayView::contains(std::string_view key) const noexcept -> bool
{
    return find(key).get() != nullptr;
}

auto ArrayView::begin() const noexcept -> Iterator
{
    HashPosition position = 0;
    if (_array) {
        zend_hash_internal_pointer_reset_ex(_array, &position);
    }
    return {_array, position};
}

auto ArrayView::end() const noexcept -> Iterator
{
    return {_array, _array ? _array->nNumUsed : 0};
}

auto ArrayView::cbegin() const noexcept -> Iterator
{
    return begin();
}

auto ArrayView::cend() const noexcept -> Iterator
{
    return end();
}

auto ArrayView::Iterator::operator*() const noexcept -> Entry
{
    // Zend handles packed and mixed layouts, preserving signed integer key bits.
    auto         position = _position;
    zend_string* string   = nullptr;
    zend_ulong   index    = 0;
    auto const   key_type = zend_hash_get_current_key_ex(_array, &string, &index, &position);
    auto const   value    = ValueView(zend_hash_get_current_data_ex(_array, &position));
    if (key_type == HASH_KEY_IS_STRING) {
        return {.key = StringView(string), .value = value};
    }
    return {.key = static_cast<zend_long>(index), .value = value};
}

auto ArrayView::Iterator::operator++() noexcept -> Iterator&
{
    zend_hash_move_forward_ex(_array, &_position);
    return *this;
}

auto ArrayView::Iterator::operator++(int) noexcept -> Iterator
{
    auto previous = *this;
    ++*this;
    return previous;
}

} // namespace php
