#pragma once

// IWYU pragma: private include "object.hpp"
// #include "object.hpp"

// Internal header included only from object.hpp - not part of the public API
// Do NOT include this file directly.

#include "logging.hpp"
#include "object_priv.hpp"

#include <algorithm>
#include <tuple>
#include <type_traits>

namespace jb::core {

// Signal<Args...>::connect - member-function / callable-with-receiver slot

template <typename... Args>
template <typename Receiver, typename Slot>
auto Signal<Args...>::connect(Receiver* receiver, Slot&& slot, ConnectionType type) const -> Connection
{
    static_assert(std::is_base_of_v<Object, Receiver>, "Signal::connect: Receiver must be derived from Object");
    if (!receiver) {
        log_fatal("Signal::connect: receiver must not be null");
        return {};
    }

    auto conn               = std::make_shared<TypedConn>();
    conn->conn_type         = type;
    conn->receiver          = receiver;
    conn->receiver_lifetime = receiver->lifetime();

    if constexpr (std::is_member_function_pointer_v<std::decay_t<Slot>>) {
        static_assert(std::is_invocable_r_v<void, std::decay_t<Slot>, Receiver*, Args const&...>,
                      "Signal::connect: member slot must be callable with the receiver and borrowed signal arguments");
        // member-function pointer: bind the receiver so the stored function only
        // needs to be called with borrowed signal arguments.
        conn->slot = [r = receiver, s = std::forward<Slot>(slot)](Args const&... args) -> void { (r->*s)(args...); };
    }
    else {
        static_assert(std::is_invocable_r_v<void, std::decay_t<Slot>&, Args const&...>,
                      "Signal::connect: slot must be callable with borrowed signal arguments");
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
auto Signal<Args...>::connect(Callable&& callable) const -> Connection
{
    static_assert(std::is_invocable_r_v<void, std::decay_t<Callable>&, Args const&...>,
                  "Signal::connect: callable must accept borrowed signal arguments");

    auto conn       = std::make_shared<TypedConn>();
    conn->slot      = std::forward<Callable>(callable);
    conn->conn_type = ConnectionType::Direct;
    conn->receiver  = nullptr;

    std::lock_guard<std::mutex> lock{_mx};
    _connections.push_back(conn);

    return Connection{conn};
}

// Signal<Args...>::disconnect

template <typename... Args>
void Signal<Args...>::disconnect(Connection const& conn) const noexcept
{
    if (auto p = conn._data.lock()) {
        p->deactivate();
    }
}

// Signal<Args...>::disconnect_all

template <typename... Args>
void Signal<Args...>::disconnect_all() const noexcept
{
    std::lock_guard<std::mutex> lock{_mx};
    for (auto& c : _connections) {
        c->deactivate();
    }
    _connections.clear();
}

// Signal<Args...>::emit

template <typename... Args>
void Signal<Args...>::emit(Object* sender, Args const&... args) const
{
    //--- 1. snapshot the live connections under the lock
    // We prune dead connections while holding the mutex, then release it before
    // invoking any slot (re-entrancy: slots may connect/disconnect)
    std::vector<std::shared_ptr<TypedConn>> snapshot;
    {
        std::lock_guard<std::mutex> lock{_mx};

        _connections.erase(
            std::remove_if(_connections.begin(),
                           _connections.end(),
                           [](auto const& c) -> auto { return !c->active.load(std::memory_order_relaxed); }),
            _connections.end());
        snapshot = _connections;
    }

    //--- 2. resolve routing before invoking user code
    struct Delivery {
        std::shared_ptr<TypedConn>            connection;
        EventLoop*                            event_loop{nullptr}; // nullptr means Direct
        std::shared_ptr<priv::ObjectLifetime> receiver_lifetime;
    };

    std::vector<Delivery> deliveries;
    deliveries.reserve(snapshot.size());
    bool has_queued_delivery = false;

    for (auto const& c : snapshot) {
        if (!c->active.load(std::memory_order_acquire)) {
            continue;
        }

        if (c->conn_type == ConnectionType::Direct) {
            deliveries.push_back({c});
            continue;
        }

        auto lifetime = c->receiver_lifetime.lock();
        if (!lifetime) {
            continue;
        }

        std::unique_lock lock{lifetime->event_loop_mx};
        if (!lifetime->alive.load(std::memory_order_acquire)) {
            continue;
        }

        auto* event_loop = lifetime->event_loop;
        if (c->conn_type == ConnectionType::Auto && (!event_loop || sender->event_loop() == event_loop)) {
            // Auto is direct when the sender and receiver share an EventLoop,
            // or when the receiver has no EventLoop (e.g. pre-Application).
            deliveries.push_back({c});
            continue;
        }

        if (!event_loop) {
            log_error("Signal::emit: cannot post to receiver with no EventLoop");
            continue;
        }

        deliveries.push_back({c, event_loop, std::move(lifetime)});
        has_queued_delivery = true;
    }

    //--- 3. snapshot queued arguments before any direct slot can mutate source state
    using QueuedPayload = std::tuple<Args...>;
    std::shared_ptr<QueuedPayload const> queued_payload;
    if (has_queued_delivery) {
        queued_payload = std::make_shared<QueuedPayload const>(args...);
    }

    //--- 4. dispatch in connection order
    for (auto const& delivery : deliveries) {
        auto const& c = delivery.connection;
        if (!c->active.load(std::memory_order_acquire)) {
            continue;
        }

        if (!delivery.event_loop) {
            c->invoke(args...);
            continue;
        }

        auto const&     lifetime = delivery.receiver_lifetime;
        std::lock_guard lock{lifetime->event_loop_mx};
        if (!c->active.load(std::memory_order_acquire) || !lifetime->alive.load(std::memory_order_acquire) ||
            lifetime->event_loop != delivery.event_loop) {
            continue;
        }

        Object::post_event_delivery(delivery.event_loop,
                                    c->receiver,
                                    lifetime,
                                    [c, payload = queued_payload]() -> void {
                                        if (!c->active.load(std::memory_order_acquire)) {
                                            return;
                                        }
                                        std::apply([&c](auto const&... values) -> void { c->invoke(values...); },
                                                   *payload);
                                    });
    }
}

} // namespace jb::core
