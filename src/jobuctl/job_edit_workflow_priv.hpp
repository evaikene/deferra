#pragma once

#include "attribute_registry.hpp"
#include "command_line_priv.hpp"
#include "edit_workflow_priv.hpp"

namespace jb::jobuctl::detail {

/// Runs a parsed InteractiveJobUpdate without an existing Application.
/// Captures one definition, destroys the fetch scope, edits synchronously, then submits one minimal update in a
/// fresh Render session with the original revision and complete timeout. No-op produces only a stderr notice.
/// Success/no-op/cancellation clean known files; draft/submission failures retain work and print safe recovery paths.
/// Uses existing CLI exit conventions; never refreshes the revision, retries, or launches a second editor.
[[nodiscard]] auto
run_job_edit(Command command, jb::jobu::StandardAttributeRegistry const& registry, EditWorkflowIo io = {}) -> int;

} // namespace jb::jobuctl::detail
