#include "job_edit_workflow_priv.hpp"

#include "commands/commands_priv.hpp"
#include "edit_files_priv.hpp"
#include "edit_recovery_priv.hpp"
#include "editor_command_priv.hpp"
#include "job.hpp"
#include "job_edit_priv.hpp"
#include "output_priv.hpp"
#include "run_session_priv.hpp"

#include <fmt/format.h>

#include <cstdio> // IWYU pragma: keep for stderr macro
#include <cstdlib>
#include <optional>
#include <string_view>
#include <utility>
#include <variant>

namespace jb::jobuctl::detail {
namespace {

using namespace jb::core;

auto environment_value(char const* name) -> std::string_view
{
    auto const* value = std::getenv(name);
    return value ? std::string_view{value} : std::string_view{};
}

void print_retained_directory(std::filesystem::path const& directory)
{
    if (!directory.empty()) {
        fmt::print(stderr, "Retained edit directory: {}\n", escape_human(directory.string()));
    }
}

auto fail_locally(bool json, Error const& error) -> int
{
    print_error(json, local_error(error));
    return 2;
}

void print_recovery(Command const& command, EditFiles const& files, EditRecoveryReason reason)
{
    auto const paths  = files.artifacts();
    auto       advice = format_edit_recovery(EditResource::Job, command, reason, paths);
    print_retained_directory(paths.directory);
    auto const* draft_label = reason == EditRecoveryReason::MissingDraft ? "Missing draft" : "Retained draft";
    fmt::print(stderr, "{}: {}\n", draft_label, escape_human(paths.draft.string()));
    if (!advice) {
        print_error(command.json, local_error(advice.error()));
        return;
    }
    fmt::print(stderr, "{}\n", advice->instructions);
    if (advice->paths.request) {
        fmt::print(stderr, "Minimal update request: {}\n", escape_human(advice->paths.request->string()));
    }
    if (advice->retry_command) {
        fmt::print(stderr, "Recovery command: {}\n", escape_human(*advice->retry_command));
    }
}

auto cleanup_edit(Command const& command, EditFiles& files, int exit_code) -> int
{
    auto cleaned = files.cleanup();
    if (!cleaned) {
        print_error(command.json, local_error(cleaned.error()));
        print_retained_directory(files.artifacts().directory);
        return exit_code == 0 ? 3 : exit_code;
    }
    return exit_code;
}

} // namespace

auto run_job_edit(Command command, jb::jobu::StandardAttributeRegistry const& registry, EditWorkflowIo io) -> int
{
    auto const* edit = std::get_if<InteractiveJobUpdate>(&command.request);
    if (!edit || command.kind != CommandKind::JobUpdate) {
        return fail_locally(command.json,
                            {.category = ErrorCategory::InvalidArgument,
                             .code     = "jobuctl.edit.invalid_command",
                             .message  = "Interactive update requires a parsed bare job update"});
    }

    // Local help has already returned. Refuse noninteractive use before environment processing or any fetch.
    auto terminal = io.check_terminal ? io.check_terminal() : check_editor_terminal();
    if (!terminal) {
        return fail_locally(command.json, terminal.error());
    }
    auto editor = resolve_editor_command(
        {.override_command = edit->editor ? std::optional<std::string_view>{*edit->editor} : std::nullopt,
         .visual           = environment_value("VISUAL"),
         .editor           = environment_value("EDITOR")});
    if (!editor) {
        return fail_locally(command.json, editor.error());
    }

    if (!io.run) {
        io.run = [&registry](Command next, SessionMode mode) { return run_session(std::move(next), registry, mode); };
    }
    if (!io.launch) {
        io.launch = [](auto argv, auto const& draft) { return launch_editor(argv, draft); };
    }

    auto fetch    = command;
    fetch.kind    = CommandKind::JobGet;
    fetch.method  = "job.get";
    fetch.request = edit->job_id;
    auto fetched  = io.run(std::move(fetch), SessionMode::Capture);
    if (fetched.exit_code != 0) {
        if (fetched.error) {
            print_error(command.json, *fetched.error);
        }
        return fetched.exit_code;
    }
    auto const* job = fetched.reply ? std::get_if<jb::jobu::JobDefinition>(&*fetched.reply) : nullptr;
    if (!job || job->id != edit->job_id) {
        print_error(command.json,
                    local_error({.category = ErrorCategory::Internal,
                                 .code     = "jobuctl.edit.invalid_reply",
                                 .message  = "Unable to edit the requested job from the daemon reply"}));
        return 3;
    }

    // Freeze the displayed identity/revision and comparison values before creating an editable file.
    auto baseline = JobEditBaseline::from_job(*job, registry);
    if (!baseline) {
        return fail_locally(command.json, baseline.error());
    }
    auto formatted = format_job_edit_draft(baseline->value());
    if (!formatted) {
        return fail_locally(command.json, formatted.error());
    }
    auto created = EditFiles::create(EditResource::Job, *formatted, io.temporary_parent);
    if (!created) {
        print_retained_directory(created.error().retained_directory);
        return fail_locally(command.json, created.error().error);
    }

    // The runner has destroyed Session and Application. No socket or deadline survives while a person edits.
    auto files    = std::move(created).value();
    auto launched = io.launch(*editor, files.artifacts().draft);
    if (!launched) {
        print_error(command.json, local_error(launched.error()));
        auto const code = launched.error().category == ErrorCategory::Cancelled ? 1 : 3;
        return cleanup_edit(command, files, code);
    }

    // Editor exit zero is only permission to inspect the saved file; invalid work remains available for recovery.
    auto draft = files.read_draft();
    if (!draft) {
        // Use the read result; a second filesystem probe could observe a different file.
        auto reason = EditRecoveryReason::InvalidDraft;
        if (draft.error().code == "jobuctl.edit.missing_draft") {
            reason = EditRecoveryReason::MissingDraft;
        }
        print_recovery(command, files, reason);
        return fail_locally(command.json, draft.error());
    }
    auto prepared = prepare_job_update(*baseline, *draft, registry);
    if (!prepared) {
        print_recovery(command, files, EditRecoveryReason::InvalidDraft);
        return fail_locally(command.json, prepared.error());
    }
    if (!*prepared) {
        fmt::print(stderr, "No changes; no update submitted\n");
        return cleanup_edit(command, files, 0);
    }

    // Persist exactly the validated minimal params before the mutation can reach the server.
    auto& update  = **prepared;
    auto  written = files.write_request(update.params);
    if (!written) {
        print_recovery(command, files, EditRecoveryReason::InvalidDraft);
        return fail_locally(command.json, written.error());
    }

    auto submit    = command;
    submit.request = std::move(update.request);
    // The complete configured timeout starts afresh, including connection and handshake; editor time is irrelevant.
    auto submitted = io.run(std::move(submit), SessionMode::Render);
    if (submitted.exit_code != 0) {
        // Render already delivered the existing error, including outcome_unknown. Add recovery without repeating it.
        auto reason = submitted.error ? edit_recovery_reason(EditResource::Job, *submitted.error)
                                      : EditRecoveryReason::SubmissionFailed;
        print_recovery(command, files, reason);
        return submitted.exit_code;
    }
    return cleanup_edit(command, files, 0);
}

} // namespace jb::jobuctl::detail
