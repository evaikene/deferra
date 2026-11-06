#pragma once

/// @file
/// Move-only PHP array ownership. See zend.hpp for common integration/lifetime rules.

#include <phpcxx/views.hpp> // IWYU pragma: export

namespace php {

/// One owned regular PHP array reference, confined to its request/execution thread.
///
/// Supports request-allocated arrays and Zend immutable arrays, not persistent or
/// arbitrary native HashTables. Default/moved-from owners are absent. Destruction
/// releases contained values and nominates surviving cycles for Zend GC; it must
/// occur before request shutdown for external C++ owners; members of bound native
/// objects may be released by free_obj during normal Zend request teardown.
/// PHP bailout may skip C++ destructors.
class Array {
public:
    /// Construct an absent owner without allocating an array.
    Array() noexcept = default;

    /// Release through Zend's GC-aware zval destruction path; absence is a no-op.
    /// Destroying contained objects/resources can invoke PHP cleanup code.
    ~Array() noexcept;

    /// Copying is disabled; use retain() to share or duplicate() for separate top-level storage.
    Array(Array const&) = delete;

    /// Copy assignment is disabled; use retain() or duplicate() explicitly.
    auto operator=(Array const&) -> Array& = delete;

    /// Transfer other's reference without retaining it and leave other absent.
    Array(Array&& other) noexcept;

    /// Take other's reference, release this owner's old array, and return *this.
    /// Leaves other absent; self-move is a no-op. Old-array cleanup may invoke PHP code;
    /// the replacement is installed first. Borrows of released storage may expire.
    auto operator=(Array&& other) noexcept -> Array&;

    /// Create a valid empty request-local PHP array.
    static auto create() -> Array;

    /// Consume one caller-owned regular PHP array reference without retaining it; nullptr is allowed.
    /// The caller must not release the transferred reference. Immutable Zend arrays are
    /// accepted without a counted reference; arbitrary/persistent HashTables are not accepted.
    static auto adopt(zend_array* array) noexcept -> Array;

    /// Retain a borrowed array, respecting Zend immutable-array reference rules.
    /// An absent view yields an absent owner. Call writable() on an owner before mutation.
    static auto retain(ArrayView array) noexcept -> Array;

    /// Duplicate top-level storage; an absent owner yields an absent owner.
    /// Nested arrays retain copy-on-write sharing, objects retain identity, and PHP reference
    /// wrappers retain Zend semantics (array duplication may unwrap unshared references). This is neither a recursive
    /// graph copy nor an object clone.
    auto duplicate() const -> Array;

    /// Transfer the reference without decrementing it and leave this owner absent.
    /// Returns nullptr when absent. Consume a non-null result into another Array or a
    /// zval whose destruction uses zval_ptr_dtor(), so surviving cycles are nominated
    /// for Zend GC. Bare zend_array_release() does not nominate an untracked cycle.
    auto release() noexcept -> zend_array*;

    /// Separate shared or immutable storage before a write; absent owners return nullptr.
    ///
    /// The returned pointer is borrowed. Reacquire it before each mutation after any
    /// operation that may retain/share the array. Do not replace, release, or change
    /// the reference count through it. It does not recursively separate nested arrays.
    /// Reacquire views/iterators after separation or mutation. Retaining another owner
    /// after acquiring this pointer requires a fresh writable() call before the next write.
    auto writable() & -> zend_array*;

    /// Mutable borrowing from a temporary owner is disabled.
    auto writable() && -> zend_array* = delete;

    /// Whether an array is present; a present empty array is true (not PHP truthiness).
    explicit operator bool() const noexcept { return _array != nullptr; }

    /// Borrow the read-only Zend pointer, or nullptr when absent; do not release through it.
    /// The allocation/request must remain alive. Use writable(), not a cast of this pointer, to mutate.
    auto get() const noexcept -> zend_array const* { return _array; }

    /// Borrow without retaining; an absent owner yields an absent view.
    /// Keep the backing array/request alive and reacquire borrows after separation/mutation.
    auto view() const& noexcept -> ArrayView { return ArrayView(_array); }

    /// Borrowing from a temporary owner is disabled to prevent immediately dangling views.
    auto view() const&& -> ArrayView = delete;

private:
    zend_array* _array = nullptr;
};

} // namespace php
