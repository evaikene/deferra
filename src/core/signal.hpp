#pragma once

#include "connectable.hpp"
#include "event_loop.hpp"
#include "thread_context.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

namespace jb::core {

/// Type of signal-slot connection
enum class ConnectionType : std::uint8_t {
    Direct, ///< Call slot immediately on the emitting thread (like a direct function call)
    Queued, ///< Post slot to the receiver's event loop
    Auto,   ///< Direct if same thread, Queued otherwise
};

template <typename... Args>
class Signal {
public:

    using Slot = std::function<void(Args...)>;

    /// Connect the signal to a free function or lambda
    /// @param[in] receiver The receiver object for lifetime tracking (can be nullptr)
    /// @param[in] slot The slot to connect
    /// @param[in] type The connection type (default: Auto)
    void connect(Connectable* receiver, Slot slot, ConnectionType type = ConnectionType::Auto)
    {
        assert(type != ConnectionType::Queued || (receiver && receiver->event_loop()));

        auto c      = std::make_shared<Connection>();
        c->receiver = receiver;
        if (receiver) {
            c->token = receiver->token();
        }
        c->slot = std::move(slot);
        c->type = type;

        std::lock_guard lock{_connections_mx};
        _connections.push_back(std::move(c));
    }

    /// Disconnect from the signal
    /// @param[in] receiver The receiver object to disconnect
    /// @param[in] slot The slot to disconnect
    ///
    /// This method disconnects all the connections matching the receiver and slot type. Note that
    /// the slot type is used and may disconnect multiple connections if the same slot type is
    /// connected multiple times.
    void disconnect(Connectable* receiver, Slot slot)
    {
        std::lock_guard lock(_connections_mx);
        _connections.erase(std::remove_if(_connections.begin(),
                                          _connections.end(),
                                          [receiver, &slot](ConnectionPtr const& e) -> auto {
                                              return e->receiver == receiver &&
                                                     e->slot.target_type() == slot.target_type();
                                          }),
                           _connections.end());
    }

    /// Disconnect from the signal
    /// @param[in] receiver The receiver object to disconnect
    ///
    /// Disconnects all the connections matching the receiver.
    void disconnect(Connectable* receiver)
    {
        std::lock_guard lock(_connections_mx);
        _connections.erase(
            std::remove_if(_connections.begin(),
                           _connections.end(),
                           [receiver](ConnectionPtr const& e) -> auto { return e->receiver == receiver; }),
            _connections.end());
    }

    /// Emit the signal with the given arguments
    void emit(Args... args)
    {
        auto* sender_event_loop = ThreadCtx::current()->event_loop();

        // cleanup connections with destroyed receivers before emitting
        cleanup_connections();

        // make a snapshot of the connections to call them outside the lock
        Connections connections;
        {
            std::lock_guard lock(_connections_mx);
            connections = _connections;
        }

        // call or post the slots for all the connections
        for (auto const& e : connections) {

            // double-check that the receiver is still alive
            if (e->receiver) {
                auto token = e->token.lock();
                if (!token) {
                    continue;
                }
            }

            // detect connection type and call or post the slot
            if (resolve_type(*e, sender_event_loop) == ConnectionType::Direct) {
                e->slot(args...);
            }
            else {
                post_queued_signal(*e, args...);
            }
        }
    }

private:

    struct Connection {
        Connectable*                     receiver = nullptr;
        std::weak_ptr<priv::ObjectToken> token;
        Slot                             slot;
        ConnectionType                   type = ConnectionType::Auto;
    };

    using ConnectionPtr = std::shared_ptr<Connection>;
    using Connections   = std::vector<ConnectionPtr>;

    static auto resolve_type(Connection const& e, EventLoop* sender_event_loop) -> ConnectionType
    {
        if (e.type != ConnectionType::Auto) {
            return e.type;
        }
        auto* target_event_loop = e.receiver ? e.receiver->event_loop() : nullptr;
        if (target_event_loop == nullptr || target_event_loop == sender_event_loop) {
            return ConnectionType::Direct;
        }
        return ConnectionType::Queued;
    }

    void post_queued_signal(Connection const& e, Args... args)
    {
        auto* target_event_loop = e.receiver ? e.receiver->event_loop() : nullptr;

        assert(e.receiver && target_event_loop);

        auto*                            receiver = e.receiver;
        std::weak_ptr<priv::ObjectToken> token;
        if (receiver) {
            token = e.token;
        }
        auto slot = e.slot;
        target_event_loop->post(
            [receiver, token = std::move(token), slot = std::move(slot), args...]() mutable -> void {
                if (!receiver || token.lock()) {
                    slot(args...);
                }
            });
    }

    void cleanup_connections()
    {
        // remove connections with receivers that have been destroyed
        std::lock_guard lock(_connections_mx);
        _connections.erase(
            std::remove_if(_connections.begin(),
                           _connections.end(),
                           [](ConnectionPtr const& e) -> auto { return e->receiver && !e->token.lock(); }),
            _connections.end());
    }

    std::vector<std::shared_ptr<Connection>> _connections;
    std::mutex                               _connections_mx;
};

} // namespace jb::core
