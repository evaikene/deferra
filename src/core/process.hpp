/** @file process.hpp
 * @brief Defines asynchronous local process execution with explicit arguments and environment.
 */
#pragma once

#include "byte_buffer.hpp"
#include "error.hpp"
#include "event_loop_types.hpp"
#include "object.hpp"
#include "result.hpp"
#include "signal.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace jb::core {

/// Complete target environment; no ambient variables are inherited. Names use ASCII identifier syntax.
using ProcessEnvironment = std::map<std::string, std::string, std::less<>>;

/// Lifecycle of one accepted launch, observed only on the owner thread.
enum class ProcessState : std::uint8_t {
    NotRunning, ///< Idle; a new start may be attempted.
    Starting,   ///< Accepted, awaiting resolution of the gated exec-status channel.
    Running,    ///< Launch channel resolved cleanly; no stop cause has been accepted.
    Stopping,   ///< A terminal stop cause is fixed; group termination and reaping remain pending.
    Finishing,  ///< Direct child reaped and PID/PGID invalidated; output terminals are pending.
};

/// Semantic completion cause, independent of output capture loss.
enum class ProcessExitKind : std::uint8_t {
    Exited,      ///< Normal numeric exit; only exit_code is present.
    Signaled,    ///< Unrequested signal death; only signal_number is present.
    TimedOut,    ///< Launch deadline expired; exactly one observed code or signal is present.
    Cancelled,   ///< Explicit cancellation; exactly one observed code or signal is present.
    Interrupted, ///< Explicit interruption; exactly one observed code or signal is present.
    StartFailed, ///< Proven child setup/exec failure; only start_error is present.
};

/// Explicit terminal cause requested by stop(); the first accepted cause wins.
enum class ProcessStopReason : std::uint8_t {
    Cancelled,   ///< The caller cancelled the accepted operation.
    Interrupted, ///< The caller explicitly interrupted the operation.
};

/// Owning launch request. Strings are literal, NUL-free, and never parsed as shell syntax.
struct ProcessStartInfo {
    /// Nonempty absolute path or bare name without '/'; at most 4096 bytes. Bare names require explicit PATH.
    std::string              executable;
    /// At most 1024 arguments after argv[0]; empty strings and leading dashes are preserved.
    std::vector<std::string> arguments;
    /// Exact target environment. Bare-name PATH has at most 256 nonempty absolute colon-separated entries.
    ProcessEnvironment       environment;
    /// Absolute directory, at most 4096 bytes; child chdir() authoritatively checks access/existence.
    std::filesystem::path    working_directory{"/"};
    /// Positive monotonic duration, if set. Includes preparation; its checked launch deadline must be representable.
    std::optional<Duration>  timeout;
    /// Complete process-group TERM grace before KILL, from zero through five minutes.
    Duration                 termination_grace{std::chrono::seconds{5}};
    /// Immutable policy requiring a non-root effective identity, authoritatively checked immediately before exec.
    bool                     require_non_root{false};
    /// Require strict privilege-gain prevention; unsupported platforms reject rather than weaken this policy.
    bool                     prevent_privilege_gain{false};
};

/// Owning completion metadata; contains no command, path, argument, environment, or output bytes.
/// A stop-kind preserves its semantic cause even when the target handles TERM and exits normally.
struct ProcessExit {
    /// Classification determining which optional fields are valid; the default alone is not a complete result.
    ProcessExitKind      kind{ProcessExitKind::StartFailed};
    /// Present for Exited, or a stop-kind whose leader exited normally; mutually exclusive with signal_number.
    std::optional<int>   exit_code;
    /// Present for Signaled, or a stop-kind whose leader died by signal; mutually exclusive with exit_code.
    std::optional<int>   signal_number;
    /// Present only for StartFailed; a safe core.process.* error proving child setup/exec failure.
    std::optional<Error> start_error;
    /// stdout ended through read failure or bounded post-reap forced closure rather than EOF.
    bool                 stdout_lost{false};
    /// stderr ended through read failure or bounded post-reap forced closure rather than EOF.
    bool                 stderr_lost{false};
};

/// Reusable asynchronous process-group owner with separate, binary-safe, unbuffered output signals.
/// Construct, use, and destroy on one Object/EventLoop owner thread; active processes must not change affinity.
/// A non-null parent owns this object, which must then be heap-allocated. Private state extends Object's one block.
/// The host must retain waitable SIGCHLD disposition (neither SIG_IGN nor SA_NOCLDWAIT), and outside code must
/// never reap a Process-owned PID. The target receives explicit argv/environment, stdin EOF, and its own group.
/// argv/environment storage including NULs and pointer arrays is capped at 256 KiB and the runtime argument limit;
/// expanded PATH candidate storage has a separate 256 KiB limit. No shell or ambient PATH lookup is performed.
/// All errors use core.process.* codes and fixed safe details, excluding user-supplied strings and output.
/// @note Stage 6.1 implements request preparation and idle behavior only. Valid launches currently reject with
/// core.process.monitor_unsupported, without creating native resources or emitting Process signals.
class Process final : public Object {
public:
    /// Constructs an idle Process and transfers ownership to @p parent when non-null.
    /// @param parent Optional same-thread Object parent; no launch or signal occurs during construction.
    explicit Process(Object* parent = nullptr);
    /// Suppresses Process lifecycle/output signals, invalidates readiness, and immediately kills/reaps owned work.
    /// Never rewrites durable state or signals a former PID/PGID after reaping. Object destruction rules still apply.
    ~Process() override;

