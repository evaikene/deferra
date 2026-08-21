#pragma once

#include "connection.hpp"

#include <cassert>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace jb::core {

/// Forward declarations
class Object;
class EventLoop;

namespace priv {
struct ObjectLifetime;
}

/// Type-safe, thread-aware signal for the signal-slot system.
/// @tparam Args Owning, non-reference parameter types of the signal; may be empty (Signal<>)
///
/// A Signal<Args...> is a value member of an Object subclass. Zero or more slots
/// can be connected to it; emitting the signal calls all live slots.
///
/// Declaring signal:
///
/// @code
/// class MyWorker : public jb::core::Object {
/// public:
///     Signal<>     started;
///     Signal<int>  progress;
///     Signal<bool> finished;
/// };
/// @endcode
///
/// Emitting (inside the owning class):
///
/// @code
/// emit(started);
/// emit(progress, 42);
/// emit(finished, true);
/// @endcode
///
/// Connecting from outside:
///
/// @code
/// // Member-function slot (type checked at compile time)
/// worker->progress.connect(ui, &ProgressBar::set_value);
///
/// // Lambda slot (always Direct)
/// worker->finished.connect([](bool ok) {...});
///
/// // Explicit queued connection (cross-thread)
/// worker->finished.connect(handler, &Handler::on_finished, ConnectionType::Queued);
/// @endcode
///
/// Thread safety:
///
/// connect(), disconnect(), and disconnect_all() are protected by a mutex and
/// thread-safe. Slot invocations themselves are not protected; each slot must guard
/// its own mutable state.
///
/// Connecting or disconnecting slots changes only the signal's internal
/// subscription bookkeeping. These operations are therefore available through
/// a const Signal and do not modify the logical state of the owning Object.
///
/// Argument ownership and queued connections:
///
/// Direct delivery borrows the emitted objects and presents them to stored slots
/// as immutable references. Those references remain valid only for the duration
/// of the slot invocation; a slot must copy any argument that it retains.
///
/// Before an emission with one or more Queued (cross-thread) deliveries invokes
/// any slot, the signal creates one immutable owning snapshot and shares it among
/// all queued tasks. Each argument type used for queued delivery must therefore
/// be copy-constructible. Queued delivery runs when the receiver's EventLoop
/// processes EventFlag::Events.
///
template <typename... Args>
class Signal {
public:

    Signal() = default;

    ~Signal() { disconnect_all(); }

    /// Non-copyable. non-movable - signals are always members of Objects
    Signal(Signal const&)                    = delete;
    Signal(Signal&&)                         = delete;
    auto operator=(Signal const&) -> Signal& = delete;
    auto operator=(Signal&&) -> Signal&      = delete;

    /// Connect to a member function of an Object-derived @p receiver.
    /// @param[in] receiver Receiver object derived from Object
    /// @param[in] slot Any callable compatible with invocation as `void(Args const&...)`
    /// @param[in] type Connection type (default: Auto)
    /// @return A connection handle; keep it to disconnect later
    ///
    /// The slot type is checked at compile time against borrowed, immutable
    /// `Args const&...` arguments. A compatible value-taking slot remains valid
    /// and makes its own copy. A slot that retains an argument beyond the
    /// invocation must copy it explicitly.
    ///
    /// Typical use:
    ///
    /// @code
    /// sig.connect(obj, &MyClass::my_slot);
    /// // or with a capturing lambda to a method
    /// sig.connect(obj, [obj](int v) { obj->update(v); });
    /// @endcode
    ///
    template <typename Receiver, typename Slot>
    auto connect(Receiver* receiver, Slot&& slot, ConnectionType type = ConnectionType::Auto) const -> Connection;

    /// Connect to a callable with no Object receiver
    /// @param[in] callable Any callable compatible with invocation as `void(Args const&...)`
    /// @return A connection handle; keep it to disconnect later
    ///
    /// Lambda connections are always Direct: the callable is invoked in the emitting
    /// thread. The ConnectionType parameter is intentionally absent. Arguments are
    /// borrowed for the duration of the invocation; a callable that retains an
    /// argument must copy it explicitly. Compatible value-taking callables remain
    /// valid and make their own copies.
    template <typename Callable>
    auto connect(Callable&& callable) const -> Connection;

    /// Deactivate the connection identified by @p conn
    /// @param[in] conn The connection to disconnect
    ///
    /// Equivalent to calling conn.disconnect(). The slot entry is removed lazily
    /// from the internal list on next emit.
    void disconnect(Connection const& conn) const noexcept;

    /// Deactivate all connections and clear the internal list.
    void disconnect_all() const noexcept;

    /// Returns the number of connections
    auto count() const -> std::size_t
    {
        std::lock_guard lock(_mx);
        return _connections.size();
    }

private:

    /// Only Object::emit() calls emit_signal()
    friend class Object;

    /// Internal emit entry point, called by Object::emit()
    /// @param[in] sender The Object that is emitting the signal
    /// @param[in] args Signal arguments borrowed for Direct delivery and copied once for Queued delivery
    void emit(Object* sender, Args const&... args) const;

    /// One slot connected to this signal.
    struct TypedConn : priv::ConnectionBase {
        std::function<void(Args const&...)> slot;
        Object*                             receiver{nullptr}; ///< nullptr for lambda connections
        std::weak_ptr<priv::ObjectLifetime> receiver_lifetime;
        ConnectionType                      conn_type{ConnectionType::Auto};

        void invoke(Args const&... args)
        {
            if (slot) {
                slot(args...);
            }
        }
    };

    mutable std::mutex                              _mx;
    mutable std::vector<std::shared_ptr<TypedConn>> _connections;
};

} // namespace jb::core
