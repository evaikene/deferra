#pragma once

#include <jb/core/error.hpp>
#include <jb/core/result.hpp>
#include <jb/jobu/client/control_client.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace jb::php {

/// Replay policy belongs to the operation's effect, not to its transport error.
enum class ReplayPolicy : std::uint8_t {
    Read,
    DesiredState,
    Keyed,
    Unsafe
};

/// Known read/desired-state/keyed methods; every other name defaults to Unsafe.
auto replay_policy(std::string_view name) noexcept -> ReplayPolicy;

/// Fill an omitted key using fresh OS-backed entropy; explicit keys are unchanged.
/// Reject explicit keys outside the protocol's 1–128 UTF-8 byte contract.
auto ensure_idempotency_key(std::optional<std::string>& key) -> core::Result<void, core::Error>;

/// Frozen private typed invocation. The method name must match the member passed
/// to bind(). Owns request values, never a typed-client pointer or a PHP object.
/// Typed encoders deterministically reproduce the same parameters on each attempt;
/// only the RPC correlation ID and remaining local timeout may change.
class Operation final {
public:
    using Accepted = core::Result<jobu::ControlCallId, core::Error>;

    /// Copy/move the request into the descriptor before the first attempt. The
    /// three keyed request types receive their key exactly once, before capture.
    template <typename Request>
    static auto bind(std::string name,
                     Accepted (jobu::ControlClient::*method)(Request const&, jobu::ControlCallOptions),
                     Request request) -> core::Result<Operation, core::Error>
    {
        std::optional<std::string> key;
        if constexpr (std::is_same_v<Request, jobu::CreateQueueRequest> ||
                      std::is_same_v<Request, jobu::CreateJobRequest> || std::is_same_v<Request, jobu::RunNowRequest>) {
            auto generated = ensure_idempotency_key(request.idempotency_key);
            if (!generated) {
                return core::Result<Operation, core::Error>::failure(generated.error());
            }
            key = request.idempotency_key;
        }
        return core::Result<Operation, core::Error>::success(Operation(
            std::move(name),
            [method, request = std::move(request)](jobu::ControlClient& client, jobu::ControlCallOptions options) {
                return (client.*method)(request, options);
            },
            std::move(key)));
    }

    /// Bind a parameterless typed operation, such as system.info.
    static auto bind(std::string name, Accepted (jobu::ControlClient::*method)(jobu::ControlCallOptions)) -> Operation;

    auto name() const noexcept -> std::string const&;
    auto key() const noexcept -> std::optional<std::string> const&;
    auto policy() const noexcept -> ReplayPolicy;
    auto submit(jobu::ControlClient& client, jobu::ControlCallOptions options) const -> Accepted;

private:
    using Attempt = std::function<Accepted(jobu::ControlClient&, jobu::ControlCallOptions)>;
    Operation(std::string name, Attempt attempt, std::optional<std::string> key);

    std::string                _name;
    Attempt                    _attempt;
    std::optional<std::string> _key;
};

/// Owning metadata extracted from a local error or a represented remote error.
/// Remote data remains in ControlFailure; only these stable fields enter PHP.
struct FailureMetadata {
    std::optional<std::string> code;
    std::optional<std::string> category;
};

auto category_name(core::ErrorCategory category) noexcept -> char const*;
auto failure_metadata(jobu::ControlFailure const& failure) -> FailureMetadata;

/// Transport loss permits reconnect; validation, protocol and application errors
/// do not. A false outcome_unknown bit alone is never permission to retry.
auto is_transport_failure(core::Error const& error) noexcept -> bool;

/// A represented oversized mutation response may follow commit even though the
/// native client's uncertainty bit is false for all represented remote errors.
auto mutation_outcome_unknown(Operation const& operation, jobu::ControlFailure const& failure) -> bool;

} // namespace jb::php
