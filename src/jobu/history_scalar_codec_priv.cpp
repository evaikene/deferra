#include "history_scalar_codec_priv.hpp"

namespace jb::jobu::detail {

auto history_wire_text(JobType value) noexcept -> std::optional<std::string_view>
{
    switch (value) {
        case JobType::Cli:
            return "cli";
        case JobType::Http:
            return "http";
    }
    return std::nullopt;
}

auto history_wire_text(RunOrigin value) noexcept -> std::optional<std::string_view>
{
    switch (value) {
        case RunOrigin::Scheduled:
            return "scheduled";
        case RunOrigin::Manual:
            return "manual";
        case RunOrigin::Submitted:
            return std::nullopt; // Reserved until a future submission API exists.
    }
    return std::nullopt;
}

auto history_wire_text(RunState value) noexcept -> std::optional<std::string_view>
{
    switch (value) {
        case RunState::Scheduled:
            return "scheduled";
        case RunState::Running:
            return "running";
        case RunState::RetryWait:
            return "retry_wait";
        case RunState::Succeeded:
            return "succeeded";
        case RunState::Failed:
            return "failed";
        case RunState::Interrupted:
            return "interrupted";
        case RunState::Cancelled:
            return "cancelled";
    }
    return std::nullopt;
}

auto history_wire_text(AttemptState value) noexcept -> std::optional<std::string_view>
{
    switch (value) {
        case AttemptState::Pending:
            return "pending";
        case AttemptState::Running:
            return "running";
        case AttemptState::Completed:
            return "completed";
    }
    return std::nullopt;
}

auto history_wire_text(AttemptOutcome value) noexcept -> std::optional<std::string_view>
{
    switch (value) {
        case AttemptOutcome::Succeeded:
            return "succeeded";
        case AttemptOutcome::Failed:
            return "failed";
        case AttemptOutcome::Interrupted:
            return "interrupted";
        case AttemptOutcome::Cancelled:
            return "cancelled";
    }
    return std::nullopt;
}

auto parse_history_wire_text(std::string_view text, JobType& value) noexcept -> bool
{
    if (text == "cli") {
        value = JobType::Cli;
    }
    else if (text == "http") {
        value = JobType::Http;
    }
    else {
        return false;
    }
    return true;
}

auto parse_history_wire_text(std::string_view text, RunOrigin& value) noexcept -> bool
{
    if (text == "scheduled") {
        value = RunOrigin::Scheduled;
    }
    else if (text == "manual") {
        value = RunOrigin::Manual;
    }
    else {
        return false;
    }
    return true;
}

auto parse_history_wire_text(std::string_view text, RunState& value) noexcept -> bool
{
    if (text == "scheduled") {
        value = RunState::Scheduled;
    }
    else if (text == "running") {
        value = RunState::Running;
    }
    else if (text == "retry_wait") {
        value = RunState::RetryWait;
    }
    else if (text == "succeeded") {
        value = RunState::Succeeded;
    }
    else if (text == "failed") {
        value = RunState::Failed;
    }
    else if (text == "interrupted") {
        value = RunState::Interrupted;
    }
    else if (text == "cancelled") {
        value = RunState::Cancelled;
    }
    else {
        return false;
    }
    return true;
}

auto parse_history_wire_text(std::string_view text, AttemptState& value) noexcept -> bool
{
    if (text == "pending") {
        value = AttemptState::Pending;
    }
    else if (text == "running") {
        value = AttemptState::Running;
    }
    else if (text == "completed") {
        value = AttemptState::Completed;
    }
    else {
        return false;
    }
    return true;
}

auto parse_history_wire_text(std::string_view text, AttemptOutcome& value) noexcept -> bool
{
    if (text == "succeeded") {
        value = AttemptOutcome::Succeeded;
    }
    else if (text == "failed") {
        value = AttemptOutcome::Failed;
    }
    else if (text == "interrupted") {
        value = AttemptOutcome::Interrupted;
    }
    else if (text == "cancelled") {
        value = AttemptOutcome::Cancelled;
    }
    else {
        return false;
    }
    return true;
}

} // namespace jb::jobu::detail
