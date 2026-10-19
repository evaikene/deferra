#include "session_priv.hpp"

#include "client.hpp"
#include "command_helpers_priv.hpp"
#include "local_socket.hpp"
#include "object_priv.hpp"
#include "run_inspection_priv.hpp"
#include "timer.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio> // IWYU pragma: keep for stdout macro
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace jb::jobuctl::detail {

using namespace jb::core;
using namespace jb::jobu;
using namespace jb::net;

namespace {

auto connection_error_message(std::filesystem::path const& endpoint) -> std::string
{
    // POSIX paths can contain arbitrary bytes. Keep the quoted endpoint safe for terminals and JSON UTF-8 encoding.
    auto escaped = std::string{};
    for (auto const character : endpoint.string()) {
        auto const byte = static_cast<unsigned char>(character);
        if (character == '"' || character == '\\') {
            escaped += '\\';
            escaped += character;
        }
        else if (byte < 0x20U || byte >= 0x7fU) {
            escaped += fmt::format("\\x{:02X}", byte);
        }
        else {
            escaped += character;
        }
    }

    return fmt::format("Daemon socket connection failed at \"{}\". Use --socket PATH to select the daemon endpoint.",
                       escaped);
}

enum class SessionPhase : std::uint8_t {
    Idle,
    Connecting,
    Initializing,
    QueueLookup,
    QueueNameProbe,
    Command,
    Polling,
    InspectionList,
    InspectionDetails,
    OutputList,
    OutputChunk,
    Finished,
};

auto is_call_phase(SessionPhase phase) noexcept -> bool
{
    return phase == SessionPhase::QueueLookup || phase == SessionPhase::QueueNameProbe ||
           phase == SessionPhase::Command || phase == SessionPhase::Polling || phase == SessionPhase::InspectionList ||
           phase == SessionPhase::InspectionDetails || phase == SessionPhase::OutputList ||
           phase == SessionPhase::OutputChunk;
}

auto is_queue_selection_phase(SessionPhase phase) noexcept -> bool
{
    return phase == SessionPhase::QueueLookup || phase == SessionPhase::QueueNameProbe;
}

auto command_queue_selector(CommandKind kind, CommandRequest& request) -> QueueSelector*
{
    // Only ordinary queue commands carry intent. Raw requests and cursor continuations have none.
    switch (kind) {
        case CommandKind::QueueGet:
            return &std::get<GetQueueRequest>(request).queue;
        case CommandKind::QueueUpdate:
            return &std::get<UpdateQueueRequest>(request).queue;
        case CommandKind::QueueSuspend:
        case CommandKind::QueueResume:
        case CommandKind::QueueDelete:
            return &std::get<QueueSelector>(request);
        case CommandKind::QueueStats: {
            auto& statistics = std::get<QueueStatisticsListRequest>(request);
            if (auto* query = std::get_if<QueueStatisticsQuery>(&statistics)) {
                return &query->selector;
            }
            return nullptr;
        }
        default:
            return nullptr;
    }
}

auto is_queue_not_found(CliError const& error) -> bool
{
    return error.kind == "remote" &&
           error.rpc_code == static_cast<std::int64_t>(jb::rpc::ErrorCode::ApplicationError) &&
           error.category == "not_found" && error.code == "jobu.queue.not_found";
}

auto invalid_session_response() -> CliError
{
    return local_error({.category = ErrorCategory::InvalidArgument,
                        .code     = "jobu.client.invalid_response",
                        .message  = "JobU response is invalid"});
}

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
        case CommandKind::RunOutput:
        case CommandKind::AttemptGet:
        case CommandKind::AttemptList:
        case CommandKind::AttemptOutput:
        case CommandKind::SecretList:
        case CommandKind::SystemStats:
        case CommandKind::QueueStats:
        case CommandKind::ScheduleValidate:
        case CommandKind::ScheduleNext:
            return false;
    }
    return false;
}