    /// Process ownership and affinity cannot be copied.
    Process(Process const&)                    = delete;
    /// Process ownership and affinity cannot be moved.
    Process(Process&&)                         = delete;
    /// Process ownership and affinity cannot be copy-assigned.
    auto operator=(Process const&) -> Process& = delete;
    /// Process ownership and affinity cannot be move-assigned.
    auto operator=(Process&&) -> Process&      = delete;

    /// Validates and attempts to accept one launch on the current valid owner EventLoop in NotRunning.
    /// @param start_info Owning explicit request; no allocation or parsing is deferred to child execution.
    /// @return Success establishes exactly one later finished obligation, normally returning in Starting.
    /// If the deadline expires after watch setup but before gate release, acceptance returns in Stopping with
    /// TimedOut fixed; the gate stays closed, no started is emitted, and termination grace does not apply.
    /// Rejection emits no Process signal and leaves NotRunning. No signal is emitted inside start().
    /// Errors include invalid_state (Conflict), event_loop_unavailable (Unavailable), invalid_request
    /// (InvalidArgument), signal_configuration (Conflict), monitor_unsupported/security_unsupported (Unsupported),
    /// and safe resource_setup_failed, fork_failed, watch_failed, child_setup_failed or security_failed errors.
    /// Proven asynchronous child setup/chdir/security/exec errors instead appear in finished.start_error.
    [[nodiscard]] auto start(ProcessStartInfo start_info) -> Result<void, Error>;

    /// Requests group TERM followed by KILL at the configured grace deadline, retaining ownership until completion.
    /// @param reason Explicit first terminal cause; valid initially only in Starting or Running.
    /// @return Success for acceptance or an identical repeated reason in Stopping/Finishing. A different reason
    /// returns stop_conflict; NotRunning, or Finishing without an explicit stop, returns invalid_state.
    /// Finishing never signals a former group. A timeout-fixed result cannot be replaced by an explicit stop.
    /// Initial signal_failed leaves state/cause unchanged; ESRCH counts as successful delivery. No signal is emitted
    /// inside stop(), and successful delivery alone does not discharge the accepted completion obligation.
    [[nodiscard]] auto stop(ProcessStopReason reason = ProcessStopReason::Cancelled) -> Result<void, Error>;

    /// Returns the lifecycle state on the owner thread; NotRunning is established before finished emission.
    [[nodiscard]] auto state() const noexcept -> ProcessState;
    /// Returns the owned unreaped child ID, or no value in NotRunning/Finishing. Query on the owner thread only.
    [[nodiscard]] auto process_id() const noexcept -> std::optional<std::int64_t>;

    /// Reports clean exec-status EOF after gate release, before any output or finished, on the owner thread.
    /// Normally indicates successful exec, but pre-exec signal death can also close the writer without an error
    /// record; this signal does not prove target code executed. A deliberately unreleased gate never emits it.
    /// State is Running unless already Stopping. A direct slot may stop(); start() remains invalid.
    /// Never synchronously delete the sender from a slot: use delete_later(), allowing readiness work to return.
    Signal<>            started;
    /// Delivers nonempty owning stdout chunks in byte order after started, on the owner thread; no cumulative buffer.
    /// Ordering relative to stderr is unspecified. Direct slots may stop() subject to the current state rules,
    /// including non-signallable Finishing; start() is invalid. Never synchronously delete the sender: use
    /// delete_later() because the bounded output drain may resume after the slot returns.
    Signal<ByteBuffer>  standard_output;
    /// Delivers nonempty owning stderr chunks in byte order after started, on the owner thread; no cumulative buffer.
    /// Ordering relative to stdout is unspecified. Direct slots may stop() subject to the current state rules,
    /// including non-signallable Finishing; start() is invalid. Never synchronously delete the sender: use
    /// delete_later() because the bounded output drain may resume after the slot returns.
    Signal<ByteBuffer>  standard_error;
    /// Emits exactly once per accepted start, on the owner thread, after reaping and both output terminals.
    /// A terminal is EOF or explicit capture loss, including bounded post-reap forced closure. State is NotRunning
    /// and process_id() is empty before emission; a direct slot may start another operation; stop() is invalid.
    /// Never synchronously delete the sender: use delete_later() so the emitting readiness path can return safely.
    Signal<ProcessExit> finished;

private:
    struct Private;
};

} // namespace jb::core
