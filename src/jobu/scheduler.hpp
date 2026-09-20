/// @file scheduler.hpp
/// @brief Defines JobU's centralized owner-thread scheduler.
///
#pragma once

#include "attempt_executor.hpp"
#include "cron.hpp"
#include "error.hpp"
#include "object.hpp"
#include "result.hpp"
#include "run.hpp"
#include "signal.hpp"
#include "time_source.hpp"
#include "uuid.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace jb::db {
class Database;
}

namespace jb::jobu {

class AttributeRegistry;

/// Controls scheduler capacity, candidate paging, and wall-clock reevaluation.
///
/// Construction copies this value. Zero concurrency limits, a zero batch size, or a nonpositive wall-clock recheck
/// interval are rejected by `Scheduler::start()` with `jobu.scheduler.invalid_options`.
///
struct SchedulerOptions {
    /// Maximum concurrently running CLI attempts across all queues.
    std::uint32_t        cli_concurrency{4};
    /// Maximum concurrently running HTTP attempts across all queues.
    std::uint32_t        http_concurrency{16};
    /// Maximum candidates fetched by one bounded scheduler repository query.
    std::size_t          candidate_batch_size{200};
    /// Maximum monotonic wait before persisted UTC deadlines are reevaluated.
    std::chrono::seconds wall_clock_recheck{60};
};

/// Describes the public scheduler lifecycle.
enum class SchedulerState : std::uint8_t {
    /// No scheduling cycle or wake is armed.
    Stopped,
    /// Scheduling cycles and event-loop wakes are enabled.
    Running,
    /// A fatal scheduler error was stored and the instance cannot be restarted.
    Failed,
    /// Shutdown was requested without a fatal error; dispatch and completion persistence are permanently disabled.
    Shutdown,
};

/// Describes whether run cancellation completed durably or was delegated to an active executor.
enum class CancelDisposition : std::uint8_t {
    /// The cancellation transaction committed and `CancelRunResult::run` is the terminal cancelled snapshot.
    Completed,
    /// The active executor accepted cancellation and `CancelRunResult::run` remains a running snapshot.
    Requested,
};

/// Owning result of cancelling one non-terminal run.
///
/// `Completed` means the run is durably terminal, any required recurring successor and suspension drains committed in
/// the same transaction, and capacity may be reused. `Requested` means the executor still owns an active attempt; the
/// scheduler retains its capacity until the executor reports completion and the forced cancelled outcome commits.
///
struct CancelRunResult {
    /// Owning run snapshot at the cancellation operation's durable boundary.
    JobRun            run;
    /// Whether cancellation completed synchronously or awaits executor completion.
    CancelDisposition disposition{CancelDisposition::Completed};
};

/// Centralizes durable JobU dispatch on one owner-thread event loop.
///
/// Scheduler borrows its database, attribute registry, cron engine, UUID generator, time source, and attempt executor;
/// every dependency must outlive it. The database must already be open and idle. Construct and use the scheduler on the
/// database and executor owner thread after that thread's EventLoop has been installed. Passing @p parent transfers
/// normal `jb::core::Object` lifetime ownership but does not transfer ownership of any scheduler dependency.
///
/// Scheduling is event-driven and uses one non-repeating timer. Executor completion handlers remain valid after stop()
/// so their durable outcomes can commit, but they do not restart dispatch while stopped. Shutdown, fatal failure, and
/// destruction invalidate those handlers: later owner-thread delivery is a no-op, even after Scheduler destruction.
/// The borrowed executor must still outlive the Scheduler; shutdown does not terminate executor-owned work.
///
class Scheduler final : public jb::core::Object {
public:
    /// Constructs a stopped scheduler without querying the database or invoking callbacks.
    /// @param database Open, idle database borrowed for the scheduler lifetime.
    /// @param attributes Immutable registry borrowed for decoding every persisted execution snapshot.
    /// @param cron Cron implementation borrowed for recurring successors.
    /// @param uuid_generator UUID source borrowed for recurring successors.
    /// @param time_source Wall-clock source borrowed for durable scheduler timestamps and wake calculations.
    /// @param executor Attempt executor borrowed for availability, start, and cancellation operations.
    /// @param options Copied scheduler limits and wake policy. Invalid values are retained for start() to report.
    /// @param parent Optional Object that owns this scheduler and supplies its event-loop affinity.
    /// @warning Every argument and the constructor call itself belong to the same owner thread.
    ///
    Scheduler(jb::db::Database&        database,
              AttributeRegistry const& attributes,
              CronEngine const&        cron,
              jb::core::UuidGenerator& uuid_generator,
              jb::core::TimeSource&    time_source,
              AttemptExecutor&         executor,
              SchedulerOptions         options = {},
              jb::core::Object*        parent  = nullptr);

