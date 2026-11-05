#pragma once

/// @file
/// Borrowed read-only Zend access. See zend.hpp for common integration/lifetime rules.

#include <phpcxx/zend.hpp> // IWYU pragma: export

#include <cstddef>
#include <iterator>
#include <string_view>
#include <variant>

namespace php {

/// Borrowed, read-only PHP string. Copying a view does not change reference counts.
///
/// The string must remain alive on its owning request/thread. Destruction or mutation
/// of its backing storage invalidates the view, including any returned bytes().
/// All character access, comparisons, and searches operate on bytes, including NULs;
/// there is no encoding validation. Case-sensitive ordering compares unsigned bytes.
/// Methods prefixed with i fold ASCII A-Z only, independently of locale.
/// An absent view behaves as an empty byte string for comparison/search/iteration,
/// but operator bool() distinguishes it from a present empty PHP string. Returned
/// pointers/references/iterators do not retain storage and must never be used to mutate it.
class StringView {
public:
    /// Read-only contiguous byte iterator, borrowing the string's storage.
    using const_iterator = char const*;

    /// Reverse byte iterator with the same borrowed lifetime.
    using const_reverse_iterator = std::reverse_iterator<const_iterator>;

    /// Search failure sentinel; as an rfind() position, requests searching from the end.
    static constexpr auto npos = std::string_view::npos;

    /// Borrow a live Zend string without retaining it; nullptr constructs an absent view.
    explicit StringView(zend_string* string = nullptr) noexcept
        : _string(string)
    {}

    /// Whether a PHP string is present; a valid zero-length string is present.
    explicit operator bool() const noexcept { return _string != nullptr; }

    /// Borrow the underlying read-only Zend pointer, or nullptr when absent; never release it.
    auto get() const noexcept -> zend_string const* { return _string; }

    /// Binary-safe bytes; an absent view returns an empty string view.
    auto bytes() const noexcept -> std::string_view
    {
        return _string ? std::string_view(ZSTR_VAL(_string), ZSTR_LEN(_string)) : std::string_view{};
    }

    /// Zero-terminated borrowed storage, or "" for an absent view. Embedded NULs are preserved.
    auto data() const noexcept -> char const* { return _string ? ZSTR_VAL(_string) : ""; }

    /// Return data(): zero-terminated borrowed bytes, or "" when absent.
    auto c_str() const noexcept -> char const* { return data(); }

    /// Byte count excluding the trailing terminator; zero when absent.
    auto size() const noexcept -> std::size_t { return _string ? ZSTR_LEN(_string) : 0; }

    /// Same byte count as size(); does not count Unicode characters.
    auto length() const noexcept -> std::size_t { return size(); }

    /// Whether size() is zero, for both absent and present empty strings.
    auto empty() const noexcept -> bool { return size() == 0; }

    /// Unchecked character access; index must be less than size().
    auto operator[](std::size_t index) const noexcept -> char const& { return data()[index]; }

    /// Borrow the first byte; the view must be nonempty.
    auto front() const noexcept -> char const& { return (*this)[0]; }

    /// Borrow the last byte before the terminator; the view must be nonempty.
    auto back() const noexcept -> char const& { return (*this)[size() - 1]; }

    /// First byte iterator; equals end() when empty/absent. Excludes the trailing terminator.
    auto begin() const noexcept -> const_iterator { return data(); }

    /// Past-the-last-byte iterator; must not be dereferenced as an element iterator.
    auto end() const noexcept -> const_iterator { return data() + size(); }

    /// Read-only begin(); has the same storage lifetime.
    auto cbegin() const noexcept -> const_iterator { return begin(); }

    /// Read-only end(); has the same storage lifetime.
    auto cend() const noexcept -> const_iterator { return end(); }

    /// Reverse iterator at the last byte; equals rend() when empty/absent.
    auto rbegin() const noexcept -> const_reverse_iterator { return const_reverse_iterator(end()); }

    /// Reverse past-the-end iterator; must not be dereferenced.
    auto rend() const noexcept -> const_reverse_iterator { return const_reverse_iterator(begin()); }

