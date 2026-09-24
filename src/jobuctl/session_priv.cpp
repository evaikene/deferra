#include "session_priv.hpp"

#include "client.hpp"
#include "local_socket.hpp"
#include "object_priv.hpp"
#include "timer.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

namespace jb::jobuctl::detail {

using namespace jb::core;
using namespace jb::jobu;
using namespace jb::net;

namespace {

enum class SessionPhase : std::uint8_t {
    Idle,
    Connecting,
    Initializing,
    Command,
    Polling,
    Finished,
};

enum class SuspensionObservation : std::uint8_t {
    Pending,
    Complete,
    Conflict,
};

auto is_mutation(CommandKind kind) noexcept -> bool
{
    switch (kind) {
        case CommandKind::QueueCreate:
        case CommandKind::QueueUpdate:
        case CommandKind::QueueSuspend:
        case CommandKind::QueueResume:
        case CommandKind::QueueDelete:
        case CommandKind::JobCreate:
        case CommandKind::JobUpdate:
        case CommandKind::JobSuspend:
        case CommandKind::JobResume:
        case CommandKind::JobMove:
        case CommandKind::JobDelete:
        case CommandKind::JobRunNow:
        case CommandKind::RunCancel:
        case CommandKind::SecretSet:
        case CommandKind::SecretDelete:
            return true;
        case CommandKind::SystemInfo:
        case CommandKind::QueueGet:
        case CommandKind::QueueList:
        case CommandKind::JobGet:
        case CommandKind::JobList:
        case CommandKind::RunGet:
        case CommandKind::RunList:
        case CommandKind::AttemptGet:
        case CommandKind::AttemptList:
        case CommandKind::AttemptOutput:
        case CommandKind::SecretList:
            return false;
    }
    return false;
}

auto call_command(ControlClient& client, Command const& command, ControlCallOptions options)
    -> Result<ControlCallId, Error>
{
    // The builders and request-file decoder establish the request alternative paired with each kind.
    switch (command.kind) {
        case CommandKind::QueueCreate:
            return client.create_queue(std::get<CreateQueueRequest>(command.request), options);
        case CommandKind::QueueGet:
            return client.get_queue(std::get<QueueSelector>(command.request), options);
        case CommandKind::QueueList:
            return client.list_queues(std::get<QueueListRequest>(command.request), options);
        case CommandKind::QueueUpdate:
            return client.update_queue(std::get<UpdateQueueRequest>(command.request), options);
        case CommandKind::QueueSuspend:
            return client.suspend_queue(std::get<QueueSelector>(command.request), options);
        case CommandKind::QueueResume:
            return client.resume_queue(std::get<QueueSelector>(command.request), options);
        case CommandKind::QueueDelete:
            return client.delete_queue(std::get<QueueSelector>(command.request), options);
        case CommandKind::JobCreate:
            return client.create_job(std::get<CreateJobRequest>(command.request), options);
        case CommandKind::JobGet:
            return client.get_job(std::get<Uuid>(command.request), options);
        case CommandKind::JobList:
            return client.list_jobs(std::get<JobListRequest>(command.request), options);
        case CommandKind::JobUpdate:
            return client.update_job(std::get<UpdateJobRequest>(command.request), options);
        case CommandKind::JobSuspend:
            return client.suspend_job(std::get<Uuid>(command.request), options);
        case CommandKind::JobResume:
            return client.resume_job(std::get<Uuid>(command.request), options);
        case CommandKind::JobMove:
            return client.move_job(std::get<MoveJobRequest>(command.request), options);
        case CommandKind::JobDelete:
            return client.delete_job(std::get<DeleteJobRequest>(command.request), options);
        case CommandKind::JobRunNow:
            return client.run_now(std::get<RunNowRequest>(command.request), options);
        case CommandKind::RunGet:
            return client.get_run(std::get<Uuid>(command.request), options);
        case CommandKind::RunList:
            return client.list_runs(std::get<RunListRequest>(command.request), options);
        case CommandKind::RunCancel:
            return client.cancel_run(std::get<Uuid>(command.request), options);
        case CommandKind::AttemptGet:
            return client.get_attempt(std::get<AttemptKey>(command.request), options);
        case CommandKind::AttemptList:
            return client.list_attempts(std::get<AttemptListRequest>(command.request), options);
        case CommandKind::AttemptOutput:
            return client.read_attempt_output(std::get<AttemptOutputRequest>(command.request), options);
        case CommandKind::SecretSet:
            return client.set_secret(std::get<SetSecretRequest>(command.request), options);
        case CommandKind::SecretList:
            return client.list_secrets(std::get<SecretListRequest>(command.request), options);
        case CommandKind::SecretDelete:
            return client.delete_secret(std::get<std::string>(command.request), options);
        case CommandKind::SystemInfo:
            break;
    }
    return Result<ControlCallId, Error>::failure({
        .category = ErrorCategory::Internal,
        .code     = "jobuctl.session.invalid_command",
        .message  = "Unable to issue the command",
    });
}

auto remaining_options(TimePoint deadline) -> std::optional<ControlCallOptions>
{
    auto const remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now());
    if (remaining.count() <= 0) {
        return std::nullopt;
    }
    return ControlCallOptions{.timeout = remaining};
}

auto operation_exit(Error const& error) -> int
{
    if (error.code == "jobu.client.unsupported_method" || error.category == ErrorCategory::Unsupported) {
        return 1;
    }
    if (error.category == ErrorCategory::InvalidArgument && error.code != "jobu.client.invalid_response") {
        return 2;
    }
    return 3;
}

auto observe_suspension(CommandKind kind, ControlReply const& reply, std::optional<Uuid>& poll_id)
    -> SuspensionObservation
{
    if (kind == CommandKind::QueueSuspend) {
        if (auto const* queue = std::get_if<Queue>(&reply)) {
            if (poll_id && *poll_id != queue->id) {
                return SuspensionObservation::Conflict;
            }
            poll_id = queue->id;
            if (queue->state == QueueState::Suspending) {
                return SuspensionObservation::Pending;
            }
            if (queue->state == QueueState::Suspended) {
                return SuspensionObservation::Complete;
            }
        }
    }
    else if (kind == CommandKind::JobSuspend) {
        if (auto const* job = std::get_if<JobDefinition>(&reply)) {
            if (poll_id && *poll_id != job->id) {
                return SuspensionObservation::Conflict;
            }
            poll_id = job->id;
            if (job->state == JobState::Suspending) {
                return SuspensionObservation::Pending;
            }
            if (job->state == JobState::Suspended) {
                return SuspensionObservation::Complete;
            }
        }
    }
    return SuspensionObservation::Conflict;
}

auto job_run_from_details(RunDetails const& run) -> JobRun
{
    return {.id             = run.id,
            .job_id         = run.job_id,
            .job_revision   = run.job_revision,
            .queue_id       = run.queue_id,
            .origin         = run.origin,
            .schedule_owned = run.schedule_owned,
            .planned_at     = run.planned_at,
            .runnable_at    = run.runnable_at,
            .started_at     = run.started_at,
            .completed_at   = run.completed_at,
            .type           = run.type,
            .priority       = run.priority,
            .attributes     = run.attributes,
            .payload        = run.payload,
            .state          = run.state,
            .result         = run.result};
}

} // namespace

