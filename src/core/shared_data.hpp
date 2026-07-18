#pragma once

/**
 * @file shared_data.hpp
 * @brief Provides reference-counted data with explicit detach semantics.
 *
 * Derive a payload from `SharedData` and manage it with
 * `ExplicitlySharedDataPointer<T>`. Copies of the pointer share the same
 * payload, so mutations are visible through every pointer until one of them
 * explicitly calls `detach()`:
 *
 * \code{.cpp}
 * struct Payload : jb::core::SharedData {
 *     int value{};
 * };
 *
 * auto first = jb::core::make_explicitly_shared<Payload>();
 * first->value = 1;
 *
 * auto second = first;
 * second.detach();
 * second->value = 2;
 *
 * // first->value is 1; second->value is 2.
 * \endcode
 *
 * `detach()` copies the concrete payload when the pointer is shared and makes
 * the calling pointer its unique owner. It is not automatic copy-on-write:
 * callers must invoke it before mutating data that should no longer be shared.
 * The payload type must therefore be copy-constructible when `detach()` is
 * used.
 *
 * `ExplicitlySharedDataPointer<T>` owns the pointer supplied to its raw-pointer
 * constructor or `reset()` call and deletes the payload when the last owner is
 * destroyed. The supplied pointer must be a newly owned allocation and must
 * not already be managed by another `ExplicitlySharedDataPointer`.
 */

#include <atomic>
#include <cassert>
#include <cstddef>
#include <type_traits>
#include <utility>

namespace jb::core {

/// Base class for any object that wants to be held by an `ExplicitlySharedDataPointer`.
/// Holds the atomic reference count.
class SharedData {
public:
    std::atomic<int> ref_count;

    explicit SharedData() noexcept
        : ref_count(0)
    {}

    /// Copy constructor always creates a brand-new object with no references
    SharedData(SharedData const& _)
        : ref_count(0)
    {}

    /// Non-movable and non-assignable
    SharedData(SharedData&&)                         = delete;
    auto operator=(SharedData const&) -> SharedData& = delete;
    auto operator=(SharedData&&) -> SharedData&      = delete;

protected:

    /// Protected and non-virtual destructor. The pointer always deletes via the
    /// concrete `T*`,
    ~SharedData() = default;
};

/// Reference-counting smart pointer to a SharedData-derived object.
/// Copies are cheap with refcount bump; mutations through one pointer are visible
/// to all other pointers sharing the same data. The `detach()` method allows to
/// create a unique copy of the data when needed.
template <typename T> requires std::is_base_of_v<SharedData, T>
class ExplicitlySharedDataPointer {
public:
    using Type    = T;
    using pointer = T*;

    /// Default constructor
    ///
    /// Constructs an empty pointer with no data (i.e., `nullptr`).
    ExplicitlySharedDataPointer() noexcept = default;

    /// Constructor from a raw pointer
    ///
    /// Constructs a pointer that takes ownership of the given raw pointer. The
    /// reference count of the data is initialized to 1. The caller must ensure that
    /// the raw pointer is valid and not shared with any other `ExplicitlySharedDataPointer`
    /// instance.
    explicit ExplicitlySharedDataPointer(T* data) noexcept
        : _d(data)
    {
        if (_d) {
            _d->ref_count.store(1, std::memory_order_relaxed);
        }
    }

    /// Copy constructor
    ///
    /// Constructs a new pointer that shares ownership of the same data as the other
    /// pointer. The reference count of the data is incremented by 1.
    ExplicitlySharedDataPointer(ExplicitlySharedDataPointer const& other) noexcept
        : _d(other._d)
    {
        if (_d) {
            _d->ref_count.fetch_add(1, std::memory_order_relaxed);
        }
    }

    /// Move constructor
    ///
    /// Constructs a new pointer that takes ownership of the data from the other
    /// pointer. The other pointer is left in an empty state (i.e., `nullptr`).
    ExplicitlySharedDataPointer(ExplicitlySharedDataPointer&& other) noexcept
        : _d(std::exchange(other._d, nullptr))
    {}

    /// Destructor
    ///
    /// Decrements the reference count of the data and deletes it if the count reaches zero.
    ~ExplicitlySharedDataPointer() noexcept { release(_d); }

    /// Copy assignment operator
    ///
    /// Assigns the pointer to share ownership of the same data as the other pointer.
    /// The reference count of the new data is incremented by 1, and the reference count
    /// of the old data is decremented by 1 (and deleted if it reaches zero).
    auto operator=(ExplicitlySharedDataPointer const& other) noexcept -> ExplicitlySharedDataPointer&
    {
        if (this != &other && this->_d != other._d) {
            if (other._d) {
                other._d->ref_count.fetch_add(1, std::memory_order_relaxed);
            }
            T* old = _d;
            _d     = other._d;
            release(old);
        }
        return *this;
    }

