#pragma once

#include <phpcxx/exception.hpp>
#include <phpcxx/result.hpp>
#include <phpcxx/views.hpp>

#include <Zend/zend_gc.h>
#include <Zend/zend_objects.h>

#include <cstddef>
#include <limits>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace php {

/// Reports native-owned PHP references to Zend without retaining or destroying them.
/// Only valid during the binding policy's trace() call. Report each owned reference
/// once, including separate owners of the same value; never report borrowed references.
/// Tracing must not mutate state, execute PHP, or recursively invoke GC.
class GcVisitor {
public:
    /// Borrow a non-null Zend GC scratch buffer; normally supplied by ObjectBinding.
    explicit GcVisitor(zend_get_gc_buffer* buffer) noexcept
        : _buffer(buffer)
    {}

    /// Report an owned zval (including reference wrappers). Absent views are ignored.
    /// The backing value must remain alive and unchanged throughout GC enumeration.
    auto add(ValueView value) const noexcept -> void
    {
        if (value.get()) {
            // Zend copies the slot without retaining it; the local copy owns nothing.
            zval slot;
            ZVAL_COPY_VALUE(&slot, value.get());
            zend_get_gc_buffer_add_zval(_buffer, &slot);
        }
    }

private:
    zend_get_gc_buffer* _buffer;
};

/// Default policy: T explicitly implements trace(GcVisitor&) const noexcept, even
/// when it owns no PHP references. Cloning is disabled. A custom policy supplies
/// static trace(T const&, GcVisitor&) noexcept and constexpr bool cloneable.
/// A cloneable policy also supplies clone(T const&) noexcept -> Result<T> and a
/// nothrow-movable T. That policy defines native copy/deep-copy semantics explicitly.
template <typename T>
struct ObjectBindingPolicy {
    static constexpr bool cloneable = false;

    /// Enumerate the native value's owned PHP references without changing ownership.
    static auto trace(T const& native, GcVisitor& visitor) noexcept -> void
    {
        static_assert(noexcept(native.trace(visitor)), "Native tracing must be noexcept");
        native.trace(visitor);
    }
};

/// Embeds T in one request-allocated Zend object, retaining standard property behavior.
///
/// Bind an ordinary generated internal class during module startup. PHP subclasses
/// may inherit this binding; replacing handlers or mixing native layouts is unsupported.
/// Every object and borrowed native pointer stays on its owning PHP thread/request.
/// T need not be movable or standard-layout, but must be nothrow-destructible and
/// fit Zend's allocator alignment. Construction must not execute PHP or invoke GC;
/// perform recoverable work before construct(), or after it with Result-based cleanup.
/// PHP constructor failure does not undo a completed T: free_obj destroys it later.
///
/// Native destruction precedes standard-property cleanup. Access is rejected once
/// destruction starts. Destructors/callbacks must not unwind C++ exceptions; PHP
/// bailout may bypass cleanup and is not recovered here. Serialization is disabled.
/// Opt-in clones initialize native state before copying properties/running __clone.
/// Clones that fail before native initialization skip __destruct, but still run free_obj.
/// PHP 8.5's clone-with-properties operation is deliberately unsupported.
template <typename T, typename Policy = ObjectBindingPolicy<T>>
class ObjectBinding {
    static_assert(std::is_object_v<T> && !std::is_array_v<T> && std::is_same_v<T, std::remove_cv_t<T>>);
    static_assert(std::is_nothrow_destructible_v<T>, "Native destruction must be noexcept");
    static_assert(alignof(T) <= ZEND_MM_ALIGNMENT, "Native alignment exceeds Zend allocation alignment");
    static_assert(noexcept(Policy::trace(std::declval<T const&>(), std::declval<GcVisitor&>())),
                  "GC tracing must be noexcept");

public:
    /// Install storage handlers on a generated internal class during module startup.
    /// Rejects nonordinary classes and incompatible existing allocators/serialization.
    /// Does not register the class or retain its entry. Handlers are immutable and
    /// contain no class pointers or request state; subclasses inherit the allocator.
    static auto bind(zend_class_entry* entry) -> Result<void>
    {
        if (!entry || entry->type != ZEND_INTERNAL_CLASS ||
            (entry->ce_flags & (ZEND_ACC_INTERFACE | ZEND_ACC_TRAIT | ZEND_ACC_ENUM)) != 0 ||
            (entry->create_object && entry->create_object != create_object) || entry->serialize || entry->unserialize) {
            return Error{.kind = ErrorKind::Type, .message = "Class cannot use this native binding"};
        }

        // Initialize once during startup, before request threads can create objects.
        (void)handlers();
        entry->ce_flags      |= ZEND_ACC_NOT_SERIALIZABLE;
        entry->create_object  = create_object;
        return {};
    }

    /// Placement-construct native state exactly once; returns a borrowed pointer.
    /// Requires a live object of this binding. Rejects reinitialization and pending
    /// PHP exceptions. Arguments must provide a nothrow constructor; construction
    /// itself must not call PHP/GC or expose partially constructed state.
    template <typename... Args>
    static auto construct(zend_object* object, Args&&... args) -> Result<T*>
    {
        static_assert(std::is_nothrow_constructible_v<T, Args...>, "Native construction must be noexcept");

        if (has_exception()) {
            return pending_exception();
        }
        if (!matches(object)) {
            return Error{.kind = ErrorKind::Type, .message = "Object has an incompatible native binding"};
        }

        auto* storage = storage_of(object);
        if (storage->_state != State::Empty) {
            return Error{.kind = ErrorKind::Runtime, .message = "Native object is already initialized or unavailable"};
        }

        storage->_state = State::Constructing;
        auto* native    = ::new (static_cast<void*>(storage->_native)) T(std::forward<Args>(args)...);
        storage->_state = State::Ready;
        return native;
    }

