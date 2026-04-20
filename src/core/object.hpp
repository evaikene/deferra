#pragma once

#include "connectable.hpp"
#include "signal.hpp"

#include <memory>

namespace jb::core {

class EventThread;

class ThreadCtx;
namespace priv {
struct ObjectData;
} // namespace priv

/// Base for all the objects
class Object : public Connectable {
public:

    /// Constructor
    /// @param[in] parent Optional parent
    ///
    /// Note that `parent` must be created in the same thread as this object.
    explicit Object(Object* parent = nullptr);

    /// Destructor
    ~Object() override;

    /// Objects are not copyable nor movable
    Object(Object const&)                    = delete;
    Object(Object&&)                         = delete;
    auto operator=(Object const&) -> Object& = delete;
    auto operator=(Object&&) -> Object&      = delete;

    /// Returns the thread context this object was created in
    /// @return Thread context this object was created in
    auto thread_ctx() const -> ThreadCtx const*;

    /// Returns the event loop this object lives on
    /// @return Event loop this object lives on (can be nullptr if not set)
    auto event_loop() const -> EventLoop* override;

    /// Returns the parent of this object
    /// @return Parent of this object (can be nullptr if no parent)
    auto parent() const -> Object*;

    /// Returns a list of this object's children
    /// @return List of this object's children
    auto children() const -> std::vector<Object*> const&;

    /// Schedules the object for deletion. The object will be deleted by the
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

    /// Sets or changes the parent
    /// @param[in] parent New parent
    /// @return True if the parent was changed successfully; false otherwise
    ///
    /// The parent must have been created in the same thread as this object.
    auto set_parent(Object* parent) -> bool;

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

    /// Sets the event loop this object lives on.
    void set_event_loop(EventLoop* event_loop);

    /// Moves a parented object to the event loop of the specified thread
    /// @param[in] parent Parent of the object to move
    /// @param[in] event_thread Event thread to move to
    void move_to_thread(Object* parent, EventThread* event_thread);

private:

    friend struct priv::ObjectData;

    /// d-ptr with private data (also used as the lifetime token)
    std::shared_ptr<priv::ObjectData> _d;
};

} // namespace jb::core
