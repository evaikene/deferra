#pragma once

#include <phpcxx/views.hpp> // IWYU pragma: export

namespace php {

/// One owned regular PHP array reference, confined to its request/execution thread.
///
/// Supports request-allocated arrays and Zend immutable arrays, not persistent or
/// arbitrary native HashTables. Default/moved-from owners are absent. Destruction
/// releases contained values and nominates surviving cycles for Zend GC; it must
/// occur before request shutdown. PHP bailout may skip C++ destructors.
class Array {
public:
    Array() noexcept = default;
    ~Array() noexcept;
    Array(Array const&)                    = delete;
    auto operator=(Array const&) -> Array& = delete;
    Array(Array&& other) noexcept;
    auto operator=(Array&& other) noexcept -> Array&;

    /// Create a valid empty request-local PHP array.
    static auto create() -> Array;

    /// Consume one caller-owned array reference without retaining it; nullptr is allowed.
    static auto adopt(zend_array* array) noexcept -> Array;

    /// Retain a borrowed array, respecting Zend immutable-array reference rules.
    static auto retain(ArrayView array) noexcept -> Array;

    /// Duplicate top-level storage; nested values keep PHP copy/reference semantics.
    auto duplicate() const -> Array;

    /// Transfer the reference to the caller and leave this owner absent.
    auto release() noexcept -> zend_array*;

    /// Separate shared or immutable storage before a write; absent owners return nullptr.
    ///
    /// The returned pointer is borrowed. Reacquire it before each mutation after any
    /// operation that may retain/share the array. Do not replace, release, or change
    /// the reference count through it. It does not recursively separate nested arrays.
    auto writable() & -> zend_array*;
    auto writable() && -> zend_array* = delete;

    explicit operator bool() const noexcept { return _array != nullptr; }

    auto get() const noexcept -> zend_array const* { return _array; }

    auto view() const& noexcept -> ArrayView { return ArrayView(_array); }

    auto view() const&& -> ArrayView = delete;

private:
    zend_array* _array = nullptr;
};

} // namespace php
