#pragma once

/// @file
/// Move-only PHP value ownership. See zend.hpp for common integration/lifetime rules.

#include <phpcxx/views.hpp> // IWYU pragma: export

namespace php {

/// An owned zval, confined to its PHP request/execution thread.
///
/// Default and moved-from values are IS_UNDEF, distinct from PHP null. Accepts
/// ordinary PHP values, including references, or IS_UNDEF; internal engine-only
/// slot types such as IS_INDIRECT are outside this contract. Destruction can invoke
/// PHP object/resource cleanup and must precede request shutdown. PHP bailout may
/// skip C++ destructors; this type does not provide a bailout recovery boundary.
class Value {
public:
    /// Construct an uninitialized IS_UNDEF slot, distinct from PHP null.
    Value() noexcept = default;

    /// Destroy the owned value with zval_ptr_dtor(); may invoke PHP object/resource cleanup.
    ~Value() noexcept;

    /// Copying is disabled; use retain() or duplicate() to acquire another owner explicitly.
    Value(Value const&) = delete;

    /// Copy assignment is disabled; use retain() or duplicate() explicitly.
    auto operator=(Value const&) -> Value& = delete;

    /// Transfer other's value and leave its slot IS_UNDEF; invalidates views of other's slot.
    Value(Value&& other) noexcept;

    /// Take other's value, destroy this owner's old value, and return *this.
    /// Leaves other IS_UNDEF; self-move is a no-op. Invalidates existing slot views.
    /// The replacement is installed before old-value cleanup can reenter PHP code.
    auto operator=(Value&& other) noexcept -> Value&;

    /// Construct a valid PHP null value.
    static auto null() noexcept -> Value;

    /// Transfer a zval's ownership without retaining it and set the source slot to IS_UNDEF.
    /// The source must own its value (or be IS_UNDEF); do not pass a borrowed array/property
    /// slot whose owner still manages it. Invalidates existing views of the source slot.
    static auto adopt(zval& value) noexcept -> Value;

    /// Copy using ZVAL_COPY semantics, retaining PHP reference wrappers without dereferencing.
    /// Absent/IS_UNDEF views yield an IS_UNDEF owner. Arrays share copy-on-write storage;
    /// objects preserve identity. No PHP value conversion or recursive duplication occurs.
    static auto retain(ValueView value) noexcept -> Value;

    /// Duplicate a direct array's top-level storage, otherwise retain/copy its PHP value.
    /// IS_UNDEF remains IS_UNDEF. Nested arrays retain copy-on-write sharing, objects retain
    /// identity, and a directly held PHP reference remains shared without dereferencing.
    /// Array contents follow Array::duplicate() semantics; no recursive graph copy/object clone occurs.
    auto duplicate() const -> Value;

    /// Transfer ownership without decrementing references and leave this slot IS_UNDEF.
    /// The caller must consume the returned zval into a new owner/Zend value or call
    /// zval_ptr_dtor() on it. Invalidates slot views; IS_UNDEF is transferred unchanged.
    auto release() noexcept -> zval;

    /// Borrow this owner's read-only slot; never null, even when its type is IS_UNDEF.
    /// Do not destroy or change reference counts through it. Moving, releasing, replacing,
    /// or destroying this owner invalidates the borrow.
    auto get() const noexcept -> zval const* { return &_value; }

    /// Borrow this owner's slot without retaining; shares get()'s lifetime/invalidation rules.
    auto view() const& noexcept -> ValueView { return ValueView(&_value); }

    /// Borrowing from a temporary owner is disabled to prevent immediately dangling views.
    auto view() const&& -> ValueView = delete;

    /// Whether the stored type is other than IS_UNDEF; PHP null is initialized.
    auto is_initialized() const noexcept -> bool { return view().is_initialized(); }

    /// Whether the stored type is exactly IS_NULL; does not dereference PHP references.
    auto is_null() const noexcept -> bool { return view().is_null(); }

private:
    zval _value{};
};

} // namespace php
