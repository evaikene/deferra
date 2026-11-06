#include "request.hpp"

#include <jb/core/event_loop.hpp>
#include <jb/core/object.hpp>
#include <jb/core/scoped_event_loop.hpp>
#include <jb/jobu/attribute_registry.hpp>
#include <jb/jobu/client/control_client.hpp>
#include <jb/net/local_socket.hpp>
#include <jb/rpc/client.hpp>

#include <cassert>
#include <utility>

namespace jb::php {

Request::Request() noexcept = default;

// Declaration order keeps the registry and transport alive until their users
// are destroyed. The receiver is separately reset first by Session::close().
struct Session::Bundle {
    jobu::StandardAttributeRegistry      attributes;
    net::LocalSocket                     socket;
    std::unique_ptr<rpc::Client>         raw;
    std::unique_ptr<jobu::ControlClient> typed;
    std::unique_ptr<core::Object>        receiver{std::make_unique<core::Object>()};
};

Request::~Request() noexcept
{
    // Zend can free native wrappers after RSHUTDOWN. Detach every surviving
    // session now, while the loop still exists; close() removes its registry entry.
    while (!_sessions.empty()) {
        _sessions.back()->close();
    }
    // ScopedEventLoop restores the prior binding and discards queued work without
    // dispatch. Every dependent Object has already been destroyed synchronously.
    _scope.reset();
}

auto Request::create_session() -> core::Result<std::unique_ptr<Session>, core::Error>
{
    // Object constructors use the current binding. A host temporarily attaching
    // another loop must not silently put this request's next session on that loop.
    if (_scope && core::EventLoop::current() != &_scope->loop()) {
        return core::Result<std::unique_ptr<Session>, core::Error>::failure(
            {.category = core::ErrorCategory::Conflict,
             .code     = "php.request.invalid_context",
             .message  = "The request event loop is not current on this thread"});
    }

    if (!_scope) {
        auto scope = core::ScopedEventLoop::create();
        if (!scope) {
            return core::Result<std::unique_ptr<Session>, core::Error>::failure(scope.error());
        }
        _scope = std::move(scope).value();
    }

    auto session = std::unique_ptr<Session>(new Session(*this));
    _sessions.push_back(session.get());
    return core::Result<std::unique_ptr<Session>, core::Error>::success(std::move(session));
}

auto Request::loop() const noexcept -> core::EventLoop*
{
    return _scope ? &_scope->loop() : nullptr;
}

Session::Session(Request& request)
    : _request(&request)
    , _bundle(std::make_unique<Bundle>())
{}

Session::~Session() noexcept
{
    close();
}

auto Session::close() noexcept -> void
{
    if (!_bundle) {
        return;
    }

    // No observer may reach wrapper state while client destructors cancel pending
    // calls. Native clients own their internal receiver fences and timers.
    _bundle->receiver.reset();
    _bundle.reset();

    std::erase(_request->_sessions, this);
    _request = nullptr;
}

auto Session::is_closed() const noexcept -> bool
{
    return !_bundle;
}

auto Session::request() const noexcept -> Request*
{
    return _request;
}

auto Session::attach_clients() -> core::Result<void, core::Error>
{
    if (!_bundle || _bundle->raw || _bundle->socket.state() != net::LocalSocketState::Connected) {
        return core::Result<void, core::Error>::failure({.category = core::ErrorCategory::Conflict,
                                                         .code     = "php.session.invalid_state",
                                                         .message = "Session requires an unattached connected socket"});
    }

    _bundle->raw   = std::make_unique<rpc::Client>(_bundle->socket);
    _bundle->typed = std::make_unique<jobu::ControlClient>(*_bundle->raw, _bundle->attributes);
    return core::Result<void, core::Error>::success();
}

auto Session::receiver() const noexcept -> core::Object&
{
    assert(_bundle);
    return *_bundle->receiver;
}

auto Session::socket() const noexcept -> net::LocalSocket&
{
    assert(_bundle);
    return _bundle->socket;
}

auto Session::typed() const noexcept -> jobu::ControlClient&
{
    assert(_bundle && _bundle->typed);
    return *_bundle->typed;
}

} // namespace jb::php
