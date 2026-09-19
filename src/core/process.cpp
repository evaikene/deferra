#include "process.hpp"

#include "event_loop.hpp"
#include "logging.hpp"
#include "process_priv.hpp"
#include "process_request_priv.hpp"

#include <unistd.h>

#include <memory>
#include <type_traits>
#include <utility>

#if defined(__linux__) || defined(__APPLE__)
#  include <algorithm>
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
#if defined(__linux__) || defined(__APPLE__)
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
#if defined(__linux__) || defined(__APPLE__)
    auto*      data        = d_ptr<Private>();
    // Capture once before preparation; every later timeout decision uses this exact checked absolute deadline.
    auto const launch_time = data->operations->monotonic_now();
#else
    auto const launch_time = Clock::now();
#endif
    auto prepared = priv::prepare_process_request(std::move(start_info), launch_time, sysconf(_SC_ARG_MAX));
    if (!prepared) {
        return Result<void, Error>::failure(prepared.error());
    }
    auto signal_configuration = priv::validate_process_signal_configuration();
    if (!signal_configuration) {
        return signal_configuration;
    }
#if defined(__linux__) || defined(__APPLE__)
    data->request = std::move(prepared).value();
    return data->launch();
#else
    return Result<void, Error>::failure({.category = ErrorCategory::Unsupported,
                                         .code     = "core.process.monitor_unsupported",
                                         .message  = "Process execution backend is unavailable",
                                         .detail   = "backend.not_implemented"});
#endif
}

auto Process::stop(ProcessStopReason reason) -> Result<void, Error>
{
#if defined(__linux__) || defined(__APPLE__)
    return d_ptr<Private>()->stop(reason);
#else
    static_cast<void>(reason);
    return Result<void, Error>::failure({.category = ErrorCategory::Conflict,
                                         .code     = "core.process.invalid_state",
                                         .message  = "Process has no accepted operation"});
#endif
}

auto Process::state() const noexcept -> ProcessState
{
    return d_ptr<Private const>()->state;
}

auto Process::process_id() const noexcept -> std::optional<std::int64_t>
{
#if defined(__linux__) || defined(__APPLE__)
    auto const pid = d_ptr<Private const>()->pid;
    if (pid > 0) {
        return pid;
    }
#endif
    return std::nullopt;
}

#if defined(__linux__) || defined(__APPLE__)
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
    auto plan_result = priv::prepare_process_child(*request, *operations);
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
    for (auto& channel : channels) {
        channel.anchor   = std::make_shared<Anchor>(Anchor{.data = this, .generation = generation});
        channel.terminal = false;
    }
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
    for (auto& fd : descriptors.output_write) {
        priv::close_process_fd(fd);
    }
    priv::close_process_fd(descriptors.status_write);
    priv::close_process_fd(descriptors.gate_child);
    if (operations->establish_group(pid) != 0) {
        return reject(priv::process_error("core.process.child_setup_failed", ErrorCategory::Io, "parent.group", errno));
    }
    process_group = pid;
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
                                 data->drain_output(0);
                                 data->drain_output(1);
                                 data->finish_if_ready();
                             });
    if (!status_watch) {
        return reject(
            priv::process_error("core.process.watch_failed", ErrorCategory::Unavailable, "watch.exec_status"));
    }
    for (std::size_t i = 0; i < channels.size(); ++i) {
        auto& channel = channels[i];
        channel.watch =
            event_loop->watch_fd(descriptors.output_read[i],
                                 FdEvent::Read,
                                 FdTriggerMode::Edge,
                                 [weak = std::weak_ptr<Anchor>{channel.anchor}, i](int, FdEvents) noexcept {
                                     auto anchor = weak.lock();
                                     if (anchor && anchor->data && anchor->generation == anchor->data->generation) {
                                         anchor->data->output_ready(i);
                                     }
                                 });
        if (!channel.watch) {
            return reject(priv::process_error("core.process.watch_failed",
                                              ErrorCategory::Unavailable,
                                              i == 0 ? "watch.stdout" : "watch.stderr"));
        }
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

    if (auto const deadline = request->deadline()) {
        auto const accepted_generation = generation;
        timeout_timer                  = event_loop->post_at(*deadline, [this, accepted_generation]() noexcept {
            if (generation == accepted_generation) {
                timeout_expired();
            }
        });
        if (!timeout_timer) {
            return reject(
                priv::process_error("core.process.watch_failed", ErrorCategory::Unavailable, "timer.timeout"));
        }

        // Ownership and every watch are committed, so expiry here is an accepted asynchronous timeout. Keeping the
        // gate closed proves the target never executed and makes termination grace unnecessary.
        if (operations->monotonic_now() >= *deadline) {
            cancel_timer(timeout_timer);
            stop_kind = ProcessExitKind::TimedOut;
            state     = ProcessState::Stopping;
            priv::close_process_fd(descriptors.gate_parent);
            attempt_group_kill("timeout.pre_gate_kill");
            guard.accepted = true;
            return Result<void, Error>::success();
        }
    }

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
    // The gate is the acceptance commit point: the target cannot execute before all four watches exist.
    // No signal is emitted here, even when the child has already exited before start() returns.
    gate_released = true;
    priv::close_process_fd(descriptors.gate_parent);
    guard.accepted = true;
    return Result<void, Error>::success();
}