    /// Binary lexicographic comparison; returns negative, zero, or positive.
    auto compare(std::string_view other) const noexcept -> int { return bytes().compare(other); }

    /// Compare other's bytes with compare(std::string_view); absence compares as empty.
    auto compare(StringView other) const noexcept -> int { return compare(other.bytes()); }

    /// Binary-safe, locale-independent comparison folding only ASCII A-Z to a-z.
    /// Bytes outside ASCII are compared unchanged; this is not Unicode case folding.
    /// Returns negative, zero, or positive for less than, equal to, or greater than other.
    auto icompare(std::string_view other) const noexcept -> int;

    /// Compare other's bytes with icompare(std::string_view); absence compares as empty.
    auto icompare(StringView other) const noexcept -> int { return icompare(other.bytes()); }

    /// Whether icompare(other) is zero, including equal lengths and bytes after NULs.
    auto iequals(std::string_view other) const noexcept -> bool { return icompare(other) == 0; }

    /// ASCII-folded equality of both views' bytes; absence compares as empty.
    auto iequals(StringView other) const noexcept -> bool { return icompare(other) == 0; }

    /// Case-sensitive prefix match; empty prefix always matches, oversized prefix never matches.
    auto starts_with(std::string_view prefix) const noexcept -> bool { return bytes().starts_with(prefix); }

    /// Match prefix's bytes with starts_with(std::string_view); an absent prefix is empty.
    auto starts_with(StringView prefix) const noexcept -> bool { return starts_with(prefix.bytes()); }

    /// Whether the first byte equals prefix; false when this view is empty/absent.
    auto starts_with(char prefix) const noexcept -> bool { return !empty() && front() == prefix; }

    /// Binary-safe ASCII case-insensitive prefix match; an empty prefix always matches.
    /// A prefix longer than size() never matches.
    auto istarts_with(std::string_view prefix) const noexcept -> bool;

    /// Match prefix's bytes with istarts_with(std::string_view); an absent prefix is empty.
    auto istarts_with(StringView prefix) const noexcept -> bool { return istarts_with(prefix.bytes()); }

    /// ASCII-folded first-byte match; false when this view is empty/absent.
    auto istarts_with(char prefix) const noexcept -> bool { return istarts_with(std::string_view(&prefix, 1)); }

    /// Case-sensitive suffix match; empty suffix always matches, oversized suffix never matches.
    auto ends_with(std::string_view suffix) const noexcept -> bool { return bytes().ends_with(suffix); }

    /// Match suffix's bytes with ends_with(std::string_view); an absent suffix is empty.
    auto ends_with(StringView suffix) const noexcept -> bool { return ends_with(suffix.bytes()); }

    /// Whether the last byte equals suffix; false when this view is empty/absent.
    auto ends_with(char suffix) const noexcept -> bool { return !empty() && back() == suffix; }

    /// Binary-safe ASCII case-insensitive suffix match; an empty suffix always matches.
    /// A suffix longer than size() never matches.
    auto iends_with(std::string_view suffix) const noexcept -> bool;

    /// Match suffix's bytes with iends_with(std::string_view); an absent suffix is empty.
    auto iends_with(StringView suffix) const noexcept -> bool { return iends_with(suffix.bytes()); }

    /// ASCII-folded last-byte match; false when this view is empty/absent.
    auto iends_with(char suffix) const noexcept -> bool { return iends_with(std::string_view(&suffix, 1)); }

    /// Case-sensitive substring membership; an empty needle always matches, even when absent.
    auto contains(std::string_view needle) const noexcept -> bool { return bytes().find(needle) != npos; }

    /// Search needle's bytes with contains(std::string_view); an absent needle is empty.
    auto contains(StringView needle) const noexcept -> bool { return contains(needle.bytes()); }

    /// Whether any byte equals needle; false when empty/absent. The terminator is not searched.
    auto contains(char needle) const noexcept -> bool { return bytes().find(needle) != npos; }

