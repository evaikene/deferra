#include "operation.hpp"

#include <jb/core/text_validation.hpp>
#include <jb/core/time_source.hpp>
#include <jb/core/uuid.hpp>
#include <jb/rpc/protocol.hpp>

#include <algorithm>
#include <array>
#include <variant>

namespace jb::php {

auto replay_policy(std::string_view name) noexcept -> ReplayPolicy
{
    static constexpr std::array reads{"system.info",
                                      "system.stats",
                                      "queue.get",
                                      "queue.list",
                                      "queue.stats",
                                      "job.get",
                                      "job.list",
                                      "run.get",
                                      "run.list",
                                      "attempt.get",
                                      "attempt.list",
                                      "attempt.output",
                                      "secret.list",
                                      "schedule.validate",
                                      "schedule.next"};
    if (std::ranges::find(reads, name) != reads.end()) {
        return ReplayPolicy::Read;
    }
    if (name == "queue.suspend" || name == "queue.resume" || name == "job.suspend" || name == "job.resume") {
        return ReplayPolicy::DesiredState;
    }
    if (name == "queue.create" || name == "job.create" || name == "job.run_now") {
        return ReplayPolicy::Keyed;
    }
    return ReplayPolicy::Unsafe;
}

auto ensure_idempotency_key(std::optional<std::string>& key) -> core::Result<void, core::Error>
{
    if (key) {
        if (key->empty() || key->size() > 128 || !core::is_valid_utf8(*key)) {
            return core::Result<void, core::Error>::failure(
                {.category = core::ErrorCategory::InvalidArgument,
                 .code     = "php.client.invalid_idempotency_key",
                 .message  = "Idempotency key must contain 1 through 128 UTF-8 bytes"});
        }
        return core::Result<void, core::Error>::success();
    }
    // Reuse the SDK's OS-backed generator. Two freshly seeded UUIDv7 tails give
    // 148 random bits; no monotonic increment or process-global generator state
    // reduces the entropy. The 72-byte key fits the protocol's 128-byte limit.
    core::SystemTimeSource time;
    std::string            generated;
    for (int part = 0; part < 2; ++part) {
        core::UuidV7Generator generator(time);
        auto                  uuid = generator.generate();
        if (!uuid) {
            return core::Result<void, core::Error>::failure(uuid.error());
        }
        generated += uuid.value().to_string();
    }
    key = std::move(generated);
    return core::Result<void, core::Error>::success();
}

Operation::Operation(std::string name, Attempt attempt, std::optional<std::string> key)
    : _name(std::move(name))
    , _attempt(std::move(attempt))
    , _key(std::move(key))
{}

auto Operation::bind(std::string name, Accepted (jobu::ControlClient::*method)(jobu::ControlCallOptions)) -> Operation
{
    return {
        std::move(name),
        [method](jobu::ControlClient& client, jobu::ControlCallOptions options) { return (client.*method)(options); },
        std::nullopt};
}

auto Operation::name() const noexcept -> std::string const&
{
    return _name;
}

auto Operation::key() const noexcept -> std::optional<std::string> const&
{
    return _key;
}

auto Operation::policy() const noexcept -> ReplayPolicy
{
    auto const policy = replay_policy(_name);
    // A malformed internal descriptor must never enable an unkeyed create replay.
    return policy == ReplayPolicy::Keyed && (!_key || _key->empty()) ? ReplayPolicy::Unsafe : policy;
}

auto Operation::submit(jobu::ControlClient& client, jobu::ControlCallOptions options) const -> Accepted
{
    return _attempt(client, options);
}

auto category_name(core::ErrorCategory category) noexcept -> char const*
{
    switch (category) {
        case core::ErrorCategory::InvalidArgument:
            return "invalid_argument";
        case core::ErrorCategory::NotFound:
            return "not_found";
        case core::ErrorCategory::Conflict:
            return "conflict";
        case core::ErrorCategory::PermissionDenied:
            return "permission_denied";
        case core::ErrorCategory::Unavailable:
            return "unavailable";
        case core::ErrorCategory::ResourceExhausted:
            return "resource_exhausted";
        case core::ErrorCategory::Cancelled:
            return "cancelled";
        case core::ErrorCategory::Timeout:
            return "timeout";
        case core::ErrorCategory::Io:
            return "io";
        case core::ErrorCategory::Unsupported:
            return "unsupported";
        case core::ErrorCategory::Internal:
            return "internal";
    }
    return "internal";
}

auto failure_metadata(jobu::ControlFailure const& failure) -> FailureMetadata
{
    if (auto const* local = std::get_if<core::Error>(&failure.error)) {
        return {.code = local->code, .category = category_name(local->category)};
    }
    FailureMetadata result;
    auto const&     remote = std::get<rpc::RpcError>(failure.error);
    if (remote.data && remote.data->is_object()) {
        for (auto const& [name, value] : remote.data->as_object()) {
            if (!value.is_string()) {
                continue;
            }
            if (name == "code") {
                result.code = value.as_string();
            }
            else if (name == "category") {
                result.category = value.as_string();
            }
        }
    }
    return result;
}

auto is_transport_failure(core::Error const& error) noexcept -> bool
{
    return error.code == "php.client.connection_failed" || error.code == "rpc.connection_closed" ||
           error.code == "rpc.short_write" || error.code == "jobu.client.not_ready";
}

auto mutation_outcome_unknown(Operation const& operation, jobu::ControlFailure const& failure) -> bool
{
    if (operation.policy() == ReplayPolicy::Read) {
        return false;
    }
    return failure.outcome_unknown || failure_metadata(failure).code == "jobu.response.too_large";
}

} // namespace jb::php
