#pragma once

#include "connectable.hpp"
#include "event_loop.hpp"
#include "thread_context.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <functional>
#include <mutex>
#include <type_traits>
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

    using Slot = std::function<void(Args...)>;

    /// Connect the signal to a free function or lambda
    /// @param[in] receiver The receiver object for lifetime tracking (can be nullptr)
    /// @param[in] slot The slot to connect
    /// @param[in] type The connection type (default: Auto)
    /// @return Connection object representing the connection
    auto connect(Connectable* receiver, Slot slot, ConnectionType type = ConnectionType::Auto) -> Connection
    {
        assert(type != ConnectionType::Queued || (receiver && receiver->event_loop()));

        auto c      = std::make_shared<Entry>();
        c->id       = s_next_id.fetch_add(1, std::memory_order_relaxed);
        c->receiver = receiver;
        if (receiver) {
            c->token = receiver->token();
        }
        c->slot = std::move(slot);
        c->type = type;

        std::lock_guard lock{_entries_mx};
        _entries.push_back(std::move(c));

        return {_entries.back()->id};
    }

    /// Connect the signal to a member function directly
    /// @param[in] receiver The receiver object for lifetime tracking (can be nullptr)
    /// @param[in] fn The member function to connect
    /// @param[in] type The connection type (default: Auto)
    /// @return Connection object representing the connection
    template <typename Receiver,
              typename Member,
              typename = std::enable_if_t<std::is_base_of_v<Connectable, Receiver>>,
              typename = std::enable_if_t<std::is_invocable_v<Member, Receiver*, Args...>>>
    auto connect(Receiver* receiver, Member fn, ConnectionType type = ConnectionType::Auto) -> Connection
    {
        auto slot = [receiver, fn = std::move(fn)](Args... args) -> auto { (receiver->*fn)(args...); };
        return connect(receiver, std::move(slot), type);
    }

    /// Disconnect the connection from the signal
    /// @param[in] c The connection to disconnect
    ///
    /// This method disconnects the specified connection from the signal. If the connection
    /// is invalid or has already been disconnected, this method does nothing.
    void disconnect(Connection c)
    {
        std::lock_guard lock(_entries_mx);
        _entries.erase(
            std::remove_if(_entries.begin(), _entries.end(), [c](EntryPtr const& e) -> auto { return e->id == c.id; }),
            _entries.end());
    }

    /// Disconnect all the slots from the signal associated with the given receiver
    /// @param[in] receiver The receiver object to disconnect
    ///
    /// Disconnects all the connections matching the receiver. If the receiver is nullptr,
    /// disconnects all the connections without a receiver.
    void disconnect(Connectable* receiver)
    {
        std::lock_guard lock(_entries_mx);
        _entries.erase(std::remove_if(_entries.begin(),
                                      _entries.end(),
                                      [receiver](EntryPtr const& e) -> auto { return e->receiver == receiver; }),
                       _entries.end());
    }

    /// Emit the signal with the given arguments
    void emit(Args... args)
    {
        auto* sender_event_loop = ThreadCtx::current()->event_loop();

        // cleanup connections with destroyed receivers before emitting
        cleanup_connections();

        // make a snapshot of the connections to call them outside the lock
        Entries connections;
        {
            std::lock_guard lock(_entries_mx);
            connections = _entries;
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

    /// Emit via operator()
    void operator()(Args... args) { emit(std::forward<Args>(args)...); }

private:

    struct Entry {
        connection_id_t                  id{0};
        Connectable*                     receiver{nullptr};
        std::weak_ptr<priv::ObjectToken> token;
        Slot                             slot;
        ConnectionType                   type{ConnectionType::Auto};
    };

    using EntryPtr = std::shared_ptr<Entry>;
    using Entries  = std::vector<EntryPtr>;

    static auto resolve_type(Entry const& e, EventLoop* sender_event_loop) -> ConnectionType
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

    void post_queued_signal(Entry const& e, Args... args)
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
                // double-check that the receiver is still alive before calling the slot
                if (!receiver || token.lock()) {
                    slot(args...);
                }
            });
    }

    void cleanup_connections()
    {
        // remove connections with receivers that have been destroyed
        std::lock_guard lock(_entries_mx);
        _entries.erase(std::remove_if(_entries.begin(),
                                      _entries.end(),
                                      [](EntryPtr const& e) -> auto { return e->receiver && !e->token.lock(); }),
                       _entries.end());
    }

    inline static std::atomic<connection_id_t> s_next_id;
    std::vector<std::shared_ptr<Entry>> _entries;
    std::mutex                          _entries_mx;
};

} // namespace jb::core