    /// Return the first case-sensitive match's byte index at or after position, or npos.
    /// An empty needle matches at position when position <= size(); position > size()
    /// always returns npos. Embedded NULs participate; the trailing terminator does not.
    auto find(std::string_view needle, std::size_t position = 0) const noexcept -> std::size_t
    {
        return bytes().find(needle, position);
    }

    /// Search needle's bytes using find(std::string_view, std::size_t), including position/empty rules.
    auto find(StringView needle, std::size_t position = 0) const noexcept -> std::size_t
    {
        return find(needle.bytes(), position);
    }

    /// Return the first matching byte index at or after position, or npos if no element matches.
    auto find(char needle, std::size_t position = 0) const noexcept -> std::size_t
    {
        return bytes().find(needle, position);
    }

    /// Return the last case-sensitive match's starting byte index <= position, or npos.
    /// Positions beyond size() (including npos) search from the end. An empty needle
    /// matches at min(position, size()). The trailing terminator is not an element.
    auto rfind(std::string_view needle, std::size_t position = npos) const noexcept -> std::size_t
    {
        return bytes().rfind(needle, position);
    }

    /// Search needle's bytes using rfind(std::string_view, std::size_t), including position/empty rules.
    auto rfind(StringView needle, std::size_t position = npos) const noexcept -> std::size_t
    {
        return rfind(needle.bytes(), position);
    }

    /// Return the last matching byte index <= position, or npos; oversized positions search from the end.
    auto rfind(char needle, std::size_t position = npos) const noexcept -> std::size_t
    {
        return bytes().rfind(needle, position);
    }

    /// Case-sensitive byte equality; absent and present empty strings compare equal.
    friend auto operator==(StringView left, StringView right) noexcept -> bool { return left.compare(right) == 0; }

    /// Case-sensitive equality with right's bytes; an absent left view compares as empty.
    friend auto operator==(StringView left, std::string_view right) noexcept -> bool
    {
        return left.compare(right) == 0;
    }

    /// Case-sensitive equality with left's bytes; an absent right view compares as empty.
    friend auto operator==(std::string_view left, StringView right) noexcept -> bool
    {
        return right.compare(left) == 0;
    }

private:
    friend class String;
    zend_string* _string;
};

/// Borrowed PHP value slot; defined below after ArrayView.
class ValueView;

/// Borrowed, read-only regular PHP array (not an arbitrary or persistent HashTable).
///
/// Copying a view does not retain the array. Keep its backing owner/request alive
/// on the same thread. Reacquire views after separation or mutation; element views
/// can also be invalidated by HashTable reallocation or replacement.
/// Accepts request-allocated arrays and Zend immutable arrays. An absent view has
/// size zero, no matching keys, and an empty iterator range. Presence (operator bool())
/// is distinct from nonemptiness. No lookup/iteration operation retains, inserts,
/// separates, or unwraps PHP references. Returned raw pointers are borrowed and
/// must not be released or used to mutate the array; use an Array owner's writable().
class ArrayView {
public:
    /// Either a signed PHP integer key or a borrowed binary-safe string key.
    using Key = std::variant<zend_long, StringView>;

    /// Borrowed key/value entry returned by iteration; defined below.
    struct Entry;

    /// Read-only input iterator with its own cursor; defined below.
    class Iterator;

    /// Iteration is read-only even when the view object itself is mutable.
    using iterator = Iterator;

    /// Same read-only iterator type as iterator.
    using const_iterator = Iterator;

    /// Borrow a live regular PHP array without retaining it; nullptr creates an absent view.
    explicit ArrayView(zend_array* array = nullptr) noexcept
        : _array(array)
    {}

    /// Whether an array is present; true for a present empty array (not PHP truthiness).
    explicit operator bool() const noexcept { return _array != nullptr; }

    /// Borrow the underlying read-only Zend pointer, or nullptr when absent; never release it.
    auto get() const noexcept -> zend_array const* { return _array; }

    /// Number of live elements, excluding deleted slots; zero when absent.
    auto size() const noexcept -> std::size_t { return _array ? zend_hash_num_elements(_array) : 0; }

