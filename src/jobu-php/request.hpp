#pragma once

#include <jb/core/error.hpp>
#include <jb/core/result.hpp>

#include <memory>
#include <vector>

namespace jb::core {
class EventLoop;
class Object;
class ScopedEventLoop;
} // namespace jb::core

namespace jb::net {
class LocalSocket;
}

namespace jb::rpc {
class Client;
}

namespace jb::jobu {
class ControlClient;
}

namespace jb::php {

class Session;

/// Private extension owner, created by RINIT and destroyed by RSHUTDOWN.
/// All access belongs to the PHP calling thread. The registry borrows sessions;
/// it never owns PHP objects or keeps them alive. The loop is attached lazily.
class Request final {
public:
    Request() noexcept;
    ~Request() noexcept;
    Request(Request const&)                    = delete;
    auto operator=(Request const&) -> Request& = delete;

    /// Creates a separate unconnected bundle on this request's shared loop.
    /// Returns core loop initialization/reentrancy errors, or php.request.invalid_context
    /// if a host has temporarily replaced this request's binding; no session is published.
    auto create_session() -> core::Result<std::unique_ptr<Session>, core::Error>;

    /// Borrow the loop after successful session creation; null before first use.
    auto loop() const noexcept -> core::EventLoop*;

private:
    friend class Session;
    std::unique_ptr<core::ScopedEventLoop> _scope;
    std::vector<Session*>                  _sessions;
};

/// Independent transport bundle. Its PHP wrapper owns this object; Request only
/// borrows it. Shutdown closes and detaches it even if the wrapper is freed later.
/// This layer owns no Zend references, performs no waits and implements no retry.
class Session final {
public:
    ~Session() noexcept;
    Session(Session const&)                    = delete;
    auto operator=(Session const&) -> Session& = delete;

    /// Fence receiver-aware callbacks before destroying typed/raw/socket objects.
    /// Idempotent; safe after request shutdown. Must run on the owning thread.
    auto close() noexcept -> void;
    auto is_closed() const noexcept -> bool;

    /// Attach raw/typed clients once the socket is connected. No handshake or I/O
    /// is initiated here. Reject closed, unconnected, or already attached bundles.
    auto attach_clients() -> core::Result<void, core::Error>;

    /// Borrow bundle components while open. All borrowers must stop using them
    /// before close/RSHUTDOWN. Observers capturing wrapper state use receiver().
    /// typed() additionally requires successful attach_clients().
    auto receiver() const noexcept -> core::Object&;
    auto socket() const noexcept -> net::LocalSocket&;
    auto typed() const noexcept -> jobu::ControlClient&;

private:
    friend class Request;
    explicit Session(Request& request);
    struct Bundle;
    Request*                _request;
    std::unique_ptr<Bundle> _bundle;
};

/// Borrow the active module request; null outside RINIT/RSHUTDOWN boundaries.
auto current_request() noexcept -> Request*;

} // namespace jb::php
