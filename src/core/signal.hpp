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
        auto* target_event_loop = receiver ? receiver->event_loop() : nullptr;

        assert(type != ConnectionType::Queued || receiver != nullptr);
        assert(type != ConnectionType::Queued || target_event_loop != nullptr);

        Entry entry;
        entry.receiver = receiver;
        if (receiver) {
            entry.token             = receiver->token();
            entry.target_event_loop = target_event_loop;
        }
        entry.slot = std::move(slot);
        entry.type = type;

        std::lock_guard lock{_entries_mx};
        _entries.push_back(std::move(entry));
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
        std::lock_guard lock(_entries_mx);
        _entries.erase(std::remove_if(_entries.begin(),
                                      _entries.end(),
                                      [receiver, &slot](Entry const& e) -> auto {
                                          return e.receiver == receiver && e.slot.target_type() == slot.target_type();
                                      }),
                       _entries.end());
    }

    /// Disconnect from the signal
    /// @param[in] receiver The receiver object to disconnect
    ///
    /// Disconnects all the connections matching the receiver.
    void disconnect(Connectable* receiver)
    {
        std::lock_guard lock(_entries_mx);
        _entries.erase(std::remove_if(_entries.begin(),
                                      _entries.end(),
                                      [receiver](Entry const& e) -> auto { return e.receiver == receiver; }),
                       _entries.end());
    }

    void emit(Args... args)
    {
        auto* sender_event_loop = ThreadCtx::current()->event_loop();

        std::lock_guard lock(_entries_mx);

        auto it = _entries.begin();
        while (it != _entries.end()) {

            // ensure the receiver is still alive
            if (it->receiver) {
                auto token = it->token.lock();
                if (!token) {
                    it = _entries.erase(it);
                    continue;
                }
            }

            // detect connection type and call or post the slot
            if (resolve_type(*it, sender_event_loop) == ConnectionType::Direct) {
                it->slot(args...);
            }
            else {
                post_queued_signal(*it, args...);
            }

            ++it;
        }
    }

private:

    struct Entry {
        Connectable*                     receiver = nullptr;
        std::weak_ptr<priv::ObjectToken> token;
        EventLoop*                       target_event_loop = nullptr;
        Slot                             slot;
        ConnectionType                   type = ConnectionType::Auto;
    };

    static auto resolve_type(Entry const& e, EventLoop* sender_event_loop) -> ConnectionType
    {
        if (e.type != ConnectionType::Auto) {
            return e.type;
        }
        if (e.target_event_loop == nullptr || e.target_event_loop == sender_event_loop) {
            return ConnectionType::Direct;
        }
        return ConnectionType::Queued;
    }

    void queue_task(EventLoop* event_loop, std::function<void()> task);

    void post_queued_signal(Entry const& e, Args... args)
    {
        assert(e.target_event_loop);

        auto* receiver = e.receiver;
        std::weak_ptr<priv::ObjectToken> token;
        if (receiver) {
            token = e.token;
        }
        auto slot  = e.slot;
        e.target_event_loop->post([receiver, token = std::move(token), slot = std::move(slot), args...]() mutable -> void {
            if (!receiver || token.lock()) {
                slot(args...);
            }
        });
    }

    std::vector<Entry> _entries;
    std::mutex         _entries_mx;
};

} // namespace jb::core