auto Process::Private::stop(ProcessStopReason reason) -> Result<void, Error>
{
    auto const requested_kind =
        reason == ProcessStopReason::Cancelled ? ProcessExitKind::Cancelled : ProcessExitKind::Interrupted;

    if (state == ProcessState::Stopping || state == ProcessState::Finishing) {
        if (stop_kind == requested_kind) {
            return Result<void, Error>::success();
        }
        if (stop_kind) {
            return Result<void, Error>::failure({.category = ErrorCategory::Conflict,
                                                 .code     = "core.process.stop_conflict",
                                                 .message  = "A different Process stop cause is already active"});
        }
    }
    if (state != ProcessState::Starting && state != ProcessState::Running) {
        return Result<void, Error>::failure({.category = ErrorCategory::Conflict,
                                             .code     = "core.process.invalid_state",
                                             .message  = "Process cannot be stopped in its current state"});
    }

    auto const delivered = operations->signal_group(process_group, SIGTERM);
    auto const error     = errno;
    if (delivered != 0 && error != ESRCH) {
        return Result<void, Error>::failure(
            priv::process_error("core.process.signal_failed", ErrorCategory::Io, "signal.term", error));
    }

    // Commit the semantic result only after the caller's initial TERM request is accepted. A failed explicit
    // delivery leaves the operation unchanged and retryable.
    begin_stopping(requested_kind);
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

void Process::Private::invalidate_output_work(std::size_t index)
{
    auto& channel = channels[index];
    // Removal may retain callbacks, and posted continuations may outlive this run. Neither may regain access after
    // forced final draining, reset, restart, or destruction.
    if (channel.anchor) {
        channel.anchor->data = nullptr;
        channel.anchor.reset();
    }
    channel.continuation_pending = false;
    if (channel.watch) {
        static_cast<void>(event_loop->unwatch_fd(channel.watch));
        channel.watch = {};
    }
}

void Process::Private::retire_output(std::size_t index)
{
    auto& channel = channels[index];
    invalidate_output_work(index);
    channel.final_drain_pending = false;
    channel.terminal            = true;
    priv::close_process_fd(descriptors.output_read[index]);
}

void Process::Private::output_ready(std::size_t index)
{
    drain_output(index);
    finish_if_ready();
}

void Process::Private::drain_output(std::size_t index, bool final_drain)
{
    auto& channel = channels[index];
    if (channel.terminal || (!final_drain && channel.continuation_pending)) {
        return;
    }
    if (channel.draining) {
        if (final_drain) {
            // A direct output slot may run the deadline reentrantly. Retire all future entry points immediately;
            // the active callback will use only the remainder of its existing budget, then force this terminal.
            invalidate_output_work(index);
            channel.final_drain_pending = true;
        }
        return;
    }

    if (final_drain) {
        // The deadline retires native and queued entry points before the last direct read. A queued continuation may
        // remain in EventLoop storage, but its expired weak anchor makes it permanently inert.
        invalidate_output_work(index);
        channel.final_drain_pending = false;
    }

    channel.draining = true;
    // A coalesced output event can precede launch-channel dispatch. Resolve it before exposing any target bytes.
    read_status();
    if (!status_resolved) {
        channel.draining = false;
        return;
    }
    ByteBuffer  chunk(kPipeReadChunkBytes);
    std::size_t total{0};
    while (total < kPipeReadBudgetBytes) {
        chunk.resize(std::min(kPipeReadChunkBytes, kPipeReadBudgetBytes - total));
        auto const count = operations->read_output(descriptors.output_read[index], chunk.data(), chunk.size());
        if (count > 0) {
            auto const size  = static_cast<std::size_t>(count);
            total           += size;
            chunk.resize(size);
            owner->emit(index == 0 ? owner->standard_output : owner->standard_error, chunk);
            // Direct slots may request delete_later(); the Object remains alive until this bounded drain returns.
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        }
        if (count < 0) {
            (index == 0 ? exit.stdout_lost : exit.stderr_lost) = true;
        }
        retire_output(index);
        break;
    }
    channel.draining            = false;
    auto const force_terminal   = final_drain || channel.final_drain_pending;
    channel.final_drain_pending = false;
    if (force_terminal && !channel.terminal) {
        // Reaching the hard budget did not prove EOF. Close without an extra read so the final callback remains
        // strictly bounded, and preserve every byte already delivered within that budget.
        (index == 0 ? exit.stdout_lost : exit.stderr_lost) = true;
        retire_output(index);
    }
    else if (!channel.terminal && total == kPipeReadBudgetBytes) {
        // Edge readiness need not recur while unread bytes remain. Coalesce with native callbacks until delivery;
        // each continuation consumes only one budget and queues any further work for a later Object event cycle.
        channel.continuation_pending = true;
        auto const posted            = event_loop->post_event_delivery(
            owner,
            lifetime,
            [weak = std::weak_ptr<Anchor>{channel.anchor}, index]() noexcept {
                auto anchor = weak.lock();
                if (!anchor || !anchor->data || anchor->generation != anchor->data->generation) {
                    return;
                }
                auto* data                                 = anchor->data;
                data->channels[index].continuation_pending = false;
                data->output_ready(index);
            });
        if (!posted) {
            // Receiver-bound delivery preserves Object lifetime safety while reporting enqueue failure.
            // Merely clearing pending cannot restore a missing edge; explicitly terminate only this capture.
            (index == 0 ? exit.stdout_lost : exit.stderr_lost) = true;
            retire_output(index);
        }
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

void Process::Private::cancel_timer(TimerHandle& timer) noexcept
{
    if (timer) {
        event_loop->cancel_timer(timer);
        timer = {};
    }
}

void Process::Private::attempt_group_kill(char const* stage) noexcept
{
    if (group_kill_attempted || process_group <= 0) {
        return;
    }

    group_kill_attempted = true;
    auto const delivered = operations->signal_group(process_group, SIGKILL);
    auto const error     = errno;
    if (delivered != 0 && error != ESRCH) {
        // Accepted lifecycle work has no synchronous caller to fail. Keep waiting for the owned leader and log only
        // the fixed operation stage plus the native code; request data must never enter diagnostics.
        log_error("Process {} failed with native error {}", stage, error);
    }
}

void Process::Private::begin_stopping(ProcessExitKind kind)
{
    stop_kind = kind;
    state     = ProcessState::Stopping;
    cancel_timer(timeout_timer);

    auto const grace = request->termination_grace();
    if (grace == Duration::zero()) {
        attempt_group_kill("termination.kill");
        return;
    }

    auto const now                 = operations->monotonic_now();
    // The validated grace is small, but keep synthetic test clocks and unusual steady-clock epochs overflow-safe.
    termination_deadline           = now > TimePoint::max() - grace ? TimePoint::max() : now + grace;
    auto const accepted_generation = generation;
    termination_timer              = event_loop->post_at(termination_deadline, [this, accepted_generation]() noexcept {
        if (generation == accepted_generation) {
            termination_grace_expired();
        }
    });
    if (!termination_timer) {
        log_error("Process termination timer could not be armed; escalating immediately");
        attempt_group_kill("termination.timer_unavailable");
    }
}

void Process::Private::timeout_expired()
{
    timeout_timer = {};
    if (state != ProcessState::Starting && state != ProcessState::Running) {
        return;
    }

    auto const delivered = operations->signal_group(process_group, SIGTERM);
    auto const error     = errno;
    if (delivered != 0 && error != ESRCH) {
        // Automatic timeout cannot return an operational error. Preserve TimedOut and continue to KILL escalation.
        log_error("Process timeout TERM failed with native error {}", error);
    }
    begin_stopping(ProcessExitKind::TimedOut);
}

void Process::Private::termination_grace_expired()
{
    termination_timer = {};
    if (state != ProcessState::Stopping || group_kill_attempted) {
        return;
    }

    attempt_group_kill("termination.kill");
    if (leader_exit_observed) {
        // Process readiness already proved that this wait cannot block. Retaining the leader until now protected the
        // numeric process-group identity while descendants received their full TERM grace.
        reap_child(0);
    }
}

void Process::Private::post_reap_drain_expired()
{
    post_reap_timer = {};
    if (state != ProcessState::Finishing || !reaped) {
        return;
    }

    for (std::size_t i = 0; i < channels.size(); ++i) {
        if (!channels[i].terminal) {
            drain_output(i, true);
        }
    }
    finish_if_ready();
}

void Process::Private::reap_child(int options)
{
    int   status{};
    pid_t waited;
    do {
        waited = operations->wait_process(pid, &status, options);
    } while (waited < 0 && errno == EINTR);
    if (waited == 0) {
        return;
    }
    if (waited != pid) {
        // Outside code reaping our child violates the public exclusive-reaping contract.
        std::terminate();
    }
    enter_finishing(status);
}

void Process::Private::enter_finishing(int status)
{
    retire_process();
    cancel_timer(timeout_timer);
    cancel_timer(termination_timer);

    // Invalidate both numeric identities before output can invoke user code. From Finishing onward, no path may
    // signal the former process group even when a direct output slot re-enters stop() or requests destruction.
    pid                  = -1;
    process_group        = -1;
    reaped               = true;
    leader_exit_observed = false;
    state                = ProcessState::Finishing;
    exit.exit_code.reset();
    exit.signal_number.reset();
    if (WIFEXITED(status)) {
        exit.exit_code = WEXITSTATUS(status);
    }
    else {
        exit.signal_number = WTERMSIG(status);
    }

    drain_output(0);
    drain_output(1);
    if (!channels[0].terminal || !channels[1].terminal) {
        auto const now = operations->monotonic_now();
        auto const deadline =
            now > TimePoint::max() - kPostReapDrainTimeout ? TimePoint::max() : now + kPostReapDrainTimeout;
        auto const accepted_generation = generation;
        post_reap_timer                = event_loop->post_at(deadline, [this, accepted_generation]() noexcept {
            if (generation == accepted_generation) {
                post_reap_drain_expired();
            }
        });
        if (!post_reap_timer) {
            log_error("Process post-reap output timer could not be armed; applying the bounded final drain now");
            post_reap_drain_expired();
            return;
        }
    }
    finish_if_ready();
}

void Process::Private::child_ready()
{
    // Resolve even a coalesced immediate exit's launch channel before any finished signal.
    read_status();

    if (state == ProcessState::Stopping && termination_timer && !group_kill_attempted &&
        operations->monotonic_now() < termination_deadline) {
        // Preserve the exited leader as an owned zombie. This prevents PID/PGID reuse and lets same-group
        // descendants use every remaining instant of the configured TERM grace.
        leader_exit_observed = true;
        retire_process();
        drain_output(0);
        drain_output(1);
        return;
    }

    if (termination_timer) {
        cancel_timer(termination_timer);
    }
    // Natural exit and expired/zero grace both clean the complete group before reaping the leader. The unreaped
    // leader keeps the PGID protected until after this attempt.
    attempt_group_kill("leader_exit.kill");
    reap_child(WNOHANG);
}

void Process::Private::finish_if_ready()
{
    if (!reaped || !status_resolved || !channels[0].terminal || !channels[1].terminal) {
        return;
    }

    if (stop_kind) {
        // The first accepted cancellation/timeout cause remains semantic even when TERM produces an ordinary exit or
        // races with a child-side setup failure. The observed wait status remains as bounded diagnostic metadata.
        exit.kind = *stop_kind;
        exit.start_error.reset();
    }
    else if (exit.start_error) {
        exit.kind = ProcessExitKind::StartFailed;
        exit.exit_code.reset();
        exit.signal_number.reset();
    }
    else {
        exit.kind = exit.exit_code ? ProcessExitKind::Exited : ProcessExitKind::Signaled;
    }
    auto result = std::move(exit);
    // Reaping alone cannot discard tail bytes. Both EOF/loss terminals are established; retire before restart.
    cleanup();
    owner->emit(owner->finished, result);
}

void Process::Private::cleanup() noexcept
{
    // No native removal, timer cancellation, descriptor close, or signal attempt may leave a callable path back into
    // this run. Establish that invariant for every registration before beginning teardown.
    if (status_anchor) {
        status_anchor->data = nullptr;
        status_anchor.reset();
    }
    if (process_anchor) {
        process_anchor->data = nullptr;
        process_anchor.reset();
    }
    for (auto& channel : channels) {
        if (channel.anchor) {
            channel.anchor->data = nullptr;
            channel.anchor.reset();
        }
        channel.continuation_pending = false;
        channel.final_drain_pending  = false;
    }

    cancel_timer(timeout_timer);
    cancel_timer(termination_timer);
    cancel_timer(post_reap_timer);

    if (status_watch) {
        static_cast<void>(event_loop->unwatch_fd(status_watch));
        status_watch = {};
    }
    if (process_watched) {
        static_cast<void>(event_loop->unwatch_process(pid));
        process_watched = false;
    }
    for (auto& channel : channels) {
        if (channel.watch) {
            static_cast<void>(event_loop->unwatch_fd(channel.watch));
            channel.watch = {};
        }
    }

    priv::close_process_fd(descriptors.gate_parent);
    if (pid > 0) {
        // Rejection and destruction are fail-safe cleanup, not normal completion. Signal only while the unreaped
        // identity is still owned; Finishing has already cleared both identifiers.
        if (process_group > 0) {
            static_cast<void>(operations->signal_group(process_group, SIGKILL));
        }
        static_cast<void>(operations->signal_process(pid, SIGKILL));
    }

    priv::close_process_fd(descriptors.status_read);
    for (std::size_t i = 0; i < channels.size(); ++i) {
        channels[i].terminal = true;
        priv::close_process_fd(descriptors.output_read[i]);
    }

    if (pid > 0) {
        pid_t waited;
        do {
            waited = operations->wait_process(pid, nullptr, 0);
        } while (waited < 0 && errno == EINTR);
        pid = -1;
    }
    descriptors.close_all();
    process_group = -1;
    request.reset();
    status_resolved      = false;
    gate_released        = false;
    reaped               = false;
    leader_exit_observed = false;
    group_kill_attempted = false;
    stop_kind.reset();
    termination_deadline = {};
    child_error          = {};
    status_bytes         = 0;
    exit                 = {};
    state                = ProcessState::NotRunning;
}

void priv::ProcessTestAccess::set_operations(Process& process, std::shared_ptr<ProcessOperations> operations) noexcept
{
    assert(process.state() == ProcessState::NotRunning && operations);
    process.d_ptr<Process::Private>()->operations = std::move(operations);
}
#endif

} // namespace jb::core
