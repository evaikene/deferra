#pragma once

#include "connection.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

namespace jb::core {

class EventLoop;
class Object;
class ThreadCtx;

namespace priv {

/// Internal Object lifetime tracking
struct ObjectLifetime {
    std::atomic_bool alive{true};
    std::atomic_bool delete_later_pending{false};

    /// Protects event-loop affinity while queued work is being routed.
    std::mutex event_loop_mx;
    EventLoop* event_loop{nullptr};
};

/// Internal state for Object
///
/// Subclass private structs inherit from this (directly or transitively):
///
/// @code
/// struct MyWidgetPrivate : jb::core::priv::ObjectPrivate {
///     int extra_field = 0;
/// };
/// @endcode
///
/// Object always deletes this struct via `delete _d`, so the virtual destructor
/// ensures the most-derived type is correctly destroyed.
struct ObjectPrivate {
    /// Parent in the ownership tree
    Object* parent = nullptr;

    /// Direct children (insertion order)
    std::vector<Object*> children;

    /// Event loop this object lives on with thread affinity
    EventLoop* event_loop = nullptr;

    /// Lifetime tracking for object deletion
    std::shared_ptr<ObjectLifetime> lifetime = std::make_shared<ObjectLifetime>();

    /// Connections where this object is the *receiver*
    /// Stored as weak_ptrs so that the Signal (sender side) can also let them
    /// expire naturally. On destruction, Object deactivates them all.
    std::mutex                                 connections_mx;
    std::vector<std::weak_ptr<ConnectionBase>> connections;

    virtual ~ObjectPrivate() = default;
};

} // namespace priv
} // namespace jb::core
