#pragma once

#include "command_line_priv.hpp"
#include "edit_files_priv.hpp"
#include "edit_resource_priv.hpp"
#include "error.hpp"
#include "output_priv.hpp"
#include "result.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace jb::jobuctl::detail {

enum class EditRecoveryReason : std::uint8_t {
    InvalidDraft,
    MissingDraft,
    SubmissionFailed,
    RevisionConflict,
    OutcomeUnknown
};

/// A display projection only; no recovery command is executed and no file is read.
struct EditRecoveryAdvice {
    EditArtifacts              paths;
    std::string                instructions;
    std::optional<std::string> retry_command;
};

/// Quotes each argv element for a POSIX shell, including empty arguments and embedded single quotes.
/// Rejects NUL, which cannot occur in a process argument. Returned shell text may contain literal control bytes;
/// renderers must handle terminal display separately, without changing the command's argument meaning.
[[nodiscard]] auto quote_shell_argv(std::span<std::string const> arguments)
    -> jb::core::Result<std::string, jb::core::Error>;

/// Classifies a failed submission using only the fixed resource conflict code; uncertain outcome takes precedence.
[[nodiscard]] auto edit_recovery_reason(EditResource resource, CliError const& error) -> EditRecoveryReason;

/// Preserves the resolved socket and timeout with the existing request-file syntax (no positional identity).
/// Only a complete minimal request gets a command. Conflict/unknown-outcome instructions require inspection
/// and reconciliation before retry; invalid drafts are never advertised as ready-to-submit requests.
/// A missing draft requires a new edit rather than correction of a retained file.
[[nodiscard]] auto
format_edit_recovery(EditResource resource, Command const& command, EditRecoveryReason reason, EditArtifacts paths)
    -> jb::core::Result<EditRecoveryAdvice, jb::core::Error>;

} // namespace jb::jobuctl::detail