    /// Whether size() is zero, including an absent array.
    auto empty() const noexcept -> bool { return size() == 0; }

    /// Borrow an element without insertion; a missing key or absent array yields an absent view.
    /// key is a signed PHP integer key, not an ordinal position. Negative keys are supported.
    /// A present null value remains a present slot; PHP references are not unwrapped.
    auto find(zend_long key) const noexcept -> ValueView;

    /// Binary-safe PHP key lookup: canonical integer strings (e.g. "1", but not "01")
    /// address integer keys, just as PHP array access does. No references are unwrapped.
    /// Integer conversion requires decimal digits with an optional minus, fitting zend_long;
    /// leading zeros (except "0"), "-0", a plus, whitespace, or trailing bytes prevent it. Missing keys/absent arrays
    /// yield an absent view; a present PHP null remains a present slot. The key bytes are not retained.
    auto find(std::string_view key) const noexcept -> ValueView;

    /// Equivalent to find(key) for an integer key; never inserts and does not require presence.
    auto operator[](zend_long key) const noexcept -> ValueView;

    /// Equivalent to find(key) with PHP string-key normalization; missing keys yield absent views.
    auto operator[](std::string_view key) const noexcept -> ValueView;

    /// Whether the integer key exists, including a present PHP null value; false when absent.
    auto contains(zend_long key) const noexcept -> bool;

    /// Whether the string key exists using find()'s normalization; present PHP null counts as present.
    auto contains(std::string_view key) const noexcept -> bool;

    /// Iterate borrowed key/value entries in insertion order, skipping deleted slots.
    /// Each iterator owns its cursor; PHP's internal array pointer is unchanged.
    /// Any array mutation or separation invalidates iterators and borrowed entries.
    /// Returns end() for empty/absent arrays. Integer keys preserve their signed PHP values.
    auto begin() const noexcept -> Iterator;

    /// Past-the-end iterator for this array; must not be dereferenced or incremented.
    auto end() const noexcept -> Iterator;

    /// Same read-only first-entry iterator as begin(), with its lifetime/invalidation rules.
    auto cbegin() const noexcept -> Iterator;

    /// Same read-only past-the-end iterator as end().
    auto cend() const noexcept -> Iterator;

private:
    friend class Array;
    zend_array* _array;
};

/// Borrowed, read-only zval slot. Copying a view never retains or dereferences it.
///
/// The slot and its value must remain alive on their owning request/thread.
/// Moving, releasing, replacing, or destroying the owning Value invalidates its
/// views. Array element views additionally depend on the array's mutation rules.
/// Accepts ordinary PHP values, references, and IS_UNDEF; internal engine-only slot
/// types such as IS_INDIRECT are outside the supported contract. A null slot pointer
/// represents absence, distinct from a present IS_UNDEF or PHP null slot. Type queries
/// and getters inspect the stored type without coercion/dereferencing; dereference()
/// explicitly borrows a referenced slot, while is_truthy() applies PHP conversion.
/// Unchecked getters require their documented type; violating a precondition is a
/// caller error, not a checked failure or C++ exception. Returned payload views/pointers
/// also borrow storage and must not be released or used to mutate the underlying value.
class ValueView {
public:
    /// Borrow a live zval slot without retaining its payload; nullptr creates an absent view.
    explicit ValueView(zval const* value = nullptr) noexcept
        : _value(value)
    {}

    /// Borrow the original read-only slot pointer, or nullptr when absent; never destroy it.
    auto get() const noexcept -> zval const* { return _value; }

    /// An absent view and an IS_UNDEF slot both report IS_UNDEF.
    /// Returns the stored Zend IS_* type tag, without dereferencing PHP references.
    auto type() const noexcept -> int { return _value ? Z_TYPE_P(_value) : IS_UNDEF; }

    /// Whether type() differs from IS_UNDEF; false when absent, true for PHP null/reference slots.
    auto is_initialized() const noexcept -> bool { return type() != IS_UNDEF; }

