#pragma once

#include "request.hpp"

#include <jb/core/event_loop_types.hpp>
#include <jb/jobu/client/control_client.hpp>

#include <chrono>
#include <functional>
#include <memory>
#include <string>

namespace jb::php {

using Failure = jobu::ControlFailure;
using Reply   = core::Result<jobu::ControlReply, Failure>;
using Submit  = std::function<core::Result<jobu::ControlCallId, core::Error>(jobu::ControlCallOptions)>;

/// Checked positive timeout conversion. The start belongs to the whole PHP operation.
auto make_deadline(core::TimePoint start, std::chrono::milliseconds timeout)
    -> core::Result<core::TimePoint, core::Error>;

/// Wait for one typed operation on the current idle owner loop. Submit must only
/// invoke a typed method on client, without pumping events or destroying client.
/// Both client and loop must outlive this call. No retry or PHP callback occurs.
/// Cancellation retires local observation only; failures retain outcome uncertainty.
auto call_synchronously(jobu::ControlClient& client,
                        core::EventLoop&     loop,
                        core::TimePoint      deadline,
                        Submit const&        submit) -> Reply;

/// Request-affine native Client state. Owns no Zend values; Request closes its
/// session before loop teardown, even if this wrapper is destroyed afterwards.
class Client final {
public:
    Client() noexcept;
    ~Client() noexcept;
    Client(Client const&)                    = delete;
    auto operator=(Client const&) -> Client& = delete;

    /// Eagerly connect and validate API 1.7 plus the v1 capability set. Failure
    /// closes partial state. Initialize once; no connection recovery is attempted.
    auto initialize(Request&                  request,
                    std::string const&        endpoint,
                    core::TimePoint           deadline,
                    std::chrono::milliseconds timeout) -> core::Result<void, core::Error>;

    /// Idempotent explicit close, also safe after RSHUTDOWN.
    auto close() noexcept -> void;

    /// Dispatch a typed method with an operation-wide deadline. Closed sessions
    /// fail before submit is invoked; the callback must obey Submit's contract.
    auto call(core::TimePoint deadline, Submit const& submit) const -> Reply;

    /// Borrow the ready typed client for a Submit callback; null after close.
    auto typed() const noexcept -> jobu::ControlClient*;
    auto timeout() const noexcept -> std::chrono::milliseconds;

private:
    std::unique_ptr<Session>  _session;
    std::chrono::milliseconds _timeout{10000};
    bool                      _initialized{false};
};

} // namespace jb::php
