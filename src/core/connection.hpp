#pragma once

#include <atomic>
#include <cstdint>
#include <memory>

namespace jb::core {

/// Connection types
/// Controls how a signal is delivered to a connected slot
///
/// Auto -   Direct if sender are received share the same event loop;
///          Queued otherwise. This is the default.
/// Direct - The slot is called synchronously in the emitting thread, regdardless
///          of which event loop the received lives on.
/// Queued - The invocation is posted to the receiver's event loop and executed in
///          the enxt time that loop processes events. Args must be copyable.
enum class ConnectionType : std::uint8_t {
    Auto,   ///< Direct if same thread, Queued otherwise (default)
    Direct, ///< Always synchronous; invoked in the sender's thread
    Queued, ///< Alsays asynchronous; posted to the receiver's event loop
};

namespace priv {

/// Type-erased base for a single signal-slot connection record.
///
/// Signal<Args...> inherits this in its internal TypedConn structure. The `active`
/// flag is the only shared state; all other data is in the typed subclass. Defined
/// here (rather than in a private header) because Connection holds a
/// shared_ptr<ConnectionBase> and the type must be complete wherever Connection
/// objects are destroyed.
struct ConnectionBase {
    std::atomic<bool> active{true};

    virtual ~ConnectionBase() = default;

    /// Mark this connection as dead. Idempotent and thread-safe
    void deactivate() noexcept { active.store(false, std::memory_order_release); }
};

} // namespace priv

/// Lightweight, copyable handle to a signal-slot connection.
///
/// Returned by Signal::connect(). All copies of a Connection refer to the same
/// underlying record; calling disconnect() on any copy deactivates them all.
///
/// Lifetime:
///
/// - The connection remains active until:
///   - disconnect() is called on any copy of the handle;
///   - the sending Signal is destroyed (its owner Object is deleted);
///   - the receiving Object is destroyed.
/// - Connections are NOT automatically cleaned up when the Connection handle
///   goes out of the scope - you must call disconnect() explicitly, or let the
///   sender/receiver lifetime manage it.
///
/// Thread-safety:
///
/// is_valid() and disconnect() are thread-safe.
class Connection {
public:

    /// Constructors / assignment - all defaulted; Connection is freely copyable
    Connection()                                         = default;
    Connection(Connection const&)                        = default;
    Connection(Connection&&) noexcept                    = default;
    auto operator=(Connection const&) -> Connection&     = default;
    auto operator=(Connection&&) noexcept -> Connection& = default;

    /// Returns true if the handle refers to a live (non-disconnected) connection
    [[nodiscard]] auto is_valid() const noexcept -> bool
    {
        auto p = _data.lock();
        return p && p->active.load(std::memory_order_acquire);
    }

    /// Deactivates the slot.
    ///
    /// After this call, the Signal will skip the slot on future emissions.
    /// Already-in-flight queued deliveries may still execute once. Calling
    /// disconnect() more than once is safe.
    void disconnect() noexcept
    {
        if (auto p = _data.lock()) {
            p->deactivate();
        }
        _data.reset();
    }

private:

    template <typename...>
    friend class Signal;

    std::weak_ptr<priv::ConnectionBase> _data;

    explicit Connection(std::shared_ptr<priv::ConnectionBase> const& data) noexcept
        : _data(data)
    {}
};

} // namespace jb::core
