#pragma once

// IWYU pragma: private include "object.hpp"
#include "object.hpp"

// Internal header included only from object.hpp - not part of the public API
// Do NOT include this file directly.

#include "logging.hpp"

#include <algorithm>
#include <type_traits>

namespace jb::core {

// Signal<Args...>::connect - member-function / callable-with-receiver slot

template <typename... Args>
template <typename Receiver, typename Slot>
auto Signal<Args...>::connect(Receiver* receiver, Slot&& slot, ConnectionType type) -> Connection
{
    static_assert(std::is_base_of_v<Object, Receiver>,
                  "Signal::connect: Receiver must be derived from Object");
    if (!receiver) {
        log_fatal("Signal::connect: receiver must not be null");
        return {};
    }

    auto conn           = std::make_shared<TypedConn>();
    conn->conn_type     = type;
    conn->receiver      = receiver;

    if constexpr (std::is_member_function_pointer_v<std::decay_t<Slot>>) {
        // member-function pointer: bind the receiver so the stored function only
        // needs to be called with (Args...)
        conn->slot = [r = receiver, s = std::forward<Slot>(slot)](Args... args) -> void { (r->*s)(args...); };
    }
    else {
        // already a free callable (lambda, std::function, etc.) - store directly
        // the receiver is still registered for lifetime tracking.
        conn->slot = std::forward<Slot>(slot);
    }

    {
        std::lock_guard<std::mutex> lock{_mx};
        _connections.push_back(conn);
    }

    // let the receiver track this connection so it can deactivate on death
    receiver->register_connection(conn);

    return Connection{conn};
}

// Signal<Args...>::connect - context-free lambda (always Direct)
template <typename... Args>
template <typename Callable>
auto Signal<Args...>::connect(Callable&& callable) -> Connection
{
    auto conn           = std::make_shared<TypedConn>();
    conn->slot          = std::forward<Callable>(callable);
    conn->conn_type     = ConnectionType::Direct;
    conn->receiver      = nullptr;

    std::lock_guard<std::mutex> lock{_mx};
    _connections.push_back(conn);

    return Connection{conn};
}

// Signal<Args...>::disconnect

template <typename... Args>
void Signal<Args...>::disconnect(Connection const& conn) noexcept
{
    if (auto p = conn._data.lock()) {
        p->deactivate();
    }
}

// Signal<Args...>::disconnect_all

template <typename... Args>
void Signal<Args...>::disconnect_all() noexcept
{
    std::lock_guard<std::mutex> lock{_mx};
    for (auto& c : _connections) {
        c->deactivate();
    }
    _connections.clear();
}

// Signal<Args...>::emit

template <typename... Args>
void Signal<Args...>::emit(Object* sender, Args... args)
{
    //--- 1. snapshot the live connections under the lock
    // We prune dead connections while holding the mutex, then release it before
    // invoking any slot (re-entrancy: slots may connect/disconnect)
    std::vector<std::shared_ptr<TypedConn>> snapshot;
    {
        std::lock_guard<std::mutex> lock{_mx};

        _connections.erase(
            std::remove_if(_connections.begin(), _connections.end(), [](auto const& c) -> auto {
                return !c->active.load(std::memory_order_relaxed);
            }),
            _connections.end());
        snapshot = _connections;
    }

    //--- 2. dispatch each connection
    for (auto& c : snapshot) {
        if (!c->active.load(std::memory_order_acquire)) {
            continue; // deactivated between snapshot and now
        }

        // determine whether to call inline (Direct) or post (Queued)
        auto const call_direct = [&]() -> bool {
            switch (c->conn_type) {
                case ConnectionType::Direct: {
                    return true;
                }

                case ConnectionType::Queued: {
                    return false;
                }

                case ConnectionType::Auto:
                default: {
                    // direct when the sender and receiver share the same EventLoop,
                    // or when the receiver has no EventLoop (e.g. pre-Application).
                    auto* receiver_loop = c->receiver ? c->receiver->event_loop() : nullptr;
                    return !receiver_loop || (sender->event_loop() == receiver_loop);
                }
            }
        }();

        if (call_direct) {
            // synchronous: invoke in the current (sender's) thread
            c->invoke(args...);
        }
        else {
            // asynchronous: capture args by value and post to the receiver loop
            c->receiver->event_loop()->post([c, ...captured_args = args]() mutable -> auto {
                if (c->active.load(std::memory_order_acquire)) {
                    c->invoke(captured_args...);
                }
            });
        }
    }
}

} // namespace jb::core
