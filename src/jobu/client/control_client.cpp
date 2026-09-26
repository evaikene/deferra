#include "control_client.hpp"

#include "control_client_priv.hpp"

#include "event_loop.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <exception>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace jb::jobu {
namespace {

using jb::core::Error;
using jb::core::ErrorCategory;
using jb::core::JsonValue;
using jb::core::Result;

constexpr std::size_t max_typed_calls{128};

auto client_error(ErrorCategory category, std::string_view code, std::string_view message) -> Error
{
    return {
        .category = category,
        .code     = std::string{code},
        .message  = std::string{message},
    };
}

auto invalid_state_error() -> Error
{
    return client_error(ErrorCategory::Unavailable, "jobu.client.not_ready", "JobU client is not ready");
}

auto timeout_error() -> Error
{
    return client_error(ErrorCategory::Timeout, "jobu.client.timeout", "JobU call timed out");
}

auto cancelled_error() -> Error
{
    return client_error(ErrorCategory::Cancelled, "jobu.client.cancelled", "JobU call was cancelled");
}

auto closed_error() -> Error
{
    return client_error(ErrorCategory::Cancelled, "jobu.client.closed", "JobU client was closed");
}

auto unsupported_method_error() -> Error
{
    return client_error(ErrorCategory::Unsupported, "jobu.client.unsupported_method", "JobU method is not advertised");
}

auto valid_deadline(ControlCallOptions options) -> std::optional<jb::core::TimePoint>
{
    using Clock = jb::core::Clock;
    if (options.timeout.count() <= 0 ||
        options.timeout > std::chrono::duration_cast<std::chrono::milliseconds>(Clock::duration::max())) {
        return std::nullopt;
    }

    auto const interval = std::chrono::duration_cast<Clock::duration>(options.timeout);
    auto const now      = Clock::now();
    if (interval > Clock::time_point::max() - now) {
        return std::nullopt;
    }
    return now + interval;
}

auto local_failure(Error error, bool outcome_unknown = false) -> ControlFailure
{
    return {
        .kind            = ControlFailureKind::Local,
        .error           = std::move(error),
        .outcome_unknown = outcome_unknown,
    };
}

auto wire_number(jb::rpc::RequestId const& id) -> std::optional<std::uint64_t>
{
    if (auto const* number = std::get_if<std::uint64_t>(&id)) {
        return *number;
    }
    return std::nullopt;
}

void post_required(jb::core::EventLoop& loop, jb::core::Task task)
{
    // Losing the only deferred completion after accepting a call would leave that call unresolved.
    if (!loop.post(std::move(task))) {
        std::terminate();
    }
}

} // anonymous namespace

void ControlClient::Private::bind_owner(ControlClient& client)
{
    owner = &client;
    assert(owner->event_loop() != nullptr);
    assert(rpc.event_loop() == owner->event_loop());

    auto* data = this;
    result_connection =
        rpc.result_received.connect(owner, [data](auto const& id, auto const& value) { data->on_result(id, value); });
    error_connection    = rpc.error_received.connect(owner, [data](auto const& id, auto const& error) {
        data->on_remote_error(id, error);
    });
    terminal_connection = rpc.terminated.connect(owner, [data](Error const& error) { data->on_terminated(error); });
    timeout_connection  = deadline_timer.timeout.connect(owner, [data] { data->on_timeout(); });
}

void ControlClient::Private::disconnect_sources() noexcept
{
    result_connection.disconnect();
    error_connection.disconnect();
    terminal_connection.disconnect();
    timeout_connection.disconnect();
    deadline_timer.stop();
}

void ControlClient::Private::on_result(jb::rpc::RequestId const& id, JsonValue const& value)
{
    record_response(id, value);
}

void ControlClient::Private::on_remote_error(jb::rpc::RequestId const& id, jb::rpc::RpcError const& error)
{
    record_response(id, error);
}

void ControlClient::Private::record_response(jb::rpc::RequestId const&                  id,
                                             std::variant<JsonValue, jb::rpc::RpcError> outcome)
{
    if (phase != Phase::Initializing && phase != Phase::Ready) {
        return;
    }

    auto const number = wire_number(id);
    if (!number) {
        return;
    }

    auto const correlated = wire_to_local.find(*number);
    if (correlated == wire_to_local.end()) {
        // The raw client also serves custom calls whose replies do not belong to this wrapper.
        return;
    }

    auto const local_id = correlated->second;
    wire_to_local.erase(correlated);
    auto const entry = pending.find(local_id);
    if (entry == pending.end()) {
        return;
    }

    std::visit([&entry](auto&& value) { entry->second.outcome = std::forward<decltype(value)>(value); },
               std::move(outcome));
    ready_outcomes.push_back(local_id);
    rearm_deadline();
    schedule_delivery();
}

