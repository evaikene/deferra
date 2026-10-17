#include "editor_posix_priv.hpp"

#include <fmt/format.h>

#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

#ifdef __APPLE__
#  include <crt_externs.h>
#endif

namespace jb::jobuctl::detail {
namespace {

using namespace jb::core;
using Completion = Result<void, Error>;

auto system_failure(char const* code, char const* message, int number) -> Completion
{
    return Completion::failure(
        {.category = ErrorCategory::Io, .code = code, .message = fmt::format("{} (system error {})", message, number)});
}

class Descriptor final {
public:
    explicit Descriptor(int value)
        : _value{value}
    {}

    ~Descriptor() { close(); }

    Descriptor(Descriptor const&)                    = delete;
    auto operator=(Descriptor const&) -> Descriptor& = delete;

    auto get() const -> int { return _value; }

    void close()
    {
        auto const value = std::exchange(_value, -1);
        if (value >= 0) {
            // close() is never retried: EINTR may already have released the descriptor.
            static_cast<void>(::close(value));
        }
    }

private:
    int _value;
};

auto open_foreground_terminal() -> int
{
    auto const descriptor = ::open("/dev/tty", O_RDWR | O_CLOEXEC);
    if (descriptor < 0) {
        return -1;
    }
    auto const foreground = ::tcgetpgrp(descriptor);
    if (foreground < 0 || foreground != ::getpgrp()) {
        auto const number = foreground < 0 ? errno : EPERM;
        static_cast<void>(::close(descriptor));
        errno = number;
        return -1;
    }
    return descriptor;
}

class ParentSignals final {
public:
    ParentSignals() = default;

    ~ParentSignals() { static_cast<void>(restore()); }

    ParentSignals(ParentSignals const&)                    = delete;
    auto operator=(ParentSignals const&) -> ParentSignals& = delete;

    auto install() -> Completion
    {
        struct sigaction child{};
        if (::sigaction(SIGCHLD, nullptr, &child) != 0) {
            return system_failure("jobuctl.editor.signal_failed",
                                  "Unable to inspect editor child signal policy",
                                  errno);
        }
        if (child.sa_handler == SIG_IGN || (child.sa_flags & SA_NOCLDWAIT) != 0) {
            return Completion::failure({.category = ErrorCategory::Unavailable,
                                        .code     = "jobuctl.editor.wait_unavailable",
                                        .message  = "Editor launch requires waitable child processes"});
        }

        // Block child notifications while this scope owns the PID; waitpid still waits for that exact child.
        sigset_t blocked{};
        static_cast<void>(::sigemptyset(&blocked));
        static_cast<void>(::sigaddset(&blocked, SIGCHLD));
        if (::sigprocmask(SIG_BLOCK, &blocked, &_mask) != 0) {
            return system_failure("jobuctl.editor.signal_failed", "Unable to block editor child notifications", errno);
        }
        _mask_saved = true;

        struct sigaction ignored{};
        ignored.sa_handler = SIG_IGN;
        static_cast<void>(::sigemptyset(&ignored.sa_mask));
        if (::sigaction(SIGINT, &ignored, &_interrupt) != 0) {
            return system_failure("jobuctl.editor.signal_failed", "Unable to protect editor parent from SIGINT", errno);
        }
        _interrupt_saved = true;
        if (::sigaction(SIGQUIT, &ignored, &_quit) != 0) {
            return system_failure("jobuctl.editor.signal_failed",
                                  "Unable to protect editor parent from SIGQUIT",
                                  errno);
        }
        _quit_saved = true;
        return Completion::success();
    }

    auto child_mask() const -> sigset_t
    {
        auto mask = _mask;
        // The editor must receive terminal interrupts even if the caller had temporarily blocked them.
        static_cast<void>(::sigdelset(&mask, SIGINT));
        static_cast<void>(::sigdelset(&mask, SIGQUIT));
        return mask;
    }