struct Session::Private : jb::core::priv::ObjectPrivate {
    Private(Command value, StandardAttributeRegistry const& attributes)
        : command{std::move(value)}
        , registry{attributes}
    {}

    Command                          command;
    StandardAttributeRegistry const& registry;

    // Reverse destruction stops the timer and typed client before their borrowed RPC client and socket.
    LocalSocket                      socket;
    std::unique_ptr<jb::rpc::Client> rpc;
    std::unique_ptr<ControlClient>   control;
    Timer                            timer;
    Timer                            poll_timer;
    SessionPhase                     phase{SessionPhase::Idle};
    TimePoint                        deadline;
    std::optional<ControlCallId>     active_call;
    std::optional<Uuid>              poll_id;
    std::chrono::milliseconds        poll_interval{100};
};

Session::Session(Command command, StandardAttributeRegistry const& registry)
    : Object{
          *new Private{std::move(command), registry}
}
{
    // Receiver-aware observers are installed after Object owns its only private allocation.
    auto* data = d_ptr<Private>();
    data->socket.set_read_buffer_limit(std::size_t{2} * 1024U * 1024U);
    data->socket.error_occurred.connect(this, [this](IOError, std::string const&) {
        auto* state = d_ptr<Private>();
        finish(3,
               local_error(
                   {
                       .category = ErrorCategory::Io,
                       .code     = "jobuctl.connection_failed",
                       .message  = "Daemon socket connection failed",
                   },
                   state->phase == SessionPhase::Command && state->active_call && is_mutation(state->command.kind)));
    });
    data->socket.connected.connect(this, [this] { connected(); });
    data->timer.timeout.connect(this, [this] { deadline_expired(); });
    data->poll_timer.timeout.connect(this, [this] { poll_wait(); });
}

Session::~Session()
{
    // Stop borrowed-client callbacks while this derived object and its signals are intact.
    auto* data  = d_ptr<Private>();
    data->phase = SessionPhase::Finished;
    data->timer.stop();
    data->poll_timer.stop();
    data->control.reset();
    data->rpc.reset();
}