void ControlClient::Private::bind_wire_id(ControlCallId local_id, jb::rpc::RequestId const& wire_id)
{
    // The raw acceptance hook runs before even a reply supplied during write can reach record_response().
    auto entry = pending.find(local_id);
    if (entry == pending.end()) {
        return;
    }
    entry->second.wire_id       = wire_id;
    entry->second.possibly_sent = true;

    if (auto const number = wire_number(wire_id)) {
        wire_to_local.emplace(*number, local_id);
    }
    rearm_deadline();
}

void ControlClient::Private::schedule_delivery()
{
    if (delivery_scheduled) {
        return;
    }
    delivery_scheduled = true;

    auto       guard              = std::weak_ptr<int>{lifetime_guard};
    auto const current_generation = generation;
    auto*      data               = this;
    post_required(*owner->event_loop(), [guard, current_generation, data] {
        if (!guard.expired() && data->generation == current_generation) {
            data->deliver_ready_outcomes();
        }
    });
}

void ControlClient::Private::deliver_ready_outcomes()
{
    auto const lifetime            = std::weak_ptr<int>{lifetime_guard};
    auto const delivery_generation = generation;
    delivery_scheduled             = false;
    while (!ready_outcomes.empty() && (phase == Phase::Initializing || phase == Phase::Ready)) {
        auto const id = ready_outcomes.front();
        ready_outcomes.erase(ready_outcomes.begin());

        auto entry = pending.find(id);
        if (entry == pending.end()) {
            continue;
        }
        auto call = std::move(entry->second);
        pending.erase(entry);
        rearm_deadline();
        deliver_one(id, std::move(call));

        // A handler can destroy or close the wrapper. Check lifetime before reading private state again.
        if (lifetime.expired()) {
            return;
        }
        if (generation != delivery_generation) {
            return;
        }
    }
}

void ControlClient::Private::fail_handshake(Error error)
{
    phase = Phase::Failed;
    disconnect_sources();
    pending.clear();
    wire_to_local.clear();
    ready_outcomes.clear();
    error.detail.clear();
    owner->emit(owner->failed, error);
}

void ControlClient::Private::on_terminated(Error const& raw_error)
{
    if (phase == Phase::Closed || phase == Phase::Failed) {
        return;
    }

    // Latch and release every correlation before public observers can reenter close().
    phase = Phase::Failed;
    disconnect_sources();
    auto failures = std::vector<std::pair<ControlCallId, bool>>{};
    for (auto const& [id, call] : pending) {
        if (id != 0U) {
            auto const uncertain = is_mutation(call.method) &&
                                   !std::holds_alternative<jb::rpc::RpcError>(call.outcome) &&
                                   (call.possibly_sent || raw_error.code == "rpc.short_write" ||
                                    raw_error.code == "rpc.connection_closed");
            failures.emplace_back(id, uncertain);
        }
    }
    pending.clear();
    wire_to_local.clear();
    ready_outcomes.clear();
    ++generation;

    auto safe_error = raw_error;
    safe_error.detail.clear();
    auto       guard              = std::weak_ptr<int>{lifetime_guard};
    auto const current_generation = generation;
    auto*      data               = this;
    post_required(
        *owner->event_loop(),
        [guard, current_generation, data, safe_error = std::move(safe_error), failures = std::move(failures)] {
            if (guard.expired() || data->generation != current_generation) {
                return;
            }
            data->owner->emit(data->owner->failed, safe_error);
            if (guard.expired()) {
                return;
            }

            // The failures were latched before the terminal signal. A live wrapper still owes them after reentrant
            // close().
            for (auto const& [id, uncertain] : failures) {
                data->owner->emit(data->owner->call_failed, id, local_failure(safe_error, uncertain));
                if (guard.expired()) {
                    return;
                }
            }
        });
}

void ControlClient::Private::rearm_deadline()
{
    deadline_timer.stop();
    auto earliest = std::optional<jb::core::TimePoint>{};
    for (auto const& [id, call] : pending) {
        static_cast<void>(id);
        if (!std::holds_alternative<std::monostate>(call.outcome)) {
            continue;
        }
        if (!earliest || call.deadline < *earliest) {
            earliest = call.deadline;
        }
    }
    if (earliest) {
        auto const now = jb::core::Clock::now();
        deadline_timer.start(*earliest <= now ? jb::core::Duration::zero() : *earliest - now);
    }
}

