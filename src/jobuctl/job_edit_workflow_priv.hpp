#pragma once

#include "attribute_registry.hpp"
#include "command_line_priv.hpp"
#include "editor_posix_priv.hpp"
#include "error.hpp"
#include "result.hpp"
#include "session_priv.hpp"

#include <filesystem>
#include <functional>

namespace jb::jobuctl::detail {

/// Synchronous, non-Object workflow seams; empty operations select the native implementation.
/// A replacement runner must honor Capture/Render delivery and return only after its network scope is destroyed.
/// Callbacks are borrowed for one invocation. No callback or registry reference escapes the workflow.
struct JobEditWorkflowIo {
    std::function<jb::core::Result<void, jb::core::Error>()> check_terminal;
    EditorLauncher                                           launch;
    std::function<SessionResult(Command, SessionMode)>       run;
    /// Empty selects the system temporary directory, as in JobEditFiles::create().
    std::filesystem::path                                    temporary_parent;
};

/// Runs a parsed InteractiveJobUpdate without an existing Application.
/// Captures one definition, destroys the fetch scope, edits synchronously, then submits one minimal update in a
/// fresh Render session with the original revision and complete timeout. No-op produces only a stderr notice.
/// Success/no-op/cancellation clean known files; draft/submission failures retain work and print safe recovery paths.
/// Uses existing CLI exit conventions; never refreshes the revision, retries, or launches a second editor.
[[nodiscard]] auto
run_job_edit(Command command, jb::jobu::StandardAttributeRegistry const& registry, JobEditWorkflowIo io = {}) -> int;

} // namespace jb::jobuctl::detail