    auto restore() -> int
    {
        auto number = 0;
        if (_quit_saved) {
            if (::sigaction(SIGQUIT, &_quit, nullptr) == 0) {
                _quit_saved = false;
            }
            else {
                number = errno;
            }
        }
        if (_interrupt_saved) {
            if (::sigaction(SIGINT, &_interrupt, nullptr) == 0) {
                _interrupt_saved = false;
            }
            else if (number == 0) {
                number = errno;
            }
        }
        if (_mask_saved) {
            if (::sigprocmask(SIG_SETMASK, &_mask, nullptr) == 0) {
                _mask_saved = false;
            }
            else if (number == 0) {
                number = errno;
            }
        }
        return number;
    }

private:
    struct sigaction _interrupt{};
    struct sigaction _quit{};
    sigset_t         _mask{};
    bool             _interrupt_saved{false};
    bool             _quit_saved{false};
    bool             _mask_saved{false};
};

class SpawnResources final {
public:
    SpawnResources() = default;

    ~SpawnResources()
    {
        if (_attributes_ready) {
            static_cast<void>(::posix_spawnattr_destroy(&_attributes));
        }
        if (_actions_ready) {
            static_cast<void>(::posix_spawn_file_actions_destroy(&_actions));
        }
    }

    SpawnResources(SpawnResources const&)                    = delete;
    auto operator=(SpawnResources const&) -> SpawnResources& = delete;

    auto initialize(int terminal, sigset_t const& mask) -> int
    {
        auto number = ::posix_spawn_file_actions_init(&_actions);
        if (number != 0) {
            return number;
        }
        _actions_ready = true;

        // terminal is normalized above stdio, so these actions cannot close a just-installed standard descriptor.
        for (auto target = 0; target < 3; ++target) {
            number = ::posix_spawn_file_actions_adddup2(&_actions, terminal, target);
            if (number != 0) {
                return number;
            }
        }
        number = ::posix_spawn_file_actions_addclose(&_actions, terminal);
        if (number != 0) {
            return number;
        }

        number = ::posix_spawnattr_init(&_attributes);
        if (number != 0) {
            return number;
        }
        _attributes_ready = true;

        sigset_t defaults{};
        static_cast<void>(::sigemptyset(&defaults));
        static_cast<void>(::sigaddset(&defaults, SIGINT));
        static_cast<void>(::sigaddset(&defaults, SIGQUIT));
        number = ::posix_spawnattr_setsigdefault(&_attributes, &defaults);
        if (number != 0) {
            return number;
        }
        number = ::posix_spawnattr_setsigmask(&_attributes, &mask);
        if (number != 0) {
            return number;
        }
        // No SETPGROUP/SETSID: an editor stays in the caller's already-verified foreground group.
        return ::posix_spawnattr_setflags(&_attributes, POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK);
    }

    auto actions() const -> posix_spawn_file_actions_t const* { return &_actions; }