    /// Whether the stored type is IS_NULL; false for absence or a reference to null.
    auto is_null() const noexcept -> bool { return type() == IS_NULL; }

    /// Whether the view is absent or the stored type is IS_UNDEF.
    auto is_undef() const noexcept -> bool { return type() == IS_UNDEF; }

    /// Whether the stored type is IS_TRUE or IS_FALSE; does not apply PHP truthiness.
    auto is_bool() const noexcept -> bool { return is_true() || is_false(); }

    /// Whether the stored type is exactly IS_TRUE; use is_truthy() for PHP boolean conversion.
    auto is_true() const noexcept -> bool { return type() == IS_TRUE; }

    /// Whether the stored type is exactly IS_FALSE; absence/null/zero are not boolean false slots.
    auto is_false() const noexcept -> bool { return type() == IS_FALSE; }

    /// Whether the stored type is IS_LONG; numeric strings/references do not qualify.
    auto is_long() const noexcept -> bool { return type() == IS_LONG; }

    /// Whether the stored type is IS_DOUBLE; integers/references do not qualify.
    auto is_double() const noexcept -> bool { return type() == IS_DOUBLE; }

    /// Whether the stored type is IS_STRING; does not convert or dereference.
    auto is_string() const noexcept -> bool { return type() == IS_STRING; }

    /// Whether the stored type is IS_ARRAY; does not dereference PHP references.
    auto is_array() const noexcept -> bool { return type() == IS_ARRAY; }

    /// Whether the stored type is IS_OBJECT; does not dereference PHP references.
    auto is_object() const noexcept -> bool { return type() == IS_OBJECT; }

    /// Whether the stored type is IS_RESOURCE; does not validate resource usability or dereference.
    auto is_resource() const noexcept -> bool { return type() == IS_RESOURCE; }

    /// Whether the stored type is IS_REFERENCE, regardless of its contained value's type.
    auto is_reference() const noexcept -> bool { return type() == IS_REFERENCE; }

    /// Evaluate PHP truthiness using zend_is_true(), including reference dereferencing.
    /// Absent/undefined views are false. Does not retain or convert the stored value.
    /// May invoke object conversion handlers and emit PHP diagnostics; normal PHP
    /// error/exception handling applies. Keep the backing value alive throughout the call.
    /// Null, false, numeric zero, "", "0", and empty arrays are false; for example,
    /// "false" and nonempty arrays are true. Object/resource behavior follows the host Zend.
    /// A boolean result does not indicate that PHP exception state is clear; this method
    /// neither clears pending PHP exceptions nor provides a bailout recovery boundary.
    auto is_truthy() const -> bool;

    /// Unchecked access: the slot must be present and is_bool() must be true. No PHP coercion.
    auto get_bool() const noexcept -> bool { return Z_TYPE_P(_value) == IS_TRUE; }

    /// Unchecked access: the slot must be present and is_long() must be true.
    auto get_long() const noexcept -> zend_long { return Z_LVAL_P(_value); }

    /// Unchecked access: the slot must be present and is_double() must be true.
    auto get_double() const noexcept -> double { return Z_DVAL_P(_value); }

    /// Unchecked borrowed access: the slot must be present and is_string() must be true.
    /// The returned StringView shares the payload/request lifetime and does not retain it.
    auto get_string() const noexcept -> StringView { return StringView(Z_STR_P(_value)); }

    /// Unchecked borrowed access: the slot must be present and is_array() must be true.
    /// The returned ArrayView shares the payload/request lifetime and mutation restrictions.
    auto get_array() const noexcept -> ArrayView { return ArrayView(Z_ARR_P(_value)); }

    /// Unchecked borrowed access: the slot must be present and is_object() must be true.
    /// Returns the existing object identity without retaining/cloning it; never release the pointer.
    auto get_object() const noexcept -> zend_object const* { return Z_OBJ_P(_value); }

    /// Unchecked borrowed access: the slot must be present and is_resource() must be true.
    /// Returns the Zend resource without retaining or checking usability; never release the pointer.
    auto get_resource() const noexcept -> zend_resource const* { return Z_RES_P(_value); }