auto call_command(ControlClient& client, CommandKind kind, CommandRequest const& request, ControlCallOptions options)
    -> Result<ControlCallId, Error>
{
    // The builders and request-file decoder establish the request alternative paired with each kind.
    switch (kind) {
        case CommandKind::QueueCreate:
            return client.create_queue(std::get<CreateQueueRequest>(request), options);
        case CommandKind::QueueGet:
            return client.get_queue(std::get<GetQueueRequest>(request), options);
        case CommandKind::QueueList:
            return client.list_queues(std::get<QueueListRequest>(request), options);
        case CommandKind::QueueUpdate:
            return client.update_queue(std::get<UpdateQueueRequest>(request), options);
        case CommandKind::QueueSuspend:
            return client.suspend_queue(std::get<QueueSelector>(request), options);
        case CommandKind::QueueResume:
            return client.resume_queue(std::get<QueueSelector>(request), options);
        case CommandKind::QueueDelete:
            return client.delete_queue(std::get<QueueSelector>(request), options);
        case CommandKind::JobCreate:
            return client.create_job(std::get<CreateJobRequest>(request), options);
        case CommandKind::JobGet:
            return client.get_job(std::get<Uuid>(request), options);
        case CommandKind::JobList:
            return client.list_jobs(std::get<JobListRequest>(request), options);
        case CommandKind::JobUpdate:
            return client.update_job(std::get<UpdateJobRequest>(request), options);
        case CommandKind::JobSuspend:
            return client.suspend_job(std::get<Uuid>(request), options);
        case CommandKind::JobResume:
            return client.resume_job(std::get<Uuid>(request), options);
        case CommandKind::JobMove:
            return client.move_job(std::get<MoveJobRequest>(request), options);
        case CommandKind::JobDelete:
            return client.delete_job(std::get<DeleteJobRequest>(request), options);
        case CommandKind::JobRunNow:
            return client.run_now(std::get<RunNowRequest>(request), options);
        case CommandKind::RunGet:
            return client.get_run(std::get<Uuid>(request), options);
        case CommandKind::RunList:
            return client.list_runs(std::get<RunListRequest>(request), options);
        case CommandKind::RunCancel:
            return client.cancel_run(std::get<Uuid>(request), options);
        case CommandKind::RunOutput:
            return client.get_run(std::get<RunOutputRequest>(request).run_id, options);
        case CommandKind::AttemptGet:
            return client.get_attempt(std::get<AttemptKey>(request), options);
        case CommandKind::AttemptList:
            return client.list_attempts(std::get<AttemptListRequest>(request), options);
        case CommandKind::AttemptOutput:
            return client.read_attempt_output(std::get<AttemptOutputRequest>(request), options);
        case CommandKind::SecretSet:
            return client.set_secret(std::get<SetSecretRequest>(request), options);
        case CommandKind::SecretList:
            return client.list_secrets(std::get<SecretListRequest>(request), options);
        case CommandKind::SecretDelete:
            return client.delete_secret(std::get<std::string>(request), options);
        case CommandKind::SystemStats:
            return client.system_statistics(std::get<StatisticsListRequest>(request), options);
        case CommandKind::QueueStats:
            return client.queue_statistics(std::get<QueueStatisticsListRequest>(request), options);
        case CommandKind::ScheduleValidate:
            return client.validate_schedule(std::get<CronScheduleInput>(request), options);
        case CommandKind::ScheduleNext:
            return client.next_schedule_occurrences(std::get<ScheduleNextRequest>(request), options);
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
            // An accepted suspension can finish through the last one-time run.
            // Return the observed outcome instead of waiting for a Suspended state it will never reach.
            if (is_terminal_job_state(job->state)) {
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
    Private(Command value, StandardAttributeRegistry const& attributes, SessionMode delivery)
        : command{std::move(value)}
        , registry{attributes}
        , mode{delivery}
    {}

    Command                          command;
    StandardAttributeRegistry const& registry;
    SessionMode                      mode;
    std::optional<SessionResult>     result;

    // Reverse destruction stops the timer and typed client before their borrowed RPC client and socket.
    LocalSocket                      socket;
    std::unique_ptr<jb::rpc::Client> rpc;
    std::unique_ptr<ControlClient>   control;
    Timer                            timer;
    Timer                            poll_timer;
    SessionPhase                     phase{SessionPhase::Idle};
    TimePoint                        deadline;
    std::optional<ControlCallId>     active_call;
    // Only an accepted final mutation can be uncertain; selection, enrichment and polling are reads.
    bool                             active_mutation{false};
    std::optional<Uuid>              poll_id;
    std::chrono::milliseconds        poll_interval{100};

    // Preserve the first lookup across the exact-name probe. Probes are sequential, not a namespace snapshot;
    // the resulting UUID prevents subsequent name changes from redirecting the final operation.
    std::optional<QueueSelectorIntent> queue_selection;
    std::optional<Queue>               selected_queue;
    std::optional<CommandRequest>      selected_request;
    std::optional<CliError>            selection_not_found;

    // Own callback values across enrichment calls. The summary pins the exact attempt.get identity;
    // keep it unrendered until details succeed or terminal failure needs the partial-result fallback.
    std::optional<RunDetails>     inspected_run;
    std::optional<AttemptSummary> inspected_attempt;

    // Selection owns no history pages. The decreasing boundary proves progress across cursor calls;
    // once selected, the exact key stays pinned even if retention or a retry changes the live history.
    std::optional<AttemptNumber> output_before;
    std::optional<std::string>   output_cursor;
    std::optional<AttemptKey>    output_attempt;
};

Session::Session(Command command, StandardAttributeRegistry const& registry, SessionMode mode)
    : Object{
          *new Private{std::move(command), registry, mode}
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
                       .message  = connection_error_message(state->command.socket_path),
                   },
                   state->active_call && state->active_mutation));
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