    auto attributes() const -> posix_spawnattr_t const* { return &_attributes; }

private:
    posix_spawn_file_actions_t _actions{};
    posix_spawnattr_t          _attributes{};
    bool                       _actions_ready{false};
    bool                       _attributes_ready{false};
};

auto native_spawn(pid_t*                            pid,
                  std::span<char* const>            arguments,
                  posix_spawn_file_actions_t const* actions,
                  posix_spawnattr_t const*          attributes) -> int
{
#ifdef __APPLE__
    auto* environment = *_NSGetEnviron();
#else
    auto* environment = ::environ;
#endif
    return ::posix_spawnp(pid, arguments.front(), actions, attributes, arguments.data(), environment);
}

auto spawn_and_wait(std::span<char* const> arguments, SpawnResources const& resources, EditorPosixIo const& io)
    -> Completion
{
    pid_t      child{};
    auto const number = io.spawn(&child, arguments, resources.actions(), resources.attributes());
    if (number != 0) {
        // posix_spawnp returns an error number directly; errno may still describe an unrelated operation.
        return system_failure("jobuctl.editor.launch_failed", "Unable to launch the selected editor", number);
    }
    if (child <= 0) {
        return Completion::failure({.category = ErrorCategory::Internal,
                                    .code     = "jobuctl.editor.invalid_child",
                                    .message  = "Editor spawn did not return an owned child PID"});
    }

    auto  status = 0;
    pid_t waited{};
    do {
        waited = io.wait(child, &status);
    } while (waited < 0 && errno == EINTR);
    if (waited < 0) {
        return system_failure("jobuctl.editor.wait_failed", "Unable to wait for the editor child", errno);
    }
    if (waited != child) {
        return Completion::failure({.category = ErrorCategory::Internal,
                                    .code     = "jobuctl.editor.invalid_wait",
                                    .message  = "Editor wait did not return the owned child"});
    }

    if (WIFEXITED(status)) {
        auto const exit_code = WEXITSTATUS(status);
        if (exit_code == 0) {
            return Completion::success();
        }
        return Completion::failure(
            {.category = ErrorCategory::Cancelled,
             .code     = "jobuctl.editor.cancelled",
             .message  = fmt::format("Editor exited with status {}; no update submitted", exit_code)});
    }
    if (WIFSIGNALED(status)) {
        return Completion::failure(
            {.category = ErrorCategory::Cancelled,
             .code     = "jobuctl.editor.interrupted",
             .message  = fmt::format("Editor terminated by signal {}; no update submitted", WTERMSIG(status))});
    }
    return Completion::failure({.category = ErrorCategory::Internal,
                                .code     = "jobuctl.editor.invalid_wait",
                                .message  = "Editor wait returned a nonterminal child status"});
}

} // namespace

auto check_editor_terminal() -> Completion
{
    auto const required = [] {
        return Completion::failure({
            .category = ErrorCategory::InvalidArgument,
            .code     = "jobuctl.edit.terminal_required",
            .message  = "Interactive update requires terminal stdin/stderr and a foreground controlling terminal. "
                        "Use explicit fields with --revision, or job update --request-file FILE.",
        });
    };
    if (::isatty(STDIN_FILENO) != 1 || ::isatty(STDERR_FILENO) != 1) {
        return required();
    }
    auto const terminal = Descriptor{open_foreground_terminal()};
    if (terminal.get() < 0) {
        return required();
    }
    return Completion::success();
}

auto launch_editor(std::span<std::string const> command, std::filesystem::path const& draft, EditorPosixIo io)
    -> Completion
{
    if (command.empty() || command.front().empty() || !draft.is_absolute() ||
        draft.native().find('\0') != std::string::npos) {
        return Completion::failure(
            {.category = ErrorCategory::InvalidArgument,
             .code     = "jobuctl.editor.invalid_arguments",
             .message  = "Editor requires a nonempty executable and an absolute draft path without NUL"});
    }

    // Finish owning argv storage before exposing pointers or changing any process-wide signal state.
    auto owned = std::vector<std::string>{};
    owned.reserve(command.size() + 1);
    for (auto const& argument : command) {
        if (argument.find('\0') != std::string::npos) {
            return Completion::failure({.category = ErrorCategory::InvalidArgument,
                                        .code     = "jobuctl.editor.invalid_arguments",
                                        .message  = "Editor arguments cannot contain NUL"});
        }
        owned.push_back(argument);
    }
    owned.push_back(draft.string());
    auto arguments = std::vector<char*>{};
    arguments.reserve(owned.size() + 1);
    for (auto& argument : owned) {
        arguments.push_back(argument.data());
    }
    arguments.push_back(nullptr);

    if (!io.open_terminal) {
        io.open_terminal = open_foreground_terminal;
    }
    if (!io.spawn) {
        io.spawn = native_spawn;
    }
    if (!io.wait) {
        io.wait = [](pid_t child, int* status) { return ::waitpid(child, status, 0); };
    }

    auto terminal = Descriptor{io.open_terminal()};
    if (terminal.get() < 0) {
        return system_failure("jobuctl.editor.terminal_unavailable",
                              "Unable to open a foreground controlling terminal",
                              errno);
    }
    // Always duplicate above stdio, even if a standard descriptor was closed or an injected open omitted CLOEXEC.
    auto const helper = Descriptor{::fcntl(terminal.get(), F_DUPFD_CLOEXEC, 3)};
    if (helper.get() < 0) {
        return system_failure("jobuctl.editor.terminal_failed",
                              "Unable to prepare the editor terminal descriptor",
                              errno);
    }
    terminal.close();

    auto signals   = ParentSignals{};
    auto installed = signals.install();
    if (!installed) {
        return installed;
    }
    auto       resources = SpawnResources{};
    auto const number    = resources.initialize(helper.get(), signals.child_mask());
    auto outcome = number == 0
                     ? spawn_and_wait(arguments, resources, io)
                     : system_failure("jobuctl.editor.setup_failed", "Unable to prepare editor spawn actions", number);

    auto const restored = signals.restore();
    if (restored != 0) {
        return system_failure("jobuctl.editor.signal_restore_failed",
                              "Unable to restore editor parent signal policy",
                              restored);
    }
    return outcome;
}

} // namespace jb::jobuctl::detail