void ControlClient::Private::on_timeout()
{
    auto const lifetime = std::weak_ptr<int>{lifetime_guard};
    auto       expired  = std::vector<std::pair<ControlCallId, bool>>{};
    auto const now      = jb::core::Clock::now();
    for (auto entry = pending.begin(); entry != pending.end();) {
        if (entry->second.deadline > now || !std::holds_alternative<std::monostate>(entry->second.outcome)) {
            ++entry;
            continue;
        }

        auto const  id   = entry->first;
        auto const& call = entry->second;
        if (call.wire_id) {
            rpc.cancel(*call.wire_id);
            if (auto const number = wire_number(*call.wire_id)) {
                wire_to_local.erase(*number);
            }
        }
        expired.emplace_back(id, is_mutation(call.method) && call.possibly_sent);
        entry = pending.erase(entry);
    }
    rearm_deadline();

    for (auto const& [id, uncertain] : expired) {
        if (id == 0U) {
            fail_handshake(timeout_error());
            return;
        }
        owner->emit(owner->call_failed, id, local_failure(timeout_error(), uncertain));
        // Expired calls are already retired. Reentrant close() cannot settle them, so deliver the remaining local
        // batch.
        if (lifetime.expired()) {
            return;
        }
    }
}

auto ControlClient::Private::start_call(Method                   method,
                                        std::string_view         name,
                                        std::optional<JsonValue> params,
                                        ControlCallOptions       options) -> Result<ControlCallId, Error>
{
    using CallResult = Result<ControlCallId, Error>;
    if (phase != Phase::Ready) {
        return CallResult::failure(invalid_state_error());
    }
    if (!capabilities.contains(std::string{name})) {
        return CallResult::failure(unsupported_method_error());
    }
    auto deadline = valid_deadline(options);
    if (!deadline) {
        return CallResult::failure(client_error(ErrorCategory::InvalidArgument,
                                                "jobu.client.invalid_timeout",
                                                "JobU call timeout is invalid"));
    }
    auto const pending_limit = std::min(max_typed_calls, rpc.max_pending_requests());
    if (pending.size() >= pending_limit || next_id == 0U) {
        return CallResult::failure(client_error(ErrorCategory::ResourceExhausted,
                                                "jobu.client.pending_limit",
                                                "JobU client pending call limit reached"));
    }

    auto const local_id = next_id;
    next_id             = local_id == std::numeric_limits<ControlCallId>::max() ? 0U : local_id + 1U;
    pending.emplace(local_id, Pending{.method = method, .deadline = *deadline});
    establishing          = local_id;
    auto const lifetime   = std::weak_ptr<int>{lifetime_guard};
    auto*      raw_client = &rpc;
    auto       wire =
        raw_client->call(name, std::move(params), [lifetime, data = this, local_id](jb::rpc::RequestId const& id) {
            if (!lifetime.expired()) {
                data->bind_wire_id(local_id, id);
            }
        });

    // A device callback during the write can close the wrapper and destroy it from an earlier call's failure handler.
    if (lifetime.expired()) {
        if (!wire) {
            return CallResult::failure(std::move(wire).error());
        }
        raw_client->cancel(wire.value());
        return CallResult::success(local_id);
    }
    establishing.reset();

    if (phase == Phase::Failed || phase == Phase::Closed) {
        if (wire) {
            rpc.cancel(wire.value());
        }
        return CallResult::success(local_id);
    }
    if (!wire) {
        pending.erase(local_id);
        rearm_deadline();
        return CallResult::failure(std::move(wire).error());
    }

    return CallResult::success(local_id);
}

auto ControlClient::Private::start_encoded_call(Method                   method,
                                                std::string_view         name,
                                                Result<JsonValue, Error> params,
                                                ControlCallOptions       options) -> Result<ControlCallId, Error>
{
    if (phase != Phase::Ready) {
        return Result<ControlCallId, Error>::failure(invalid_state_error());
    }
    if (!params) {
        return Result<ControlCallId, Error>::failure(std::move(params).error());
    }
    return start_call(method, name, std::move(params).value(), options);
}