auto Session::take_result() -> std::optional<SessionResult>
{
    return std::exchange(d_ptr<Private>()->result, std::nullopt);
}

void Session::finish(int code, std::optional<CliError> error, std::optional<ControlReply> reply)
{
    auto* data = d_ptr<Private>();
    if (data->phase == SessionPhase::Finished) {
        return;
    }

    // Latch completion before close() emits any pending-call failures through receiver-aware observers.
    data->phase  = SessionPhase::Finished;
    data->result = SessionResult{.exit_code = code, .reply = std::move(reply), .error = std::move(error)};
    data->timer.stop();
    data->poll_timer.stop();
    if (data->control) {
        data->control->close();
    }

    if (data->inspected_run) {
        // Every failure path, including synchronous rejection, transport loss and timeout, preserves the summary.
        if (data->result->error && data->inspected_attempt) {
            if (auto text = format_last_attempt(*data->inspected_attempt)) {
                fmt::print(stdout, "{}", *text);
            }
        }
        fmt::print(stdout, "{}", run_output_hint(data->inspected_run->id, data->inspected_run->type));
        std::fflush(stdout);
    }
    if (data->mode == SessionMode::Render && data->result->error) {
        print_error(data->command.json, *data->result->error);
    }
    emit(finished, code);
}

void Session::deadline_expired()
{
    auto*      data    = d_ptr<Private>();
    auto const unknown = data->active_call && data->active_mutation;
    auto       message = std::string_view{"Overall command deadline expired"};
    if (data->phase == SessionPhase::Polling) {
        if (data->command.kind == CommandKind::RunCancel) {
            message = "Cancelled state was not confirmed before the deadline";
        }
        else if (data->command.kind == CommandKind::JobSuspend) {
            message = "Job drain was not confirmed before the deadline";
        }
        else {
            message = "Suspended state was not confirmed before the deadline";
        }
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
        finish(3, local_error(error, state->active_call && state->active_mutation));
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
        deliver_reply(ControlReply{info});
        return;
    }

    begin_queue_selection();
}

void Session::dispatch_command()
{
    auto* data    = d_ptr<Private>();
    auto  options = remaining_options(data->deadline);
    if (!options) {
        deadline_expired();
        return;
    }
    data->phase         = SessionPhase::Command;
    auto const& request = data->selected_request ? *data->selected_request : data->command.request;
    auto        call    = call_command(*data->control, data->command.kind, request, *options);
    if (!call) {
        finish(operation_exit(call.error()), local_error(call.error()));
        return;
    }
    data->active_call     = *call;
    data->active_mutation = is_mutation(data->command.kind);
}

void Session::begin_queue_selection()
{
    auto* data            = d_ptr<Private>();
    data->queue_selection = std::exchange(data->command.queue_selector_intent, std::nullopt);
    if (!data->queue_selection) {
        dispatch_command();
        return;
    }

    auto*      selector   = command_queue_selector(data->command.kind, data->command.request);
    auto const exact_name = data->queue_selection->form == QueueSelectionForm::ExactName;
    if (!selector || exact_name != std::holds_alternative<std::string>(*selector)) {
        finish(3,
               local_error({.category = ErrorCategory::Internal,
                            .code     = "jobuctl.session.invalid_selector",
                            .message  = "Queue selector intent does not match the command"}));
        return;
    }

    if (data->queue_selection->form == QueueSelectionForm::ExplicitId) {
        // Explicit IDs need no discovery read, including stats and lifecycle calls on older peers.
        if (data->command.kind == CommandKind::QueueSuspend && data->command.wait) {
            data->poll_id = std::get<Uuid>(*selector);
        }
        data->queue_selection.reset();
        dispatch_command();
        return;
    }
    request_queue_selection(false);
}