void Session::start()
{
    auto* data = d_ptr<Private>();
    if (data->phase != SessionPhase::Idle) {
        return;
    }

    data->phase    = SessionPhase::Connecting;
    data->deadline = Clock::now() + std::chrono::duration_cast<Clock::duration>(data->command.timeout);
    data->timer.start(data->command.timeout);
    data->socket.connect_to_server(data->command.socket_path);
}

void Session::finish(int code, std::optional<CliError> error)
{
    auto* data = d_ptr<Private>();
    if (data->phase == SessionPhase::Finished) {
        return;
    }

    // Latch completion before close() emits any pending-call failures through receiver-aware observers.
    data->phase = SessionPhase::Finished;
    data->timer.stop();
    data->poll_timer.stop();
    if (data->control) {
        data->control->close();
    }
    if (error) {
        print_error(data->command.json, *error);
    }
    emit(finished, code);
}

void Session::deadline_expired()
{
    auto*      data    = d_ptr<Private>();
    auto const unknown = data->phase == SessionPhase::Command && data->active_call && is_mutation(data->command.kind);
    auto       message = std::string_view{"Overall command deadline expired"};
    if (data->phase == SessionPhase::Polling) {
        message = data->command.kind == CommandKind::RunCancel
                    ? "Cancelled state was not confirmed before the deadline"
                    : "Suspended state was not confirmed before the deadline";
    }
    finish(3,
           local_error(
               {
                   .category = ErrorCategory::Timeout,
                   .code     = "jobu.client.timeout",
                   .message  = std::string{message},
               },
               unknown));
}

void Session::connected()
{
    auto* data = d_ptr<Private>();
    if (data->phase != SessionPhase::Connecting) {
        return;
    }

    data->rpc     = std::make_unique<jb::rpc::Client>(data->socket);
    data->control = std::make_unique<ControlClient>(*data->rpc, data->registry);
    data->control->ready.connect(this, [this](SystemInfo const& info) { ready(info); });
    data->control->reply_received.connect(this, [this](ControlCallId id, ControlReply const& reply) {
        receive_reply(id, reply);
    });
    data->control->call_failed.connect(this, [this](ControlCallId id, ControlFailure const& failure) {
        receive_failure(id, failure);
    });
    data->control->failed.connect(this, [this](Error const& error) {
        auto* state = d_ptr<Private>();
        finish(3,
               local_error(error,
                           state->phase == SessionPhase::Command && state->active_call &&
                               is_mutation(state->command.kind)));
    });

    data->phase  = SessionPhase::Initializing;
    auto options = remaining_options(data->deadline);
    if (!options) {
        deadline_expired();
        return;
    }
    auto started = data->control->initialize(*options);
    if (!started) {
        finish(operation_exit(started.error()), local_error(started.error()));
    }
}

void Session::ready(SystemInfo const& info)
{
    auto* data = d_ptr<Private>();
    if (data->phase != SessionPhase::Initializing) {
        return;
    }
    if (data->command.kind == CommandKind::SystemInfo) {
        if (!print_command_result(data->command, ControlReply{info}, data->registry)) {
            finish(3,
                   local_error({
                       .category = ErrorCategory::Internal,
                       .code     = "jobuctl.output.invalid_reply",
                       .message  = "Unable to render the daemon reply",
                   }));
            return;
        }
        finish(0);
        return;
    }

    auto options = remaining_options(data->deadline);
    if (!options) {
        deadline_expired();
        return;
    }
    data->phase = SessionPhase::Command;
    auto call   = call_command(*data->control, data->command, *options);
    if (!call) {
        finish(operation_exit(call.error()), local_error(call.error()));
        return;
    }
    data->active_call = *call;
}

