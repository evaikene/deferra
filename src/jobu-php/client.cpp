#include "client.hpp"

#include <jb/core/event_loop.hpp>
#include <jb/core/io_device.hpp>
#include <jb/core/object.hpp>
#include <jb/core/signal.hpp>
#include <jb/jobu/system_info.hpp>
#include <jb/net/local_socket.hpp>

#include <algorithm>
#include <array>
#include <optional>
#include <string_view>
#include <utility>

namespace jb::php {
namespace {

auto timeout_error() -> core::Error
{
    return {.category = core::ErrorCategory::Timeout,
            .code     = "php.client.timeout",
            .message  = "The operation deadline expired"};
}

auto closed_error() -> core::Error
{
    return {.category = core::ErrorCategory::Cancelled, .code = "php.client.closed", .message = "The client is closed"};
}

auto local_failure(core::Error error, bool uncertain = false) -> Failure
{
    return {.kind = jobu::ControlFailureKind::Local, .error = std::move(error), .outcome_unknown = uncertain};
}

auto remaining_options(core::TimePoint deadline) -> core::Result<jobu::ControlCallOptions, core::Error>
{
    auto const now = core::Clock::now();
    if (now >= deadline) {
        return core::Result<jobu::ControlCallOptions, core::Error>::failure(timeout_error());
    }
    // Round up only the native timer's millisecond granularity. run_until still
    // enforces the original absolute deadline, including the fractional remainder.
    auto const remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - now);
    return core::Result<jobu::ControlCallOptions, core::Error>::success({.timeout = remaining});
}

auto check_peer(jobu::SystemInfo const& info) -> core::Result<void, core::Error>
{
    // This is the PHP v1 contract, not the daemon's evolving registration list.
    // New capabilities are welcome; none of these thirty may be omitted.
    static constexpr std::array<std::string_view, 30> required{
        "system.info",    "system.stats", "queue.create", "queue.get",     "queue.list",        "queue.update",
        "queue.suspend",  "queue.resume", "queue.delete", "queue.stats",   "job.create",        "job.get",
        "job.list",       "job.update",   "job.suspend",  "job.resume",    "job.move",          "job.delete",
        "job.run_now",    "run.get",      "run.list",     "run.cancel",    "attempt.get",       "attempt.list",
        "attempt.output", "secret.set",   "secret.list",  "secret.delete", "schedule.validate", "schedule.next"};
    if (info.api_version.major != 1 || info.api_version.minor < 7) {
        return core::Result<void, core::Error>::failure(
            {.category = core::ErrorCategory::Unsupported,
             .code     = "php.client.unsupported_api",
             .message  = "JobU API 1.7 or a newer compatible minor is required"});
    }
    for (auto const method : required) {
        if (std::ranges::find(info.capabilities, method) == info.capabilities.end()) {
            return core::Result<void, core::Error>::failure(
                {.category = core::ErrorCategory::Unsupported,
                 .code     = "php.client.unsupported_capabilities",
                 .message  = "The daemon does not advertise all PHP v1 capabilities"});
        }
    }
    return core::Result<void, core::Error>::success();
}

auto connect_session(Session& session, core::EventLoop& loop, std::string const& endpoint, core::TimePoint deadline)
    -> core::Result<void, core::Error>
{
    bool                            connected = false;
    std::optional<core::Error>      failure;
    std::optional<jobu::SystemInfo> info;
    // The receiver dies before the borrowed stack state. Its lifetime fence also
    // suppresses already-queued delivery after any early return.
    core::Object                    receiver;
    auto&                           socket = session.socket();
    socket.connected.connect(&receiver, [&connected] { connected = true; });
    socket.error_occurred.connect(&receiver, [&failure](core::IOError, std::string const&) {
        failure = core::Error{.category = core::ErrorCategory::Unavailable,
                              .code     = "php.client.connection_failed",
                              .message  = "Unable to connect to JobU"};
    });
    if (auto options = remaining_options(deadline); !options) {
        return core::Result<void, core::Error>::failure(options.error());
    }
    socket.connect_to_server(endpoint);
    auto waited = loop.run_until(deadline, [&] { return connected || failure.has_value(); });
    if (!waited || failure) {
        return core::Result<void, core::Error>::failure(failure ? *failure : waited.error());
    }
    auto attached = session.attach_clients();
    if (!attached) {
        return attached;
    }

    auto& typed = session.typed();
    typed.ready.connect(&receiver, [&info](jobu::SystemInfo const& value) { info = value; });
    typed.failed.connect(&receiver, [&failure](core::Error const& error) { failure = error; });
    auto options = remaining_options(deadline);
    if (!options) {
        return core::Result<void, core::Error>::failure(options.error());
    }
    auto accepted = typed.initialize(options.value());
    if (!accepted) {
        return accepted;
    }
    waited = loop.run_until(deadline, [&] { return info.has_value() || failure.has_value(); });
    if (!waited || failure) {
        return core::Result<void, core::Error>::failure(failure ? *failure : waited.error());
    }
    return check_peer(*info);
}

} // namespace

