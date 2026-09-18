/// @file cli_attempt_executor.hpp
/// @brief Defines asynchronous execution of durable JobU CLI attempts.
///
#pragma once

#include "attempt_executor.hpp"
#include "object.hpp"

#include <memory>

namespace jb::jobu::cli {

namespace detail {
struct CliAttemptExecutorTestAccess;
struct CliAttemptExecutorFactory;
} // namespace detail

/// Configures process-identity policy for CLI attempts.
struct CliAttemptExecutorOptions {
    /// Permits CLI execution while the daemon's effective user ID is zero. This override is unsafe.
    bool allow_root{false};
};

/// Executes durable CLI attempts on one Object/EventLoop owner thread.
///
/// A successful start retains one exact completion handler until the accepted operation reports a terminal event.
/// Output is consumed incrementally and retained according to the immutable attempt snapshot. The handler runs only
/// after every executor-owned reference to the attempt has been retired, so it may re-enter start() or cancel().
/// Rejected starts invoke no handler and retain no callback.
///
/// The executor owns its private state through Object's single private-data allocation. A non-null parent owns the
/// executor, which must then be heap allocated. Construct, use, and destroy the executor on its owner thread.
/// Destruction suppresses retained callbacks before requesting immediate cleanup of every active operation.
///
/// CLI availability requires a valid current owner EventLoop, an installed platform Process adapter, and either a
/// non-root effective identity or the explicit unsafe override. The Linux adapter owns every accepted Process as an
/// Object child and suppresses completion delivery while executor destruction kills and reaps active process groups.
/// Platforms whose Process backend is not yet integrated report CLI unavailable.
///
/// @par Stable error codes
/// `jobu.cli.unsupported_type` rejects non-CLI work; `jobu.cli.invalid_start` rejects an empty callback or invalid key;
/// `jobu.cli.invalid_snapshot` rejects corrupt durable payload or attributes; `jobu.cli.duplicate_attempt` rejects an
/// active key; `jobu.cli.event_loop_unavailable` rejects a missing/non-current owner loop;
/// `jobu.cli.root_forbidden` enforces the default root policy; `jobu.cli.start_failed` safely wraps a process start
/// rejection; `jobu.cli.attempt_not_found` rejects cancellation of an inactive key; and `jobu.cli.cancel_failed`
/// safely wraps a process stop rejection. Errors never contain command, argument, path, environment, or output data.
///
class CliAttemptExecutor final : public jb::core::Object, public jb::jobu::AttemptExecutor {
public:
    /// Constructs an idle executor without starting external work.
    /// @param options Root-execution policy retained for the executor lifetime.
    /// @param parent Optional same-thread Object parent that assumes ownership.
    /// @warning Call on the EventLoop thread that will own all later operations.
    ///
    explicit CliAttemptExecutor(CliAttemptExecutorOptions options = {}, jb::core::Object* parent = nullptr);

    /// Suppresses completion delivery and requests immediate cleanup of every active operation.
    ~CliAttemptExecutor() override;

    /// Prevents copying an executor whose operations retain its identity.
    CliAttemptExecutor(CliAttemptExecutor const&)                    = delete;
    /// Prevents moving an executor whose operations retain its identity.
    CliAttemptExecutor(CliAttemptExecutor&&)                         = delete;
    /// Prevents copy assignment of owner-thread operation state.
    auto operator=(CliAttemptExecutor const&) -> CliAttemptExecutor& = delete;
    /// Prevents move assignment of owner-thread operation state.
    auto operator=(CliAttemptExecutor&&) -> CliAttemptExecutor&      = delete;

    /// Reports whether CLI work can currently be accepted.
    /// @param type Runner family to query.
    /// @return True only for CLI with a valid current owner loop, installed adapter, and allowed effective identity.
    /// @warning Call only on the executor owner thread.
    ///
    [[nodiscard]] auto is_available(JobType type) const noexcept -> bool override;

    /// Starts one CLI operation for an already durable running attempt.
    /// @param request Owning immutable execution snapshot.
    /// @param completion Owning nonempty exactly-once completion handler.
    /// @return Success after adapter acceptance, or a safe `jobu.cli.*` error with no callback obligation.
    /// @warning Call only on the owner thread. The handler never runs inside this call.
    ///
    [[nodiscard]] auto start(AttemptStartRequest request, AttemptCompletionHandler completion)
        -> jb::core::Result<void, jb::core::Error> override;

    /// Requests cancellation of an active CLI operation.
    /// @param key Exact active attempt identity.
    /// @return Success when cancellation is accepted, or a safe `jobu.cli.*` error.
    /// @warning Call only on the owner thread. Success retains the original completion obligation.
    ///
    [[nodiscard]] auto cancel(AttemptKey const& key) -> jb::core::Result<void, jb::core::Error> override;

private:
    friend struct detail::CliAttemptExecutorTestAccess;
    friend struct detail::CliAttemptExecutorFactory;
    struct Private;

    /// Retains allocation ownership until Object construction succeeds.
    CliAttemptExecutor(std::unique_ptr<Private> data, jb::core::Object* parent);
};

} // namespace jb::jobu::cli