void Session::request_queue_selection(bool name_probe)
{
    auto* data    = d_ptr<Private>();
    auto  options = remaining_options(data->deadline);
    if (!options) {
        deadline_expired();
        return;
    }

    auto selector = name_probe ? QueueSelector{data->queue_selection->original_text}
                               : *command_queue_selector(data->command.kind, data->command.request);
    data->phase   = name_probe ? SessionPhase::QueueNameProbe : SessionPhase::QueueLookup;
    auto call     = data->control->get_queue(
        GetQueueRequest{.queue = std::move(selector), .include_deleted = data->command.kind == CommandKind::QueueStats},
        *options);
    if (!call) {
        finish(operation_exit(call.error()), local_error(call.error()));
        return;
    }
    data->active_call = *call;
}

void Session::receive_queue_selection(ControlReply const& reply)
{
    auto*       data     = d_ptr<Private>();
    auto const* queue    = std::get_if<Queue>(&reply);
    auto const  selector = data->phase == SessionPhase::QueueNameProbe
                             ? QueueSelector{data->queue_selection->original_text}
                             : *command_queue_selector(data->command.kind, data->command.request);
    auto const  matches =
        queue != nullptr && (std::holds_alternative<Uuid>(selector) ? queue->id == std::get<Uuid>(selector)
                                                                    : queue->name == std::get<std::string>(selector));
    if (!matches || (data->command.kind != CommandKind::QueueStats && queue->state == QueueState::Deleted)) {
        finish(3, invalid_session_response());
        return;
    }

    if (data->phase == SessionPhase::QueueLookup) {
        data->selected_queue = *queue;
        if (data->queue_selection->form == QueueSelectionForm::InferredId) {
            request_queue_selection(true);
            return;
        }
    }
    else if (!data->selected_queue) {
        // A UUID-shaped name never changes the original ID interpretation, even if it is the sole match.
        auto error = std::move(*data->selection_not_found);
        error.message +=
            fmt::format(". Use name:{} to select the exact queue name", data->queue_selection->original_text);
        finish(1, std::move(error));
        return;
    }
    else if (data->selected_queue->id != queue->id) {
        auto const& text = data->queue_selection->original_text;
        finish(1,
               local_error({.category = ErrorCategory::Conflict,
                            .code     = "jobuctl.queue.ambiguous_selector",
                            .message  = fmt::format("Queue selector is ambiguous. Use id:{} or name:{}", text, text)}));
        return;
    }

    complete_queue_selection();
}

void Session::queue_selection_missing(CliError error)
{
    auto* data = d_ptr<Private>();
    if (data->phase == SessionPhase::QueueLookup && data->queue_selection->form == QueueSelectionForm::InferredId) {
        data->selection_not_found = std::move(error);
        request_queue_selection(true);
        return;
    }
    if (data->phase == SessionPhase::QueueNameProbe && data->selected_queue) {
        complete_queue_selection();
        return;
    }

    // Preserve the original ID failure when neither interpretation exists.
    finish(1, data->selection_not_found ? std::move(*data->selection_not_found) : std::move(error));
}

void Session::complete_queue_selection()
{
    auto*      data = d_ptr<Private>();
    auto const id   = data->selected_queue->id;
    data->queue_selection.reset();
    data->selection_not_found.reset();
    if (data->command.kind == CommandKind::QueueGet) {
        // Own the first result across the probe and deliver it once without a duplicate get.
        auto queue = std::move(*data->selected_queue);
        data->selected_queue.reset();
        deliver_reply(ControlReply{std::move(queue)});
        return;
    }
    data->selected_queue.reset();

    // Keep the original request for rendering (notably delete acknowledgements), and pin only dispatch data.
    data->selected_request                                               = data->command.request;
    *command_queue_selector(data->command.kind, *data->selected_request) = id;
    if (data->command.kind == CommandKind::QueueSuspend && data->command.wait) {
        data->poll_id = id;
    }
    dispatch_command();
}

