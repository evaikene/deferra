#pragma once

#include "event_loop.hpp"
#include "signal.hpp"

#include <memory>

namespace jb::core {

class EventThread;
class ThreadCtx;

namespace priv {
struct ObjectPrivate; // Defined in object_priv.hpp
} // namespace priv

/// Root base class for all objects that participate in the signal-slot
/// system and the parent-ownership tree.
///
/// Parent-child ownership:
///
/// Passing a non-null @p parent to the constructor transfers lifetime ownership.
/// When the parent is deleted, it recursively deletes all its children. Children
/// can also be explicitly deleted before their parent; the destructor unlinks
/// itself automatically.
///
/// Signals:
///
/// Subclasses declare public `Signal<Args...>` members and emit them via the
/// protected `emit()` method.
///
/// @code
/// class Button : public jb::core::Object {
/// public:
///     jb::core::Signal<> clicked;
///     void press() { emit(clicked); }
/// };
/// @endcode
///
/// Pimpl pattern:
///
/// Subclasses that need private state follow the two-constructor convention so
/// that Object handles allocation. Object always deletes the private struct;
/// subclasses must not delete it.
///
/// @code
/// MyWidget::MyWidget(Object* parent)
///     : Object(*new priv::MyWidgetPrivate(), parent)
/// {}
/// @endcode
///
/// `Object` always deletes the private struct; subclasses must not delete it.
///
/// Thread affinity:
///
/// Each Object records ThreadCtx and EventLoop at construction time. These values
/// govern Auto-connection dispatch and can be changed by `move_to_thread()`.
/// Object must be deleted in the thread it currently lives on.
///
/// Lifetime rules:
///
/// - Parent-owned Objects must be heap-allocated
/// - Objects are non-copyable and non-movable
/// - Never hold a raw pointer to parent-owned Objects past the parent's destruction.
class Object {
public:

    /// Constructs a root Object with its own ObjectPrivate
    /// @param[in] parent Optional parent that will own this object
    ///
    /// Note that `parent` must be created in the same thread as this object.
    explicit Object(Object* parent = nullptr);

    /// Destructor
    virtual ~Object();

    /// Objects are not copyable nor movable
    Object(Object const&)                    = delete;
    Object(Object&&)                         = delete;
    auto operator=(Object const&) -> Object& = delete;
    auto operator=(Object&&) -> Object&      = delete;

    /// Returns the parent of this Object
    /// @return Parent of this Object (can be nullptr if no parent)
    [[nodiscard]] auto parent() const -> Object*;

    /// Re-parents this Object
    /// @param[in] parent New parent
    /// @return True if the parent was changed successfully; false otherwise
    ///
    /// Removes this Object from the old parent's child list and appends it to
    /// the new parent's. Pass nullptr to make this a root Object.
    ///
    /// If `parent` is not null, inherits the new parent's event loop.
    ///
    /// Note that `parent` must be created in the same thread as this Object.
    auto set_parent(Object* parent) -> bool;

    /// Returns a list of this object's children in insertion order
    /// @return List of this object's children
    [[nodiscard]] auto children() const noexcept -> std::vector<Object*> const&;

    /// Returns the thread context this Object lives in
    /// @return Thread context this Object lives in
    [[nodiscard]] auto thread_ctx() const noexcept -> ThreadCtx const*;

    /// Returns the EventLoop this object is associated with
    /// @return EventLoop this object is associated with (can be nullptr if not set)
    ///
    /// nullptr is returned when the Object was created before any EventLoop
    /// was running on its thread (e.g. a static or very-early Object).
    [[nodiscard]] auto event_loop() const noexcept -> EventLoop*;

    /// Schedules the Object for deletion. The Object will be deleted by the
    /// event loop of the thread this object lives on. If called from a different
    /// thread, the deletion will be scheduled on the object's thread.
    ///
    /// NOTE: The object MUST have an event loop set on its thread for this method
    /// to work.
    void delete_later();

    /// Moves the object all its children to the event loop of the specified thread
    /// @param[in] event_thread Event thread to move to
    /// @return True if the object was moved successfully; false otherwise
    ///
    /// The object and all its children will be moved to the event loop of the specified
    /// thread. The object and its children start receiving signals and events on the
    /// new thread after this call. The object itself must not have a parent.
    ///
    /// This method is NOT thread-safe and must be called from the thread the object
    /// was created in.
    auto move_to_thread(EventThread* event_thread) -> bool;

    //--- SIGNALS ---

    /// Signal emitted when the object is destroyed.
    Signal<> destroyed;

protected:

    /// Constructor for subclasses that supply their own private data.
    /// @param[in] dd  Reference to a heap-allocated struct that inherits (directly
    ///                or transitively) from priv::ObjectPrivate. Object takes ownership;
    ///                do NOT delete @p dd elsewhere.
    /// @param[in] parent Optional parent
    explicit Object(priv::ObjectPrivate& dd, Object* parent = nullptr);

    /// Emit @p signal with the given arguments.
    ///
    /// Invokes all connected slots, dispatching Direct connections inline and
    /// posting Queued connections to their receiver's EventLoop.
    ///
    /// This overload handles Signal<Args...> with one or more parameters.
    /// The non-arg specialisation below handles Signal<>.
    template <typename... Args>
    void emit(Signal<Args...>& signal, Args... args)
    {
        signal.emit(this, args...);
    }

    /// Emit a zero-argument signal
    void emit(Signal<>& signal) { signal.emit(this); }

    /// Returns a pointer to the private data
    /// Subclasses downcast this to their concrete private data type.
    [[nodiscard]] auto d_ptr() const noexcept -> void*;

    /// Templated helper for downcasting d_ptr() to a concrete private data type
    /// @tparam T Concrete private data type to downcast to
    /// @return Pointer to the private data downcast to T
    template <typename T>
    [[nodiscard]] auto d_ptr() const noexcept -> T*
    {
        return static_cast<T*>(d_ptr());
    }

private:

    /// Grant all Signal specialisations access to register_connection()
    template <typename...>
    friend class Signal;

    /// Called by Signal<Args...>::connect() to register a connection record
    void register_connection(std::shared_ptr<priv::ConnectionBase> const& conn);

    /// Shared init called by both constructors
    void init_common(Object* parent);

    /// Internal move implementation; called by move_to_thread() after sanity checks
    auto move_to_thread_impl(EventThread* event_thread) -> bool;

    /// Internal helper to recursively move this object and its children to a new event loop
    void move_to_event_loop(EventLoop* new_loop);

    /// d-ptr with private data; owned; always non-null; deleted in ~Object()
    priv::ObjectPrivate* _d{nullptr};
};

} // namespace jb::core

// Signal method implementations
#include "signal_priv.hpp" // IWYU pragma: keep for private Signal implementations
