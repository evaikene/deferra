#include "job_edit_recovery_priv.hpp"

#include <array>
#include <utility>

namespace jb::jobuctl::detail {

using namespace jb::core;

auto quote_shell_argv(std::span<std::string const> arguments) -> Result<std::string, Error>
{
    auto command = std::string{};
    for (auto const& argument : arguments) {
        if (argument.find('\0') != std::string::npos) {
            return Result<std::string, Error>::failure({.category = ErrorCategory::InvalidArgument,
                                                        .code     = "jobuctl.edit.invalid_recovery_argument",
                                                        .message  = "Recovery command arguments cannot contain NUL"});
        }
        if (!command.empty()) {
            command += ' ';
        }
        command += '\'';
        for (auto const character : argument) {
            // End the quoted segment, escape the quote outside it, then resume quoting.
            if (character == '\'') {
                command += "'\\''";
            }
            else {
                command += character;
            }
        }
        command += '\'';
    }
    return Result<std::string, Error>::success(std::move(command));
}

auto format_job_edit_recovery(Command const& command, JobEditRecoveryReason reason, JobEditArtifacts paths)
    -> Result<JobEditRecoveryAdvice, Error>
{
    auto advice = JobEditRecoveryAdvice{.paths = std::move(paths)};
    if (reason == JobEditRecoveryReason::MissingDraft) {
        advice.paths.request.reset();
        advice.instructions = "No complete minimal request is available. The draft file is missing. "
                              "Start a new interactive edit to recreate the intended changes.";
        return Result<JobEditRecoveryAdvice, Error>::success(std::move(advice));
    }

    if (reason == JobEditRecoveryReason::InvalidDraft || !advice.paths.request) {
        advice.paths.request.reset();
        advice.instructions =
            "No complete minimal request is available. Correct the retained draft into a valid update "
            "request before using request-file mode, removing all JSONC comments. "
            "Keep the original job_id and expected_revision and select only intended changed fields. "
            "That mode requires strict JSON and sends every explicitly present field; "
            "it does not repeat the interactive diff.";
        return Result<JobEditRecoveryAdvice, Error>::success(std::move(advice));
    }

    switch (reason) {
        case JobEditRecoveryReason::InvalidDraft:
        case JobEditRecoveryReason::MissingDraft:
            break;
        case JobEditRecoveryReason::SubmissionFailed:
            advice.instructions = "The edited draft and minimal update request are retained. The command below submits "
                                  "the saved minimal request with its original revision.";
            break;
        case JobEditRecoveryReason::RevisionConflict:
            advice.instructions =
                "Fetch the current definition and manually reconcile the edit before submitting again. "
                "The saved request still contains the old revision; blindly retrying it does not fix "
                "the conflict.";
            break;
        case JobEditRecoveryReason::OutcomeUnknown:
            advice.instructions = "The update outcome is unknown. Inspect current server state before any retry; the "
                                  "update may already have succeeded. Both the draft and minimal request are retained.";
            break;
    }

    auto const arguments = std::array{std::string{"jobuctl"},
                                      std::string{"--socket"},
                                      command.socket_path.string(),
                                      std::string{"--timeout"},
                                      std::to_string(command.timeout.count()),
                                      std::string{"job"},
                                      std::string{"update"},
                                      std::string{"--request-file"},
                                      advice.paths.request->string()};
    auto       quoted    = quote_shell_argv(arguments);
    if (!quoted) {
        return Result<JobEditRecoveryAdvice, Error>::failure(std::move(quoted).error());
    }
    advice.retry_command = std::move(quoted).value();
    return Result<JobEditRecoveryAdvice, Error>::success(std::move(advice));
}

} // namespace jb::jobuctl::detail
