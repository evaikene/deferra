#pragma once

#include "command_line_priv.hpp"
#include "session_priv.hpp"

namespace jb::jobuctl::detail {

/// Runs one command with its own Application and network state; no Application may already exist.
/// The registry is borrowed for this call. Returns owning data after completion callbacks have unwound
/// and both Session and Application have been destroyed, so sequential calls start with fresh lifetimes/deadlines.
[[nodiscard]] auto run_session(Command                                    command,
                               jb::jobu::StandardAttributeRegistry const& registry,
                               SessionMode                                mode = SessionMode::Render) -> SessionResult;

} // namespace jb::jobuctl::detail