    /// Move assignment operator
    ///
    /// Assigns the pointer to take ownership of the data from the other pointer.
    /// The other pointer is left in an empty state (i.e., `nullptr`). The reference
    /// count of the old data is decremented by 1 (and deleted if it reaches zero).
    auto operator=(ExplicitlySharedDataPointer&& other) noexcept -> ExplicitlySharedDataPointer&
    {
        if (this != &other) {
            ExplicitlySharedDataPointer{std::move(other)}.swap(*this);
        }
        return *this;
    }

    /// Raw pointer assignment operator
    ///
    /// Assigns the pointer to take ownership of the given raw pointer. The caller
    /// must ensure that the raw pointer is valid and not shared with any other
    /// `ExplicitlySharedDataPointer` instance.
    auto operator=(T* data) noexcept -> ExplicitlySharedDataPointer&
    {
        ExplicitlySharedDataPointer{data}.swap(*this);
        return *this;
    }

    /// Detach the pointer to create a unique copy of the data if it is shared with
    /// other pointers.
    void detach()
    {
        if (_d && _d->ref_count.load(std::memory_order_acquire) > 1) {
            T* copy = new T{*_d};
            copy->ref_count.fetch_add(1, std::memory_order_relaxed);
            T* old = _d;
            _d     = copy;
            release(old);
        }
    }

    /// (Re)sets the pointer to take ownership of the given raw pointer.
    ///
    /// The reference count of the new data is initialized to 1, and the reference
    /// count of the old data is decremented by 1 (and deleted if it reaches zero).
    /// The caller must ensure that the raw pointer is valid and not shared with
    /// any other `ExplicitlySharedDataPointer` instance
    void reset(T* data = nullptr) { ExplicitlySharedDataPointer{data}.swap(*this); }

    /// Swaps the contents of this pointer with another pointer
    void swap(ExplicitlySharedDataPointer& other) noexcept { std::swap(_d, other._d); }

    /// Returns true if the pointer is not empty (i.e., it holds a non-null data pointer).
    explicit operator bool() const noexcept { return _d != nullptr; }

    /// Returns true if the pointer is empty (i.e., it holds a null data pointer).
    auto operator!() const noexcept -> bool { return _d == nullptr; }

    /// Returns a pointer to the data held by this pointer (can be nullptr if empty).
    [[nodiscard]] auto get() const noexcept -> T* { return _d; }

    /// Data access operator
    /// Returns a reference to the data pointed to by this pointer. The caller must ensure
    /// that the pointer is not empty before calling this operator.
    [[nodiscard]] auto operator*() const noexcept -> T&
    {
        assert(_d);
        return *_d;
    }

    /// Data access operator
    /// Returns a pointer to the data pointed to by this pointer. The caller must ensure
    /// that the pointer is not empty before calling this operator.
    [[nodiscard]] auto operator->() const noexcept -> T*
    {
        assert(_d);
        return _d;
    }

    /// Returns the use count of the data held by this pointer (i.e., the number of
    /// pointers sharing the same data).
    [[nodiscard]] auto use_count() const noexcept -> int
    {
        return _d ? _d->ref_count.load(std::memory_order_acquire) : 0;
    }

    /// Returns true if this pointer is the unique owner of the data (i.e., no other
    /// pointers share the same data).
    [[nodiscard]] auto unique() const noexcept -> bool { return use_count() == 1; }

    /// Compares this pointer with another pointer for equality. Two pointers are
    /// considered equal if they point to the same data (i.e., they share the same data pointer).
    friend auto operator==(ExplicitlySharedDataPointer const& a, ExplicitlySharedDataPointer const& b) noexcept -> bool
    {
        return a._d == b._d;
    }

    /// Compares this pointer with nullptr for equality. The pointer is considered equal
    /// to nullptr if it is empty (i.e., it holds a null data pointer).
    friend auto operator==(ExplicitlySharedDataPointer const& p, std::nullptr_t) noexcept -> bool
    {
        return p._d == nullptr;
    }

private:
    T* _d{nullptr};

    static void release(T* p) noexcept
    {
        if (p && p->ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            delete p;
        }
    }
};

/// Helper function to swap two `ExplicitlySharedDataPointer` instances
template <typename T>
void swap(ExplicitlySharedDataPointer<T>& a, ExplicitlySharedDataPointer<T>& b) noexcept
{
    a.swap(b);
}

/// Helper function to create an `ExplicitlySharedDataPointer`
template <typename T, typename... Args>
    requires std::is_base_of_v<SharedData, T>
auto make_explicitly_shared(Args&&... args) noexcept(std::is_nothrow_constructible_v<std::decay_t<T>, T&&>)
    -> ExplicitlySharedDataPointer<T>
{
    return ExplicitlySharedDataPointer<T>{new T{std::forward<Args>(args)...}};
}

} // namespace jb::core