auto make_deadline(core::TimePoint start, std::chrono::milliseconds timeout)
    -> core::Result<core::TimePoint, core::Error>
{
    // Bound the conversion before multiplying milliseconds into native ticks,
    // then bound the addition separately. Neither step may overflow signed time.
    auto const maximum = std::chrono::duration_cast<std::chrono::milliseconds>(core::Duration::max());
    if (timeout.count() <= 0 || timeout > maximum) {
        return core::Result<core::TimePoint, core::Error>::failure(
            {.category = core::ErrorCategory::InvalidArgument,
             .code     = "php.client.invalid_timeout",
             .message  = "Timeout must be positive and representable by the monotonic clock"});
    }
    auto const duration = std::chrono::duration_cast<core::Duration>(timeout);
    if (start > core::TimePoint::max() - duration) {
        return core::Result<core::TimePoint, core::Error>::failure(
            {.category = core::ErrorCategory::InvalidArgument,
             .code     = "php.client.invalid_timeout",
             .message  = "Timeout exceeds the monotonic clock range"});
    }
    return core::Result<core::TimePoint, core::Error>::success(start + duration);
}

auto call_synchronously(jobu::ControlClient& client,
                        core::EventLoop&     loop,
                        core::TimePoint      deadline,
                        Submit const&        submit) -> Reply
{
    // A completed probe validates thread/current-loop/reentrancy before any RPC.
    // It dispatches no work and reuses core's thread-wide dispatch guard.
    auto context = loop.run_until(deadline, [] { return true; });
    if (!context) {
        return Reply::failure(local_failure(context.error()));
    }
    auto options = remaining_options(deadline);
    if (!options) {
        return Reply::failure(local_failure(options.error()));
    }

    jobu::ControlCallId  id = 0;
    std::optional<Reply> outcome;
    core::Object         receiver;
    client.reply_received.connect(&receiver, [&](jobu::ControlCallId received, jobu::ControlReply const& reply) {
        if (received == id && !outcome) {
            outcome = Reply::success(reply);
        }
    });
    client.call_failed.connect(&receiver, [&](jobu::ControlCallId received, Failure const& failure) {
        if (received == id && !outcome) {
            outcome = Reply::failure(failure);
        }
    });
    // Do not settle on failed: a terminal notification precedes the per-call
    // failure that carries outcome_unknown. Typed immediate outcomes are deferred
    // until after acceptance, so id is assigned before either observer can run.
    auto accepted = submit(options.value());
    if (!accepted) {
        return Reply::failure(local_failure(accepted.error()));
    }
    id          = accepted.value();
    auto waited = loop.run_until(deadline, [&] { return outcome.has_value(); });
    if (outcome) {
        return std::move(*outcome);
    }

    // cancel_call synchronously reports whether the accepted mutation may have
    // escaped. Keep that bit but report the actual wait failure, not cancellation.
    // A terminal client may already have retired the ID with delivery queued;
    // without its outcome, conservatively preserve uncertainty rather than guess.
    client.cancel_call(id);
    auto const uncertain = !outcome || (!*outcome && outcome->error().outcome_unknown);
    return Reply::failure(local_failure(waited.error(), uncertain));
}

Client::Client() noexcept  = default;
Client::~Client() noexcept = default;

auto Client::initialize(Request&                  request,
                        std::string const&        endpoint,
                        core::TimePoint           deadline,
                        std::chrono::milliseconds timeout) -> core::Result<void, core::Error>
{
    if (_initialized) {
        return core::Result<void, core::Error>::failure({.category = core::ErrorCategory::Conflict,
                                                         .code     = "php.client.already_initialized",
                                                         .message  = "The client has already been initialized"});
    }
    _initialized = true;
    _timeout     = timeout;
    auto created = request.create_session();
    if (!created) {
        return core::Result<void, core::Error>::failure(created.error());
    }
    auto  session = std::move(created).value();
    auto& loop    = *request.loop();
    auto  context = loop.run_until(deadline, [] { return true; });
    if (!context) {
        return context;
    }
    auto connected = connect_session(*session, loop, endpoint, deadline);
    if (!connected) {
        return connected;
    }
    _session = std::move(session);
    return core::Result<void, core::Error>::success();
}

auto Client::close() noexcept -> void
{
    if (_session) {
        _session->close();
    }
}

auto Client::call(core::TimePoint deadline, Submit const& submit) const -> Reply
{
    auto* client = typed();
    if (!client) {
        return Reply::failure(local_failure(closed_error()));
    }
    return call_synchronously(*client, *client->event_loop(), deadline, submit);
}

auto Client::typed() const noexcept -> jobu::ControlClient*
{
    return _session && !_session->is_closed() ? &_session->typed() : nullptr;
}

auto Client::timeout() const noexcept -> std::chrono::milliseconds
{
    return _timeout;
}

} // namespace jb::php
