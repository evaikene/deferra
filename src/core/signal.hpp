#pragma once

#include "connection.hpp"
#include "thread_context.hpp"

#include <cassert>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace jb::core {

/// Forward declarations
class Object;
class EventLoop;

/// Type-safe, thread-aware signal for the signal-slot system.
/// @tparam Args Parameter types of the signal; may be empty (Signal<>)
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
/// Queued connections and copyability:
///
/// For Queued (cross-thread) delivery, all @p Args are captured by value and each
/// argument type must therefore be copyable (or at least movable).
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
    /// @param[in] slot Any callable compatible with `void(Args...)`
    /// @param[in] type Connection type (default: Auto)
    /// @return A connection handle; keep it to disconnect later
    ///
    /// The slot type is checked at compile time: it must be callable with
    /// `Args...`.
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
    auto connect(Receiver* receiver, Slot&& slot, ConnectionType type = ConnectionType::Auto) -> Connection;

    /// Connect to a callable with no Object receiver
    /// @param[in] callable Any callable compatible with `void(Args...)`
    /// @return A connection handle; keep it to disconnect later
    ///
    /// Lambda connections are always Direct: the callable is invoked in the emitting
    /// thread. The ConnectionType parameter is intentionally absent.
    template <typename Callable>
    auto connect(Callable&& callable) -> Connection;

    /// Deactivate the connection identified by @p conn
    /// @param[in] conn The connection to disconnect
    ///
    /// Equivalent to calling conn.disconnect(). The slot entry is removed lazily
    /// from the internal list on next emit.
    void disconnect(Connection const& conn) noexcept;

    /// Deactivate all connections and clear the internal list.
    void disconnect_all() noexcept;

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
    /// @param[in] args Signal arguments (passed by value; copied for Queued)
    void emit(Object* sender, Args... args);

    /// One slot connected to this signal.
    struct TypedConn : priv::ConnectionBase {
        std::function<void(Args...)> slot;
        Object*                      receiver{nullptr};      ///< nullptr for lambda connections
        ConnectionType               conn_type{ConnectionType::Auto};

        void invoke(Args... args)
        {
            if (slot) {
                slot(args...);
            }
        }
    };

    mutable std::mutex                      _mx;
    std::vector<std::shared_ptr<TypedConn>> _connections;
};

} // namespace jb::core
