#pragma once

#include "operation.hpp"
#include "request.hpp"

#include <jb/core/event_loop_types.hpp>
#include <jb/jobu/client/control_client.hpp>

#include <chrono>
#include <cstdint>
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

enum class ConnectionState : std::uint8_t {
    Connecting,
    Ready,
    Disconnected,
    Closed,
    Unusable
};

/// Request-affine native Client state. Owns no Zend values; Request closes its
/// session before loop teardown, even if this wrapper is destroyed afterwards.
class Client final {
public:
    Client() noexcept;
    ~Client() noexcept;
    Client(Client const&)                    = delete;
    auto operator=(Client const&) -> Client& = delete;

    /// Eagerly connect and validate API 1.7 plus the v1 capability set. Failure
    /// closes partial state. Initialize once; replacement bundles repeat these checks.
    auto initialize(Request&                  request,
                    std::string const&        endpoint,
                    core::TimePoint           deadline,
                    std::chrono::milliseconds timeout) -> core::Result<void, core::Error>;

    /// Idempotent explicit close, also safe after RSHUTDOWN.
    auto close() noexcept -> void;

    /// Execute a frozen typed operation within one total deadline. Transport loss
    /// may replace the bundle and replay only according to the operation policy.
    /// Uncertain unsafe mutations return promptly, preserving the caller's inputs.
    auto call(core::TimePoint deadline, Operation const& operation) -> Reply;

    /// Compute one deadline using the override or stored constructor timeout.
    /// Callers performing multiple internal reads pass this same deadline to call().
    auto deadline(core::TimePoint start, std::optional<std::chrono::milliseconds> override = std::nullopt) const
        -> core::Result<core::TimePoint, core::Error>;
    auto state() const noexcept -> ConnectionState;

    /// Borrow the ready typed client for a Submit callback; null after close.
    auto typed() const noexcept -> jobu::ControlClient*;
    auto timeout() const noexcept -> std::chrono::milliseconds;

private:
    auto connect(core::TimePoint deadline, bool recovering) -> core::Result<void, core::Error>;
    auto observe_terminal() -> void;

    std::string               _endpoint;
    ConnectionState           _state{ConnectionState::Closed};
    std::unique_ptr<Session>  _session;
    std::chrono::milliseconds _timeout{10000};
    bool                      _initialized{false};
};

} // namespace jb::php
