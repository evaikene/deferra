#pragma once

#include "event_loop.hpp"
#include "signal.hpp"

#include <memory>

namespace jb::core {

class EventThread;
class ThreadCtx;

namespace priv {
struct ObjectData;
} // namespace priv

 /// Root base class for all objects that participate in the signal-slot
 /// system and the parent-ownership tree.
 ///
 /// Parent-child ownership
 /// Passing a non-null @p parent to the constructor transfers lifetime ownership.
 /// When the parent is deleted, it recursively deletes all its children. Children
 /// can also be explicitly deleted before their parent; the destructor unlinks
 /// itself automatically.
 ///
 /// Signals
 /// Subclasses declare public `Signal<Args...>` members and emit them via the
 /// protected `emit()` method.
 ///
 /// Pimpl pattern
 /// Subclasses that need private state follow the two-constructor convention so
 /// that Object handles allocation. Object always deletes the private struct;
 /// subclasses must not delete it.
 ///
 /// Thread affinity
 /// Each Object records ThreadCtx and EventLoop at construction time. These values
 /// govern Auto-connection dispatch and can be changed by `move_to_thread()`.
 /// Object must be deleted in the thread it currently lives on.
 ///
 /// Lifetime rules
 /// - Parent-owned Objects must be heap-allocated
 /// - Objects are non-copyable and non-movable
 /// - Never hold a raw pointer to parent-owned Objects past the parent's destruction.
class Object {
public:

    /// Constructor
    /// @param[in] parent Optional parent
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
 
    /// Sets or changes the parent
    /// @param[in] new_parent New parent
    /// @return True if the parent was changed successfully; false otherwise
    ///
    /// Removes this Object from the old parent's child list and appends it to
    /// the new parent's. Pass nullptr to make this a root Object.
    ///
    /// Note that `parent` must be created in the same thread as this Object.
    auto set_parent(Object* new_parent) -> bool;
 
    /// Returns a list of this object's children in insertion order
    /// @return List of this object's children
    [[nodiscard]] auto children() const -> std::vector<Object*> const&;
 
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

    /// Returns a weak reference to this object's token
    /// @return Weak reference to the token
    ///
    /// The token is used to track the lifetime of the object and can be used to
    /// safely access the object from other threads. The token is valid as long
    /// as the object exists and becomes invalid when the object is destroyed.
    [[nodiscard]] auto token() const -> std::weak_ptr<priv::ObjectToken> override;

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
     /// @param[in] d      Reference to a heap-allocated struct that inherits (directly
     ///                   or transitively) from priv::ObjectData. Object takes owbership;
     ///                   do NOT delete @p d elsewhere.
     /// @param[in] parent Optional paremt
     explicit Object(priv::ObjectData& d, Object* parent = nullptr);
 
     /// Emit @p signal with the given arguments.
     ///
     /// Invokes all connected slots, dispatching Direct connections inline and
     /// posting Queued connections to their receiver's EventLoop.
     template<typename... Args>
     void emit(Signal<Args...>& signal, Args... args)
     {
         signal.emit(this, args...);
     }
 
     /// Emit a zero-argument signal
     void emit(Signal<>& signal)
     {
         signal.emit(this);
     }
 
     /// Returns a pointer to the private data
     /// Subclasses downcast this to their concrete private data type.
     [[nodiscard]] auto d_ptr() noexcept -> void*;
     [[nodiscard]] auto d_ptr() const noexcept -> void const*;

private:

    friend struct priv::ObjectData;

    /// d-ptr with private data (also used as the lifetime token)
    std::shared_ptr<priv::ObjectData> _d;
};

} // namespace jb::core

// Signal method implementations
#include "signal_priv.hpp"
