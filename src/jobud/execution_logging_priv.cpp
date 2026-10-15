#include "execution_logging_priv.hpp"

#include "logging.hpp"
#include "utc_timestamp.hpp"

#include <fmt/format.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace jb::jobud::detail {
namespace {

using namespace jb::jobu;
using jb::core::LogField;
using jb::core::LogLevel;

auto type_text(JobType type) -> std::string_view
{
    return type == JobType::Cli ? "cli" : "http";
}

auto type_label(JobType type) -> std::string_view
{
    return type == JobType::Cli ? "CLI" : "HTTP";
}

auto origin_text(RunOrigin origin) -> std::string_view
{
    switch (origin) {
        case RunOrigin::Scheduled:
            return "scheduled";
        case RunOrigin::Manual:
            return "manual";
        case RunOrigin::Submitted:
            return "submitted";
    }
    return "unknown";
}

auto phase_text(AttemptStartPhase phase) -> std::string_view
{
    switch (phase) {
        case AttemptStartPhase::Accepted:
            return "accepted";
        case AttemptStartPhase::PreparationFailed:
            return "preparation_failed";
        case AttemptStartPhase::ExecutorRejected:
            return "executor_rejected";
    }
    return "unknown";
}

/// Keep the fixed string storage separate from the growable field vector: its views never
/// point into a container that can relocate strings while optional fields are appended.
struct ExecutionFields {
    ExecutionFields(ExecutionContext const& context, std::string_view time_name, jb::core::UtcTimePoint time)
        : queue_id{
              context.queue_id.to_string()
    }
        , job_id{context.job_id.to_string()}
        , run_id{context.run_id.to_string()}
        , values{{.name = "queue_id", .value = std::string_view{queue_id}},
                 {.name = "job_id", .value = std::string_view{job_id}},
                 {.name = "run_id", .value = std::string_view{run_id}},
                 {.name = "type", .value = type_text(context.type)},
                 {.name = "origin", .value = origin_text(context.origin)}}
    {
        auto formatted = format_utc_timestamp(time);
        if (formatted) {
            timestamp = std::move(*formatted);
            values.push_back({.name = time_name, .value = std::string_view{timestamp}});
        }
    }

    ExecutionFields(ExecutionFields const&)                    = delete;
    auto operator=(ExecutionFields const&) -> ExecutionFields& = delete;

    std::string           queue_id;
    std::string           job_id;
    std::string           run_id;
    std::string           timestamp;
    std::vector<LogField> values;
};

auto attempt_message(AttemptFinished const& event, std::string_view outcome) -> std::string
{
    auto message = fmt::format("{} attempt {} {}", type_label(event.execution.type), event.attempt_number, outcome);
    if (event.start_phase == AttemptStartPhase::PreparationFailed) {
        message += " during preparation";
    }
    else if (event.start_phase == AttemptStartPhase::ExecutorRejected) {
        message += " because the executor rejected it";
    }
    else if (event.signal_number) {
        message += fmt::format(" with signal {}", *event.signal_number);
    }
    else if (event.exit_code) {
        message += fmt::format(" with exit code {}", *event.exit_code);
    }
    else if (event.http_status) {
        message += fmt::format(" with HTTP status {}", *event.http_status);
    }
    message += '.';
    return message;
}

} // namespace

void log_attempt_started(jb::jobu::AttemptStarted const& event)
{
    ExecutionFields fields{event.execution, "started_at", event.started_at};
    fields.values.push_back({.name = "attempt_number", .value = event.attempt_number});
    auto message = fmt::format("{} attempt {} started.", type_label(event.execution.type), event.attempt_number);
    jb::core::log_event_message(LogLevel::Debug1, "jobud.attempt.started", message, fields.values);
}

void log_attempt_finished(jb::jobu::AttemptFinished const& event)
{
    std::string_view name;
    std::string_view outcome;
    switch (event.outcome) {
        case AttemptOutcome::Succeeded:
            name    = "jobud.attempt.succeeded";
            outcome = "succeeded";
            break;
        case AttemptOutcome::Failed:
            name    = "jobud.attempt.failed";
            outcome = "failed";
            break;
        case AttemptOutcome::Cancelled:
            name    = "jobud.attempt.cancelled";
            outcome = "cancelled";
            break;
        case AttemptOutcome::Interrupted:
            return;
    }

    ExecutionFields fields{event.execution, "completed_at", event.completed_at};
    fields.values.push_back({.name = "attempt_number", .value = event.attempt_number});
    if (event.outcome == AttemptOutcome::Failed) {
        fields.values.push_back({.name = "start_phase", .value = phase_text(event.start_phase)});
    }
    if (event.elapsed) {
        fields.values.push_back({.name = "elapsed_ms", .value = static_cast<std::int64_t>(event.elapsed->count())});
    }
    if (event.exit_code) {
        fields.values.push_back({.name = "exit_code", .value = static_cast<std::int64_t>(*event.exit_code)});
    }
    if (event.signal_number) {
        fields.values.push_back({.name = "signal_number", .value = static_cast<std::int64_t>(*event.signal_number)});
    }
    if (event.http_status) {
        fields.values.push_back({.name = "http_status", .value = static_cast<std::uint64_t>(*event.http_status)});
    }

    // The scheduler's effective outcome is authoritative, even for nonzero exits and non-2xx statuses.
    auto message = attempt_message(event, outcome);
    jb::core::log_event_message(LogLevel::Debug1, name, message, fields.values);
}

void log_retry_scheduled(jb::jobu::RetryScheduled const& event)
{
    ExecutionFields fields{event.execution, "runnable_at", event.runnable_at};
    fields.values.push_back({.name = "completed_attempt", .value = event.completed_attempt});
    fields.values.push_back({.name = "next_attempt", .value = event.next_attempt});
    fields.values.push_back({.name  = "retry_mode",
                             .value = std::string_view{event.mode == RetryMode::Blocking ? "blocking" : "reschedule"}});
    auto message = fields.timestamp.empty() ? std::string{"Run retry scheduled; due time is unavailable."}
                                            : fmt::format("Run retry scheduled for {}.", fields.timestamp);
    jb::core::log_event_message(LogLevel::Debug1, "jobud.run.retry_scheduled", message, fields.values);
}

void log_run_finished(jb::jobu::RunFinished const& event)
{
    std::string_view name;
    std::string_view message;
    auto             level = LogLevel::Debug1;
    switch (event.state) {
        case RunState::Succeeded:
            name    = "jobud.run.succeeded";
            message = "Run succeeded.";
            break;
        case RunState::Failed:
            name    = "jobud.run.failed";
            message = "Run failed; no further retry is scheduled.";
            level   = LogLevel::Warning;
            break;
        case RunState::Cancelled:
            name    = "jobud.run.cancelled";
            message = "Run cancelled.";
            break;
        default:
            // Recovery reports Interrupted work separately; nonterminal states are not run completions.
            return;
    }

    ExecutionFields fields{event.execution, "completed_at", event.completed_at};
    if (event.completing_attempt) {
        fields.values.push_back({.name = "completing_attempt", .value = *event.completing_attempt});
    }
    jb::core::log_event_message(level, name, message, fields.values);
}

} // namespace jb::jobud::detail