    /// Permanently disables scheduling and retained completions before releasing adapter resources.
    ///
    /// Performs shutdown(); retained owner-thread completions are harmless after destruction. Active attempts are not
    /// cancelled or durably finalized. The owner must separately tear down executor-owned work.
    ///
    ~Scheduler() override;

    /// Prevents copying a thread-affine scheduler with retained callbacks.
    Scheduler(Scheduler const&)                    = delete;
    /// Prevents moving a thread-affine scheduler with fixed dependency addresses.
    Scheduler(Scheduler&&)                         = delete;
    /// Prevents copy assignment of a thread-affine scheduler.
    auto operator=(Scheduler const&) -> Scheduler& = delete;
    /// Prevents move assignment of a thread-affine scheduler.
    auto operator=(Scheduler&&) -> Scheduler&      = delete;

    /// Starts dispatch and arms the event-loop wake adapter.
    ///
    /// The call verifies option, event-loop, database owner-thread, and Phase 4 recovery preconditions before entering
    /// Running. It then performs one scheduling cycle synchronously and arms at most one future wake. Calling start()
    /// while Running succeeds without another cycle. After fatal failure or shutdown(), returns
    /// `jobu.scheduler.stopping` (Unavailable) without accessing storage; failure() retains the first fatal error.
    /// Fatal cycle or wake failures observed after entering Running may emit failed synchronously before this call
    /// returns.
    ///
    /// @return Success in Running state, or a stable scheduler/database Error. Preflight failures leave the scheduler
    /// Stopped; fatal scheduling failures leave it Failed. A reentrant shutdown during the initial cycle leaves it
    /// Shutdown and returns `jobu.scheduler.stopping`.
    ///
    [[nodiscard]] auto start() -> jb::core::Result<void, jb::core::Error>;

    /// Stops dispatch and disarms the wake timer without changing durable run state.
    ///
    /// The call is idempotent in Stopped and does not clear Failed or Shutdown. Active executor completions may still
    /// persist after stop, but successful completions do not schedule another cycle. Restart remains subject to the
    /// persisted-running-state startup guard.
    ///
    void stop();

    /// Irreversibly disables dispatch, wakes, rescans, cancellation, and retained completion persistence.
    ///
    /// Owner-thread only and idempotent. A healthy instance enters Shutdown; Failed and its first error remain intact.
    /// This only latches gates and disarms the timer: it does not cancel executor work, destroy active callback state,
    /// or write durable outcomes. Unresolved Running rows remain for startup recovery. Safe from a synchronous failed
    /// slot; destruction must still wait until the active scheduler/callback stack has unwound.
    ///
    void shutdown() noexcept;

    /// Coalesces a new scheduling cycle onto the owner event loop.
    ///
    /// When Running, this replaces a later wake with one zero-delay timer. Repeated notifications before that timer
    /// fires remain one cycle. The call is a no-op while Stopped, Failed, or Shutdown and never dispatches or emits
    /// failed synchronously.
    ///
    void request_rescan();

    /// Cancels one non-terminal run through the deterministic scheduler core.
    /// @param run_id Stable run UUID borrowed only for this call.
    /// @return A committed terminal snapshot for scheduled/retry-wait work, a still-running snapshot after one accepted
    /// executor cancellation request, or a stable run, executor, recovery, scheduler-state, or persistence Error.
    ///
    /// Scheduler must be Running. Pending cancellation, a possible recurring successor, and suspension drains commit in
    /// one transaction before `Completed` is returned. `Requested` retains capacity until the executor's exactly-once
    /// completion is forced to the cancelled outcome and commits. A synchronously completed cancellation schedules a
    /// later coalesced rescan. A fatal storage failure emits failed after transaction unwinding; expected run conflicts
    /// and executor cancellation refusals remain ordinary operation errors.
    /// After fatal failure or shutdown(), rejects with `jobu.scheduler.stopping` without accessing storage or
    /// executors.
    ///
    [[nodiscard]] auto cancel_run(jb::core::Uuid const& run_id) -> jb::core::Result<CancelRunResult, jb::core::Error>;

    /// Returns the current lifecycle state without transferring ownership.
    [[nodiscard]] auto state() const noexcept -> SchedulerState;

    /// Returns the first fatal error stored by this scheduler.
    /// @return An owning error copy in Failed state, including after shutdown(), or no value in any healthy state.
    ///
    [[nodiscard]] auto failure() const -> std::optional<jb::core::Error>;

    /// Emitted exactly once when the scheduler first enters Failed.
    ///
    /// Emission is synchronous on the owner thread after state and failure() have been updated and the timer stopped.
    /// Retained completions have already been invalidated and local transaction guards have unwound. Emission may
    /// occur inside start(), cancel_run(), a timer callback, or an executor completion callback. Direct slots may
    /// inspect the scheduler and call shutdown(), but must not destroy it or otherwise re-enter a mutating scheduler
    /// method before signal delivery returns.
    ///
    jb::core::Signal<jb::core::Error> failed;

private:
    struct Private;
};

} // namespace jb::jobu
