#pragma once

#include "connectable.hpp"
#include "signal.hpp"

#include <memory>

namespace jb::core {

namespace priv {
struct ObjectData;
} // namespace priv

/// Base for all the objects
class Object : public Connectable {
public:

    /// Constructor
    /// @param[in] parent Optional parent
    explicit Object(Object* parent = nullptr);

    /// Destructor
    ~Object() override;

    /// Objects are not copyable nor movable
    Object(Object const&)                    = delete;
    Object(Object&&)                         = delete;
    auto operator=(Object const&) -> Object& = delete;
    auto operator=(Object&&) -> Object&      = delete;

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

    /// Returns the event loop of the thread this object lives on.
    /// @return Event loop of the thread this object lives on (can be nullptr if not set)
    auto event_loop() const -> EventLoop* override;

    /// Sets or changes the parent
    /// @param[in] parent New parent
    void set_parent(Object* parent);

    //--- PUBLIC SIGNALS

    Signal<> destroyed;

private:

    friend struct priv::ObjectData;

    /// d-ptr with private data (also used as the lifetime token)
    std::shared_ptr<priv::ObjectData> _d;
};

} // namespace jb::core
