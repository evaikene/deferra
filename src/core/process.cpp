#include "process.hpp"

#include "event_loop.hpp"
#include "process_priv.hpp"
#include "process_request_priv.hpp"

#include <unistd.h>

#include <memory>
#include <type_traits>
#include <utility>

#if defined(__linux__)
#  include <cassert>
#  include <cerrno>
#  include <exception>
#  include <pthread.h>
#  include <sys/wait.h>
#endif

namespace jb::core {

Process::Process(Object* parent)
    : Process(std::make_unique<Private>(), parent)
{}

Process::Process(std::unique_ptr<Private> data, Object* parent)
    : Object(*data, parent)
{
    // The parameter owns the block if base construction throws; all Signal members construct without throwing.
    // Once construction reaches this body, Object is the sole owner and no throwing work remains.
    static_assert(std::is_nothrow_default_constructible_v<Signal<>>);
    static_assert(std::is_nothrow_default_constructible_v<Signal<ByteBuffer>>);
    static_assert(std::is_nothrow_default_constructible_v<Signal<ProcessExit>>);
    // Bind only after Object owns the block and has established parenting and affinity.
    data.release()->owner = this;
}

Process::~Process()
{
#if defined(__linux__)
    d_ptr<Private>()->cleanup();
#endif
}

auto Process::start(ProcessStartInfo start_info) -> Result<void, Error>
{
    if (d_ptr<Private>()->state != ProcessState::NotRunning) {
        return Result<void, Error>::failure({.category = ErrorCategory::Conflict,
                                             .code     = "core.process.invalid_state",
                                             .message  = "Process is not idle"});
    }
    auto* loop = event_loop();
    if (!loop || loop != EventLoop::current() || !loop->is_valid()) {
        return Result<void, Error>::failure({.category = ErrorCategory::Unavailable,
                                             .code     = "core.process.event_loop_unavailable",
                                             .message  = "Process requires a valid current owner EventLoop"});
    }
    // Capture once before preparation; future launch stages retain this exact checked absolute deadline.
    auto const launch_time = Clock::now();
    auto       prepared    = priv::prepare_process_request(std::move(start_info), launch_time, sysconf(_SC_ARG_MAX));
    if (!prepared) {
        return Result<void, Error>::failure(prepared.error());
    }
    auto signal_configuration = priv::validate_process_signal_configuration();
    if (!signal_configuration) {
        return signal_configuration;
    }
#if defined(__linux__)
    auto* data = d_ptr<Private>();
    // These later-stage features must not be silently accepted without enforcement.
    if (prepared.value()->deadline()) {
        return Result<void, Error>::failure(priv::process_error("core.process.monitor_unsupported",
                                                                ErrorCategory::Unsupported,
                                                                "timeout.not_implemented"));
    }
    if (prepared.value()->prevent_privilege_gain()) {
        return Result<void, Error>::failure(priv::process_error("core.process.security_unsupported",
                                                                ErrorCategory::Unsupported,
                                                                "hardening.not_implemented"));
    }
    data->request = std::move(prepared).value();
    return data->launch();
#else
    return Result<void, Error>::failure({.category = ErrorCategory::Unsupported,
                                         .code     = "core.process.monitor_unsupported",
                                         .message  = "Process execution backend is unavailable",
                                         .detail   = "backend.not_implemented"});
#endif
}

auto Process::stop(ProcessStopReason /*reason*/) -> Result<void, Error>
{
#if defined(__linux__)
    if (d_ptr<Private>()->state != ProcessState::NotRunning) {
        return Result<void, Error>::failure(priv::process_error("core.process.monitor_unsupported",
                                                                ErrorCategory::Unsupported,
                                                                "stop.not_implemented"));
    }
#endif
    return Result<void, Error>::failure({.category = ErrorCategory::Conflict,
                                         .code     = "core.process.invalid_state",
                                         .message  = "Process has no accepted operation"});
}

auto Process::state() const noexcept -> ProcessState
{
    return d_ptr<Private const>()->state;
}

auto Process::process_id() const noexcept -> std::optional<std::int64_t>
{
#if defined(__linux__)
    auto const pid = d_ptr<Private const>()->pid;
    if (pid > 0) {
        return pid;
    }
#endif
    return std::nullopt;
}

#if defined(__linux__)
auto Process::Private::launch() -> Result<void, Error>
{
    // Roll back rejected and exceptional setup alike, without catching or translating allocation failures.
    // The child remains gated until the last fallible setup operation has succeeded.
    struct LaunchGuard {
        Private& data;
        bool     accepted{false};

        ~LaunchGuard()
        {
            if (!accepted) {
                data.cleanup();
            }
        }
    } guard{.data = *this};

    auto reject      = [](Error error) { return Result<void, Error>::failure(std::move(error)); };
    auto plan_result = priv::prepare_process_child(*request, operations->child_options());
    if (!plan_result) {
        return reject(plan_result.error());
    }
    auto const& plan  = plan_result.value();
    auto        setup = priv::prepare_process_descriptors(descriptors, *operations);
    if (!setup) {
        return reject(setup.error());
    }
    // Allocate all readiness anchors before creation. Callbacks borrow only their own one-way lifetime token.
    ++generation;
    status_anchor  = std::make_shared<Anchor>(Anchor{.data = this, .generation = generation});
    process_anchor = std::make_shared<Anchor>(Anchor{.data = this, .generation = generation});
    sigset_t   original_mask{};
    auto const mask_error = ::pthread_sigmask(SIG_SETMASK, &plan.blocked, &original_mask);
    if (mask_error != 0) {
        return reject(priv::process_error("core.process.child_setup_failed",
                                          ErrorCategory::Io,
                                          "parent.signal_mask",
                                          mask_error));
    }
    pid                   = operations->create_child();
    auto const fork_error = errno;
    if (pid == 0) {
        priv::execute_process_child(descriptors, plan);
    }
    // Restore immediately on both parent paths. A failed restoration of this valid saved mask is fatal:
    // continuing with every signal blocked would corrupt the embedding thread's signal contract.
    if (::pthread_sigmask(SIG_SETMASK, &original_mask, nullptr) != 0) {
        std::terminate();
    }
    if (pid < 0) {
        return reject(priv::process_error("core.process.fork_failed",
                                          ErrorCategory::ResourceExhausted,
                                          "parent.fork",
                                          fork_error));
    }
    priv::close_process_fd(descriptors.input);
    priv::close_process_fd(descriptors.output);
    priv::close_process_fd(descriptors.status_write);
    priv::close_process_fd(descriptors.gate_child);
    if (operations->establish_group(pid) != 0) {
        return reject(priv::process_error("core.process.child_setup_failed", ErrorCategory::Io, "parent.group", errno));
    }
    group_established = true;
    // These private dispatch boundaries must not unwind past an accepted one-shot event: doing so could
    // lose reaping/completion permanently. Allocation and slot exceptions are fatal here, not API contracts.
    status_watch =
        event_loop->watch_fd(descriptors.status_read,
                             FdEvent::Read,
                             FdTriggerMode::Edge,
                             [weak = std::weak_ptr<Anchor>{status_anchor}](int, FdEvents) noexcept {
                                 auto anchor = weak.lock();
                                 if (!anchor || !anchor->data || anchor->generation != anchor->data->generation) {
                                     return;
                                 }
                                 // No capture is accessed after this call: it may remove the currently executing fd
                                 // callable.
                                 auto* data = anchor->data;
                                 data->read_status();
                                 data->finish_if_ready();
                             });
    if (!status_watch) {
        return reject(
            priv::process_error("core.process.watch_failed", ErrorCategory::Unavailable, "watch.exec_status"));
    }
    auto watched = event_loop->watch_process(pid, [weak = std::weak_ptr<Anchor>{process_anchor}]() noexcept {
        auto anchor = weak.lock();
        if (anchor && anchor->data && anchor->generation == anchor->data->generation) {
            anchor->data->child_ready();
        }
    });
    if (!watched) {
        return reject(watched.error());
    }
    process_watched = true;
    state           = ProcessState::Starting;
    ssize_t sent;
    do {
        sent = operations->release_gate(descriptors.gate_parent, pid);
    } while (sent < 0 && errno == EINTR);
    if (sent != 1) {
        return reject(priv::process_error("core.process.child_setup_failed",
                                          ErrorCategory::Io,
                                          "parent.gate",
                                          sent < 0 ? errno : EIO));
    }
    // The gate is the acceptance commit point: the target cannot execute before both watches exist.
    // No signal is emitted here, even when the child has already exited before start() returns.
    gate_released = true;
    priv::close_process_fd(descriptors.gate_parent);
    guard.accepted = true;
    return Result<void, Error>::success();
}

void Process::Private::retire_status()
{
    // Native removal can fail and retain a callable. Invalidate before removal or descriptor reuse.
    if (status_anchor) {
        status_anchor->data = nullptr;
        status_anchor.reset();
    }
    if (status_watch) {
        static_cast<void>(event_loop->unwatch_fd(status_watch));
        status_watch = {};
    }
    priv::close_process_fd(descriptors.status_read);
}

void Process::Private::retire_process()
{
    if (process_anchor) {
        process_anchor->data = nullptr;
        process_anchor.reset();
    }
    if (process_watched) {
        static_cast<void>(event_loop->unwatch_process(pid));
        process_watched = false;
    }
}

void Process::Private::read_status()
{
    if (status_resolved) {
        return;
    }
    auto* bytes = reinterpret_cast<char*>(&child_error);
    while (status_bytes < sizeof(child_error)) {
        auto const count = ::read(descriptors.status_read, bytes + status_bytes, sizeof(child_error) - status_bytes);
        if (count > 0) {
            status_bytes += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return;
        }
        status_resolved = true;
        if (count < 0 || status_bytes != 0) {
            exit.start_error = priv::process_error("core.process.child_setup_failed",
                                                   ErrorCategory::Io,
                                                   "parent.exec_status",
                                                   count < 0 ? errno : EIO);
        }
        retire_status();
        if (!exit.start_error && gate_released) {
            if (state == ProcessState::Starting) {
                state = ProcessState::Running;
            }
            // Clean close-on-exec EOF is only an observation: pre-exec signal death can also cause it.
            owner->emit(owner->started);
        }
        return;
    }
    status_resolved  = true;
    exit.start_error = priv::process_child_error(child_error);
    retire_status();
}

void Process::Private::child_ready()
{
    // Resolve even a coalesced immediate exit's launch channel before any finished signal.
    read_status();
    int   status{};
    pid_t waited;
    do {
        waited = ::waitpid(pid, &status, WNOHANG);
    } while (waited < 0 && errno == EINTR);
    if (waited == 0) {
        return;
    }
    if (waited != pid) {
        // Outside code reaping our child violates the public exclusive-reaping contract.
        std::terminate();
    }
    retire_process();
    pid               = -1;
    group_established = false;
    reaped            = true;
    state             = ProcessState::Finishing;
    if (WIFEXITED(status)) {
        exit.kind      = ProcessExitKind::Exited;
        exit.exit_code = WEXITSTATUS(status);
    }
    else {
        exit.kind          = ProcessExitKind::Signaled;
        exit.signal_number = WTERMSIG(status);
    }
    finish_if_ready();
}

void Process::Private::finish_if_ready()
{
    if (!reaped || !status_resolved) {
        return;
    }
    if (exit.start_error) {
        exit.kind = ProcessExitKind::StartFailed;
        exit.exit_code.reset();
        exit.signal_number.reset();
    }
    auto result = std::move(exit);
    // Stage 6.3 has no output readers. Retire everything before a direct finished slot can restart.
    cleanup();
    owner->emit(owner->finished, result);
}

void Process::Private::cleanup() noexcept
{
    retire_status();
    retire_process();
    priv::close_process_fd(descriptors.gate_parent);
    if (pid > 0) {
        // Rejection/deferred destruction must not leak the direct child. Full descendant/grace policy is Stage 6.5.
        if (group_established) {
            ::kill(-pid, SIGKILL);
        }
        ::kill(pid, SIGKILL);
        pid_t waited;
        do {
            waited = ::waitpid(pid, nullptr, 0);
        } while (waited < 0 && errno == EINTR);
        pid = -1;
    }
    descriptors.close_all();
    group_established = false;
    request.reset();
    status_resolved = false;
    gate_released   = false;
    reaped          = false;
    child_error     = {};
    status_bytes    = 0;
    exit            = {};
    state           = ProcessState::NotRunning;
}

void priv::ProcessTestAccess::set_operations(Process& process, std::shared_ptr<ProcessOperations> operations) noexcept
{
    assert(process.state() == ProcessState::NotRunning && operations);
    process.d_ptr<Process::Private>()->operations = std::move(operations);
}
#endif

} // namespace jb::core