void ControlClient::Private::close(bool emit_failures)
{
    if (phase == Phase::Closed || phase == Phase::Failed) {
        if (!emit_failures) {
            ++generation;
            lifetime_guard.reset();
        }
        return;
    }

    auto const initializing                 = phase == Phase::Initializing;
    auto const defer_initialization_failure = initializing && establishing == 0U;
    phase                                   = Phase::Closed;
    disconnect_sources();
    auto failures             = std::vector<std::pair<ControlCallId, bool>>{};
    auto establishing_failure = std::optional<std::pair<ControlCallId, bool>>{};
    for (auto const& [id, call] : pending) {
        if (call.wire_id) {
            rpc.cancel(*call.wire_id);
        }
        if (id != 0U) {
            auto const uncertain = is_mutation(call.method) &&
                                   !std::holds_alternative<jb::rpc::RpcError>(call.outcome) &&
                                   (call.possibly_sent || establishing == id);
            if (establishing == id) {
                establishing_failure.emplace(id, uncertain);
            }
            else {
                failures.emplace_back(id, uncertain);
            }
        }
    }
    pending.clear();
    wire_to_local.clear();
    ready_outcomes.clear();
    ++generation;

    if (!emit_failures) {
        lifetime_guard.reset();
        return;
    }
    auto const lifetime = std::weak_ptr<int>{lifetime_guard};

    if (initializing && !defer_initialization_failure) {
        owner->emit(owner->failed, closed_error());
        if (lifetime.expired()) {
            return;
        }
    }
    for (auto const& [id, uncertain] : failures) {
        owner->emit(owner->call_failed, id, local_failure(closed_error(), uncertain));
        if (lifetime.expired()) {
            return;
        }
    }
    if (defer_initialization_failure) {
        auto       guard              = std::weak_ptr<int>{lifetime_guard};
        auto const current_generation = generation;
        auto*      data               = this;
        post_required(*owner->event_loop(), [guard, current_generation, data] {
            if (!guard.expired() && data->generation == current_generation) {
                data->owner->emit(data->owner->failed, closed_error());
            }
        });
    }
    if (establishing_failure) {
        // A device callback may close us while the accepting raw call is still on the stack.
        auto       guard              = std::weak_ptr<int>{lifetime_guard};
        auto const current_generation = generation;
        auto const [id, uncertain]    = *establishing_failure;
        auto* data                    = this;
        post_required(*owner->event_loop(), [guard, current_generation, data, id, uncertain] {
            if (!guard.expired() && data->generation == current_generation) {
                data->owner->emit(data->owner->call_failed, id, local_failure(closed_error(), uncertain));
            }
        });
    }
}

ControlClient::ControlClient(jb::rpc::Client& rpc, AttributeRegistry const& attributes, jb::core::Object* parent)
    : Object(*new Private{rpc, attributes}, parent)
{
    d_ptr<Private>()->bind_owner(*this);
}

ControlClient::~ControlClient()
{
    // Stop callbacks while this derived object's signals still exist; destruction reports no new outcomes.
    d_ptr<Private>()->close(false);
}

auto ControlClient::initialize(ControlCallOptions options) -> Result<void, Error>
{
    using InitResult = Result<void, Error>;
    auto* data       = d_ptr<Private>();
    if (data->phase != Private::Phase::Uninitialized) {
        return InitResult::failure(invalid_state_error());
    }
    auto deadline = valid_deadline(options);
    if (!deadline) {
        return InitResult::failure(client_error(ErrorCategory::InvalidArgument,
                                                "jobu.client.invalid_timeout",
                                                "JobU call timeout is invalid"));
    }

    data->phase = Private::Phase::Initializing;
    data->pending.emplace(0U, Private::Pending{.method = Private::Method::Handshake, .deadline = *deadline});
    data->establishing = 0U;
    auto wire          = data->rpc.call("system.info", std::nullopt, [data](jb::rpc::RequestId const& id) {
        data->bind_wire_id(0U, id);
    });
    data->establishing.reset();

    if (data->phase == Private::Phase::Failed || data->phase == Private::Phase::Closed) {
        if (wire) {
            data->rpc.cancel(wire.value());
        }
        return InitResult::success();
    }
    if (!wire) {
        data->pending.clear();
        data->phase = Private::Phase::Uninitialized;
        return InitResult::failure(std::move(wire).error());
    }

    return InitResult::success();
}

void ControlClient::cancel_call(ControlCallId id)
{
    auto* data  = d_ptr<Private>();
    auto  entry = data->pending.find(id);
    if (id == 0U || entry == data->pending.end()) {
        return;
    }

    auto call = std::move(entry->second);
    data->pending.erase(entry);
    if (call.wire_id) {
        data->rpc.cancel(*call.wire_id);
        if (auto const number = wire_number(*call.wire_id)) {
            data->wire_to_local.erase(*number);
        }
    }
    std::erase(data->ready_outcomes, id);
    data->rearm_deadline();
    emit(call_failed,
         id,
         local_failure(cancelled_error(),
                       Private::is_mutation(call.method) && !std::holds_alternative<jb::rpc::RpcError>(call.outcome) &&
                           call.possibly_sent));
}

void ControlClient::close()
{
    d_ptr<Private>()->close(true);
}

} // namespace jb::jobu
