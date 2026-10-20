#pragma once

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
struct EditWorkflowIo {
    std::function<jb::core::Result<void, jb::core::Error>()> check_terminal;
    EditorLauncher                                           launch;
    std::function<SessionResult(Command, SessionMode)>       run;
    /// Empty selects the system temporary directory, as in EditFiles::create().
    std::filesystem::path                                    temporary_parent;
};

} // namespace jb::jobuctl::detail