    /// Borrow initialized native state, optionally requiring a PHP class/interface.
    /// Binding identity is checked before applying the allocation offset, including
    /// for subclasses. Failure leaves pending PHP exceptions and ownership unchanged.
    /// The pointer remains valid only while the object lives and is not being freed.
    static auto get(zend_object* object, zend_class_entry* expected = nullptr) -> Result<T*>
    {
        if (!matches(object) || (expected && !instanceof_function(object->ce, expected))) {
            return Error{.kind = ErrorKind::Type, .message = "Object has an incompatible native binding or class"};
        }

        auto* storage = storage_of(object);
        if (storage->_state != State::Ready) {
            return Error{.kind    = ErrorKind::Runtime,
                         .message = "Native object is not initialized or is being destroyed"};
        }
        return native_of(storage);
    }

private:
    enum class State : unsigned char {
        Empty,
        Constructing,
        Ready,
        Destroying
    };

    // Only this prefix is a C++ object. Zend may omit its inline property slot
    // for a propertyless class, so never placement-construct a larger C++ struct
    // containing zend_object. Zend owns the trailing object/property allocation.
    struct Storage {
        State _state = State::Empty;
        alignas(T) std::byte _native[sizeof(T)];
    };

    static constexpr std::size_t _object_offset =
        (sizeof(Storage) + alignof(zend_object) - 1) / alignof(zend_object) * alignof(zend_object);
    static_assert(alignof(Storage) <= ZEND_MM_ALIGNMENT && alignof(zend_object) <= ZEND_MM_ALIGNMENT);
    static_assert(_object_offset <= static_cast<std::size_t>(std::numeric_limits<int>::max()));

    static auto handlers() noexcept -> zend_object_handlers const&
    {
        static auto const table = [] {
            auto result      = *zend_get_std_object_handlers();
            result.offset    = static_cast<int>(_object_offset);
            result.free_obj  = free_object;
            result.get_gc    = get_gc;
            result.clone_obj = nullptr;
            if constexpr (Policy::cloneable) {
                result.clone_obj = clone_object;
            }
#if PHP_VERSION_ID >= 80500
            // The standard handler allocates an ordinary object, losing native state.
            result.clone_obj_with = nullptr;
#endif
            return result;
        }();
        return table;
    }

    static auto matches(zend_object const* object) noexcept -> bool
    {
        return object != nullptr && object->handlers == &handlers();
    }

    static auto storage_of(zend_object* object) noexcept -> Storage*
    {
        return std::launder(reinterpret_cast<Storage*>(reinterpret_cast<std::byte*>(object) - _object_offset));
    }

    static auto native_of(Storage* storage) noexcept -> T*
    {
        return std::launder(reinterpret_cast<T*>(storage->_native));
    }

    static auto create_object(zend_class_entry* entry) noexcept -> zend_object*
    {
        auto* allocation = zend_object_alloc(_object_offset + sizeof(zend_object), entry);
        ::new (allocation) Storage;
        auto* object = reinterpret_cast<zend_object*>(static_cast<std::byte*>(allocation) + _object_offset);

        zend_object_std_init(object, entry);
        object_properties_init(object, entry);
        object->handlers = &handlers();
        return object;
    }

    static auto free_object(zend_object* object) noexcept -> void
    {
        auto*      storage   = storage_of(object);
        auto const was_ready = storage->_state == State::Ready;

        // Reentrant PHP cleanup must never obtain a half-destroyed native value.
        storage->_state = State::Destroying;
        if (was_ready) {
            std::destroy_at(native_of(storage));
        }

        zend_object_std_dtor(object);
        std::destroy_at(storage);
        // Zend frees the original allocation using handlers.offset after this returns.
    }

    static auto get_gc(zend_object* object, zval** table, int* count) noexcept -> HashTable*
    {
        // Preserve both standard representations: inline slots or a property table.
        auto* properties = zend_std_get_gc(object, table, count);
        auto* storage    = storage_of(object);
        if (storage->_state != State::Ready) {
            return properties;
        }

        auto*     buffer = zend_get_gc_buffer_create();
        GcVisitor visitor(buffer);
        for (int index = 0; index < *count; ++index) {
            visitor.add(ValueView(*table + index));
        }
        Policy::trace(*native_of(storage), visitor);
        zend_get_gc_buffer_use(buffer, table, count);
        return properties;
    }

    static auto clone_object(zend_object* source) noexcept -> zend_object*
    {
        static_assert(std::is_nothrow_move_constructible_v<T>, "Cloneable native state must be nothrow-movable");
        static_assert(noexcept(Policy::clone(std::declval<T const&>())), "Native cloning must be noexcept");

        // Even failure returns a valid Zend object: the VM owns its cleanup after
        // observing the pending exception. Never return null or free it ourselves.
        auto* target = create_object(source->ce);

        // Until native construction succeeds, a subclass destructor cannot safely
        // access this target. Suppress only userland destruction; free_obj must run.
        GC_ADD_FLAGS(target, IS_OBJ_DESTRUCTOR_CALLED);
        auto native = get(source);
        if (!native) {
            raise(native.error());
            return target;
        }
        if (has_exception()) {
            return target;
        }

        auto copy = Policy::clone(*native.value());
        if (!copy) {
            raise(copy.error());
            return target;
        }

        auto constructed = construct(target, std::move(copy).value());
        if (!constructed) {
            raise(constructed.error());
            return target;
        }

        // Restore normal destruction before property copying or __clone can execute
        // PHP. Failures from that point own initialized native state and follow Zend.
        GC_DEL_FLAGS(target, IS_OBJ_DESTRUCTOR_CALLED);
        zend_objects_clone_members(target, source);
        return target;
    }
};

} // namespace php