void Session::receive_reply(ControlCallId id, ControlReply const& reply)
{
    auto* data = d_ptr<Private>();
    if (!is_call_phase(data->phase) || data->active_call != id) {
        return;
    }
    data->active_call.reset();
    data->active_mutation = false;

    if (is_queue_selection_phase(data->phase)) {
        receive_queue_selection(reply);
        return;
    }

    if (data->command.kind == CommandKind::RunOutput) {
        receive_run_output(reply);
        return;
    }

    if (data->command.kind == CommandKind::RunGet && data->mode == SessionMode::Render && !data->command.json) {
        receive_run_inspection(reply);
        return;
    }

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
                deliver_reply(ControlReply{std::move(completed)});
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

    deliver_reply(reply);
}

void Session::receive_run_inspection(ControlReply const& reply)
{
    auto* data = d_ptr<Private>();
    if (data->phase == SessionPhase::Command) {
        auto const* run = std::get_if<RunDetails>(&reply);
        if (!run || run->id != std::get<Uuid>(data->command.request)) {
            finish(3, invalid_session_response());
            return;
        }
        if (!print_command_result(data->command, reply, data->registry)) {
            finish(3,
                   local_error({.category = ErrorCategory::Internal,
                                .code     = "jobuctl.output.invalid_reply",
                                .message  = "Unable to render the daemon reply"}));
            return;
        }

        // Display the successful run before starting enrichment; later failures must not hide it.
        std::fflush(stdout);
        data->inspected_run = *run;
        data->phase         = SessionPhase::InspectionList;
        request_run_inspection();
        return;
    }

    if (data->phase == SessionPhase::InspectionList) {
        auto const* page = std::get_if<AttemptPage>(&reply);
        if (!page || page->items.size() > 1U ||
            (!page->items.empty() && page->items.front().run_id != data->inspected_run->id)) {
            finish(3, invalid_session_response());
            return;
        }
        if (page->items.empty()) {
            fmt::print(stdout, "Last attempt: none\n");
            finish(0);
            return;
        }

        data->inspected_attempt = page->items.front();
        data->phase             = SessionPhase::InspectionDetails;
        request_run_inspection();
        return;
    }

    auto const* attempt  = std::get_if<AttemptDetails>(&reply);
    auto const& selected = *data->inspected_attempt;
    if (!attempt || attempt->run_id != selected.run_id || attempt->attempt_number != selected.attempt_number) {
        finish(3, invalid_session_response());
        return;
    }
    auto text = format_last_attempt(data->inspected_run->type, *attempt);
    if (!text) {
        finish(3, invalid_session_response());
        return;
    }

    // Details may observe a newer state than the list. Print only this authoritative snapshot on success.
    fmt::print(stdout, "{}", *text);
    data->inspected_attempt.reset();
    finish(0);
}

void Session::request_run_inspection()
{
    auto* data    = d_ptr<Private>();
    auto  options = remaining_options(data->deadline);
    if (!options) {
        deadline_expired();
        return;
    }

    // The original command and deadline never change. Only the active phase chooses a follow-up typed request.
    auto call = data->phase == SessionPhase::InspectionList
                  ? data->control->list_attempts(AttemptQuery{.run_id = data->inspected_run->id, .limit = 1}, *options)
                  : data->control->get_attempt(AttemptKey{.run_id         = data->inspected_attempt->run_id,
                                                          .attempt_number = data->inspected_attempt->attempt_number},
                                               *options);
    if (!call) {
        finish(operation_exit(call.error()), local_error(call.error()));
        return;
    }
    data->active_call = *call;
}