void Session::receive_reply(ControlCallId id, ControlReply const& reply)
{
    auto* data = d_ptr<Private>();
    if ((data->phase != SessionPhase::Command && data->phase != SessionPhase::Polling) || data->active_call != id) {
        return;
    }
    data->active_call.reset();

    if (data->command.wait && data->command.kind == CommandKind::RunCancel) {
        if (data->phase == SessionPhase::Command) {
            auto const* cancelled = std::get_if<CancelRunResult>(&reply);
            if (!cancelled) {
                finish(3,
                       local_error({.category = ErrorCategory::Internal,
                                    .code     = "jobuctl.output.invalid_reply",
                                    .message  = "Unable to render the daemon reply"}));
                return;
            }
            data->poll_id = cancelled->run.id;
            if (cancelled->disposition == CancelDisposition::Requested) {
                data->phase = SessionPhase::Polling;
                schedule_poll();
                return;
            }
            if (cancelled->run.state != RunState::Cancelled) {
                finish(1,
                       local_error({.category = ErrorCategory::Conflict,
                                    .code     = "jobuctl.wait.state_changed",
                                    .message  = "Run reached a different terminal state before cancellation"}));
                return;
            }
        }
        else {
            auto const* run = std::get_if<RunDetails>(&reply);
            if (!run || !data->poll_id || run->id != *data->poll_id) {
                finish(1,
                       local_error({.category = ErrorCategory::Conflict,
                                    .code     = "jobuctl.wait.state_changed",
                                    .message  = "Cancellation observation changed run identity"}));
                return;
            }
            if (run->state == RunState::Cancelled) {
                auto completed =
                    CancelRunResult{.run = job_run_from_details(*run), .disposition = CancelDisposition::Completed};
                if (!print_command_result(data->command, ControlReply{std::move(completed)}, data->registry)) {
                    finish(3,
                           local_error({.category = ErrorCategory::Internal,
                                        .code     = "jobuctl.output.invalid_reply",
                                        .message  = "Unable to render the daemon reply"}));
                    return;
                }
                finish(0);
                return;
            }
            if (run->state == RunState::Succeeded || run->state == RunState::Failed ||
                run->state == RunState::Interrupted) {
                finish(1,
                       local_error({.category = ErrorCategory::Conflict,
                                    .code     = "jobuctl.wait.state_changed",
                                    .message  = "Run reached a different terminal state before cancellation"}));
                return;
            }
            schedule_poll();
            return;
        }
    }

    if (data->command.wait && data->command.kind != CommandKind::RunCancel) {
        auto const observation = observe_suspension(data->command.kind, reply, data->poll_id);
        if (observation == SuspensionObservation::Conflict) {
            finish(1,
                   local_error({.category = ErrorCategory::Conflict,
                                .code     = "jobuctl.wait.state_changed",
                                .message  = "Suspension did not reach a valid state"}));
            return;
        }
        if (observation == SuspensionObservation::Pending) {
            // A suspend reply is observed once; every later request is a read by stable ID.
            data->phase = SessionPhase::Polling;
            schedule_poll();
            return;
        }
    }

    if (data->command.kind == CommandKind::AttemptOutput && (data->command.raw || data->command.output_file)) {
        auto const* chunk = std::get_if<AttemptOutputChunk>(&reply);
        if (!chunk) {
            finish(3,
                   local_error({.category = ErrorCategory::Internal,
                                .code     = "jobuctl.output.invalid_reply",
                                .message  = "Unable to render the daemon reply"}));
            return;
        }
        auto delivered = write_output_chunk(data->command, *chunk);
        if (!delivered) {
            finish(2, local_error(delivered.error()));
            return;
        }
        finish(0);
        return;
    }

    if (!print_command_result(data->command, reply, data->registry)) {
        finish(3,
               local_error({
                   .category = ErrorCategory::Internal,
                   .code     = "jobuctl.output.invalid_reply",
                   .message  = "Unable to render the daemon reply",
               }));
        return;
    }
    finish(0);
}

void Session::receive_failure(ControlCallId id, ControlFailure const& failure)
{
    auto* data = d_ptr<Private>();
    if ((data->phase != SessionPhase::Command && data->phase != SessionPhase::Polling) || data->active_call != id) {
        return;
    }
    data->active_call.reset();
    if (failure.kind == ControlFailureKind::Remote) {
        finish(1, remote_error(std::get<jb::rpc::RpcError>(failure.error)));
    }
    else {
        auto const& error = std::get<Error>(failure.error);
        finish(operation_exit(error), local_error(error, failure.outcome_unknown));
    }
}

void Session::schedule_poll()
{
    auto* data = d_ptr<Private>();
    data->poll_timer.start(data->poll_interval);
    data->poll_interval = std::min(data->poll_interval * 2, std::chrono::milliseconds{500});
}

void Session::poll_wait()
{
    auto* data = d_ptr<Private>();
    if (data->phase != SessionPhase::Polling || data->active_call || !data->poll_id) {
        return;
    }

    auto options = remaining_options(data->deadline);
    if (!options) {
        deadline_expired();
        return;
    }
    auto call = [&]() -> Result<ControlCallId, Error> {
        if (data->command.kind == CommandKind::QueueSuspend) {
            return data->control->get_queue(QueueSelector{*data->poll_id}, *options);
        }
        if (data->command.kind == CommandKind::JobSuspend) {
            return data->control->get_job(*data->poll_id, *options);
        }
        return data->control->get_run(*data->poll_id, *options);
    }();
    if (!call) {
        finish(operation_exit(call.error()), local_error(call.error()));
        return;
    }
    data->active_call = *call;
}

} // namespace jb::jobuctl::detail
