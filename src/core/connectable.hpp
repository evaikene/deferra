#pragma once

#include <memory>

namespace jb::core {

class EventLoop;

namespace priv {

/// Opaque token used to track the lifetime of an object. The token is valid as
/// long as the object exists and becomes invalid when the object is destroyed.
struct ObjectToken {
    explicit ObjectToken() = default;
};

} // namespace priv

/// Minimal base class for `Signal<>`.
class Connectable {
public:

    virtual ~Connectable() = default;

    /// Weak observer into this object's liveness.
    /// Expires before any member of the derived class is destroyed.
    [[nodiscard]] virtual auto token() const -> std::weak_ptr<priv::ObjectToken> = 0;

    /// The event loop of the thread this object lives on.
    virtual auto event_loop() const -> EventLoop* = 0;
};

} // namespace jb::core
