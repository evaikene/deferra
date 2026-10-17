#pragma once

#include "error.hpp"
#include "result.hpp"

#include <filesystem>
#include <functional>
#include <span>
#include <spawn.h>
#include <string>
#include <sys/types.h>

namespace jb::jobuctl::detail {

/// Synchronous POSIX-only test seams. Empty operations use the native implementation; no callback is retained.
/// open_terminal transfers an owned descriptor, or returns -1 with errno. Native open also verifies foreground use.
/// spawn follows posix_spawnp's error-number convention and receives a null-terminated argv pointer array.
/// wait follows waitpid(pid, status, 0), including errno on failure; it must never reap another child.
/// Injected operations must not throw and their captures must outlive the launch call.
struct EditorPosixIo {
    std::function<int()> open_terminal;
    std::function<int(pid_t*, std::span<char* const>, posix_spawn_file_actions_t const*, posix_spawnattr_t const*)>
                                      spawn;
    std::function<pid_t(pid_t, int*)> wait;
};

/// A workflow can replace the whole blocking launch with an in-process fake, without starting any child.
using EditorLauncher =
    std::function<jb::core::Result<void, jb::core::Error>(std::span<std::string const>, std::filesystem::path const&)>;

/// Appends the absolute draft path as one argument and synchronously launches/waits for exactly one editor child.
/// Child stdio uses the foreground controlling terminal; the child inherits the environment and parent's group.
/// Parent SIGINT/SIGQUIT dispositions and SIGCHLD mask changes are scoped; child INT/QUIT are default and unblocked.
/// Run only after the fetch Application/Session is destroyed, without other threads changing signal policy or
/// competing child reapers. Requires waitable SIGCHLD (neither SIG_IGN nor SA_NOCLDWAIT).
/// Exit zero succeeds; nonzero/signal exits return Cancelled errors; setup/spawn/wait errors remain distinguishable.
/// Errors never echo configuration/draft contents. No network deadline, output capture, or file cleanup is performed.
[[nodiscard]] auto launch_editor(std::span<std::string const> command,
                                 std::filesystem::path const& draft,
                                 EditorPosixIo                io = {}) -> jb::core::Result<void, jb::core::Error>;

} // namespace jb::jobuctl::detail