    /// Return the stored boolean, or fallback on absence or a different type. No coercion.
    auto get_bool_or(bool fallback) const noexcept -> bool { return is_bool() ? get_bool() : fallback; }

    /// Return the stored integer, or fallback on absence or a different type. No coercion.
    auto get_long_or(zend_long fallback) const noexcept -> zend_long { return is_long() ? get_long() : fallback; }

    /// Return the stored double, or fallback on absence or a different type. No coercion.
    auto get_double_or(double fallback) const noexcept -> double { return is_double() ? get_double() : fallback; }

    /// Explicitly borrow a PHP reference's contained slot; other views are returned unchanged.
    /// Type queries/getters inspect the stored type; is_truthy() also dereferences.
    auto dereference() const noexcept -> ValueView { return is_reference() ? ValueView(Z_REFVAL_P(_value)) : *this; }

private:
    zval const* _value;
};

/// A borrowed array entry. Integer keys retain PHP's signed integer interpretation.
/// String keys and values share the array's lifetime and mutation restrictions.
struct ArrayView::Entry {
    /// Signed integer key or borrowed string key; no key payload ownership is acquired.
    Key       key;
    /// Borrowed element slot, preserving PHP reference wrappers and present null values.
    ValueView value;
};

/// Read-only input iterator returning entries by value, without retaining their payloads.
/// Copies have independent cursors. Dereference/increment require a non-end iterator.
/// Entries are returned by value, so a copied Entry still borrows its key/value payloads.
/// Mutation/separation invalidates the iterator and its borrowed entries. Keep the
/// backing array/request alive. Iteration never modifies PHP's internal array pointer.
class ArrayView::Iterator {
public:
    /// Keeps a temporary entry alive for an iterator's arrow expression.
    struct ArrowProxy {
        /// Entry snapshot owning no Zend payloads; shares the iterator's borrowing restrictions.
        Entry entry;

        /// Borrow this proxy's entry; the pointer expires when the proxy is destroyed.
        /// For a normal it->member expression, that is the end of the full expression.
        auto operator->() const noexcept -> Entry const* { return &entry; }
    };

    /// Standard input-iterator category; dereference yields an Entry value, not a stored Entry reference.
    using iterator_category = std::input_iterator_tag;

    /// Value returned by dereference, containing borrowed key/value payloads.
    using value_type = Entry;

    /// Signed distance type for standard iterator algorithms.
    using difference_type = std::ptrdiff_t;

    /// Dereference result is a value; there is no mutable reference to an array entry.
    using reference = Entry;

    /// Arrow-expression proxy, not an owning or stable pointer into the array.
    using pointer = ArrowProxy;

    /// Construct the end iterator of an absent ArrayView; equal to other default iterators.
    /// It may be copied/compared but must not be dereferenced or incremented.
    Iterator() noexcept = default;

    /// Return the current entry by value without retaining key/value payloads; requires a live element.
    auto operator*() const noexcept -> Entry;

    /// Return a temporary entry proxy; requires a live element. See ArrowProxy::operator->() for lifetime.
    auto operator->() const noexcept -> ArrowProxy { return ArrowProxy{**this}; }

    /// Advance to the next live entry or end(), skipping deleted slots; return *this.
    /// Requires a non-end iterator; does not change any copied iterator's cursor.
    auto operator++() noexcept -> Iterator&;

    /// Advance as prefix increment does and return the previous cursor by value; requires non-end.
    auto operator++(int) noexcept -> Iterator;

    /// Whether both cursors identify the same backing array and position.
    /// Different arrays' end iterators compare unequal; defaults equal an absent array's end().
    friend auto operator==(Iterator left, Iterator right) noexcept -> bool
    {
        return left._array == right._array && left._position == right._position;
    }

private:
    friend class ArrayView;

    Iterator(zend_array* array, HashPosition position) noexcept
        : _array(array)
        , _position(position)
    {}

    zend_array*  _array    = nullptr;
    HashPosition _position = 0;
};

} // namespace php
