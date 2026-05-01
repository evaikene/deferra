#pragma once

#include "event_loop.hpp"
#include "thread_context.hpp"

#include <cassert>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace jb::core {

/// Type of signal-slot connection
enum class ConnectionType : std::uint8_t {
    Direct, ///< Call slot immediately on the emitting thread (like a direct function call)
    Queued, ///< Post slot to the receiver's event loop
    Auto,   ///< Direct if same thread, Queued otherwise
};

using connection_id_t = std::uint64_t;

/// Signal-slot connection
/// Represent a signal-slot connection that can be stored and used to manage the
/// connection (e.g., disconnecting).
struct Connection {
    connection_id_t id{0}; ///< Unique connection identifier (zero means invalid connection)

    explicit operator bool() const noexcept { return id != 0; }
};

template <typename... Args>
class Signal {
public:

     Signal() = default;

     ~Signal() { disconnect_all(); }
 
     /// Connect to a member function of an Object-derived @p receiver.
     /// @param[in] receiver Receiver object derived from Object
     /// @param[in] slot Any callable compatible with `void(Args...)`
     /// @param[in] type Connection type (default: Auto)
     /// @return A connection handle
     ///
     /// The slot type is checked at compile time: it must be callable with `Args...`.
     template <typename Receiver, typename Slot>
    auto connect(Receiver& receiver, Slot&& slot, ConnectionType type = ConnectionType::Auto);

    /// Connect the signal to a member function directly
    /// @param[in] receiver The receiver object for lifetime tracking (can be nullptr)
    /// @param[in] fn The member function to connect
    /// @param[in] type The connection type (default: Auto)
    /// @return Connection object representing the connection
    template <typename Receiver, typename Member>
        requires std::is_base_of_v<Connectable, Receiver>&& std::is_invocable_v<Member, Receiver*, Args...>
    auto connect(Receiver* receiver, Member fn, ConnectionType type = ConnectionType::Auto) -> Connection
    {
        auto slot = [receiver, fn = std::move(fn)](Args... args) -> void { std::invoke(fn, receiver, args...); };
        return connect(receiver, std::move(slot), type);
    }

    /// Disconnect the connection from the signal
    /// @param[in] c The connection to disconnect
    auto connect(Receiver* receiver, Slot&& slot, ConnectionType type = ConnectionType::Auto) -> Connection;

    /// Connect to a callable with no Object receiver.
    /// @param[in] callable Any callable compatible with `void(Args...)`
    /// @return A connection handle
    ///
    /// Lambda connections are always Direct: the callable is invoked in the emitting
    /// thread.
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
        EventLoop*                   receiver_loop{nullptr}; /// nullptr for Direct/lambda
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
