#pragma once

#include "attribute_registry.hpp"
#include "command_line_priv.hpp"
#include "edit_workflow_priv.hpp"

namespace jb::jobuctl::detail {

/// Runs a parsed InteractiveQueueUpdate without an existing Application.
/// One live-only Capture session resolves the owning selector intent and freezes the fetched UUID/revision.
/// That network scope is destroyed before editing; one fresh Render session submits only the minimal UUID update,
/// without another lookup or revision refresh. Editor time consumes neither session's complete timeout.
/// Success/no-op/cancellation clean known files; invalid drafts and submission failures retain safe recovery paths.
/// No-op leaves stdout empty. Uses existing CLI exit conventions, file limits, and the shared editor launcher.
[[nodiscard]] auto
run_queue_edit(Command command, jb::jobu::StandardAttributeRegistry const& registry, EditWorkflowIo io = {}) -> int;

} // namespace jb::jobuctl::detail