void Session::receive_run_output(ControlReply const& reply)
{
    auto*       data    = d_ptr<Private>();
    auto const& request = std::get<RunOutputRequest>(data->command.request);
    if (data->phase == SessionPhase::Command) {
        auto const* run = std::get_if<RunDetails>(&reply);
        if (!run || run->id != request.run_id) {
            finish(3, invalid_session_response());
            return;
        }
        if (!output_channel_matches(run->type, request.channel)) {
            finish(2,
                   local_error({.category = ErrorCategory::InvalidArgument,
                                .code     = "jobuctl.run.invalid_channel",
                                .message  = run->type == JobType::Cli ? "CLI run output requires stdout or stderr"
                                                                      : "HTTP run output requires body or headers"}));
            return;
        }

        data->phase = SessionPhase::OutputList;
        request_run_output();
        return;
    }

    if (data->phase == SessionPhase::OutputList) {
        auto const* page = std::get_if<AttemptPage>(&reply);
        if (!page || page->items.size() > 200U ||
            (page->next_cursor && (page->items.empty() || page->next_cursor == data->output_cursor))) {
            finish(3, invalid_session_response());
            return;
        }

        // Validate the whole page before trusting its first Completed member. Strict descent also rejects
        // replayed/overlapping pages without accumulating every page or every previously observed cursor.
        auto before = data->output_before;
        for (auto const& attempt : page->items) {
            if (attempt.run_id != request.run_id || (before && attempt.attempt_number >= *before)) {
                finish(3, invalid_session_response());
                return;
            }
            before = attempt.attempt_number;
        }

        if (auto selected = latest_completed_attempt(page->items)) {
            data->output_attempt = AttemptKey{.run_id = selected->run_id, .attempt_number = selected->attempt_number};
            data->output_cursor.reset();
            data->phase = SessionPhase::OutputChunk;
            request_run_output();
            return;
        }

        data->output_before = before;
        if (page->next_cursor) {
            // Short pages can reflect the response byte budget; only the cursor decides continuation.
            data->output_cursor = *page->next_cursor;
            request_run_output();
            return;
        }

        finish(1,
               local_error(
                   {.category = ErrorCategory::NotFound,
                    .code     = before ? "jobuctl.run.no_completed_attempt" : "jobuctl.run.no_attempts",
                    .message  = before ? "Run has no completed retained attempt" : "Run has no retained attempts"}));
        return;
    }

    auto const* chunk = std::get_if<AttemptOutputChunk>(&reply);
    if (!chunk || chunk->attempt.run_id != data->output_attempt->run_id ||
        chunk->attempt.attempt_number != data->output_attempt->attempt_number || chunk->channel != request.channel ||
        chunk->offset != request.offset || chunk->bytes_returned > request.limit) {
        finish(3, invalid_session_response());
        return;
    }

    // Intermediate lookups stay private in every delivery mode. next_offset is returned as data, never followed.
    deliver_reply(reply);
}

void Session::request_run_output()
{
    auto* data    = d_ptr<Private>();
    auto  options = remaining_options(data->deadline);
    if (!options) {
        deadline_expired();
        return;
    }

    // Every page and the single pinned output read share the deadline established before connecting.
    auto const& request = std::get<RunOutputRequest>(data->command.request);
    auto        call    = [&]() -> Result<ControlCallId, Error> {
        if (data->phase == SessionPhase::OutputList) {
            if (data->output_cursor) {
                return data->control->list_attempts(CursorRequest{.cursor = *data->output_cursor}, *options);
            }
            return data->control->list_attempts(AttemptQuery{.run_id = request.run_id, .limit = 200}, *options);
        }
        return data->control->read_attempt_output({.attempt = *data->output_attempt,
                                                   .channel = request.channel,
                                                   .offset  = request.offset,
                                                   .limit   = request.limit},
                                                  *options);
    }();
    if (!call) {
        finish(operation_exit(call.error()), local_error(call.error()));
        return;
    }
    data->active_call = *call;
}

void Session::deliver_reply(ControlReply const& reply)
{
    auto* data = d_ptr<Private>();
    if (data->mode == SessionMode::Capture) {
        // Typed-client signal arguments are borrowed; own the final value before returning to its callback.
        finish(0, std::nullopt, reply);
        return;
    }

    if (is_output_command(data->command.kind) && (data->command.raw || data->command.output_file)) {
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
    if (!is_call_phase(data->phase) || data->active_call != id) {
        return;
    }
    data->active_call.reset();
    data->active_mutation = false;
    if (failure.kind == ControlFailureKind::Remote) {
        auto error = remote_error(std::get<jb::rpc::RpcError>(failure.error));
        if (is_queue_selection_phase(data->phase)) {
            if (is_queue_not_found(error)) {
                queue_selection_missing(std::move(error));
                return;
            }
            if (error.code == "jobu.queue.ambiguous_deleted_name") {
                error.message += ". Use id:UUID or --id UUID to select a retained queue explicitly";
            }
        }
        finish(1, std::move(error));
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
