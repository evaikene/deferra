/** @file attempt_executor.hpp
 * @brief Defines asynchronous attempt execution input and completion contracts.
 */
#pragma once

#include "attempt.hpp"
#include "error.hpp"
#include "job.hpp"
#include "result.hpp"
#include "run.hpp"
#include "time_source.hpp"

#include <cstdint>
#include <functional>
#include <optional>

namespace jb::jobu {

/** Identifies one durable attempt within a run.
 *
 * Both fields are copied across the scheduler/executor boundary. `attempt_number` must be positive and identifies the
 * durable running attempt committed before execution starts.
 */
struct AttemptKey {
    /// Stable identity of the owning run.
    jb::core::Uuid run_id;
    /// Positive sequence number of the attempt within the run.
    AttemptNumber  attempt_number{1};

    /// Compares both identity components.
    auto operator==(AttemptKey const&) const -> bool = default;
};

/// Classifies whether a failed attempt is eligible for scheduler retry policy.
enum class FailureDisposition : std::uint8_t {
    Terminal,  ///< The observed failure must end the run.
    Retryable, ///< The observed failure may be retried when scheduler policy permits.
};

/** Owning immutable execution snapshot supplied after the running transition commits.
 *
 * The scheduler moves this value into AttemptExecutor::start(). An executor may retain or move any field and must not
 * rely on the associated durable run remaining otherwise accessible.
 */
struct AttemptStartRequest {
    /// Durable attempt identity committed before the executor is called.
    AttemptKey             key;
    /// Job definition identity captured by the run.
    jb::core::Uuid         job_id;
    /// Queue identity captured by the run.
    jb::core::Uuid         queue_id;
    /// Runner family that the executor must support.
    JobType                type{JobType::Cli};
    /// Complete materialized attribute snapshot owned by this request.
    AttributeSet           attributes;
    /// Owning runner payload JSON object captured by the run.
    jb::rpc::JsonValue     payload;
    /// UTC time durably recorded as the attempt start.
    jb::core::UtcTimePoint started_at;
};

/** Owning terminal observation returned by an attempt executor.
 *
 * `Succeeded` and `Cancelled` forbid a failure disposition and retry deadline. `Failed` requires a failure disposition,
 * and permits `retry_not_before` only with `Retryable`. `Interrupted` is reserved for recovery and must not be emitted
 * by normal Phase 4 executors. `result` must be a user-safe JSON object whose deterministic serialized form is at most
 * 256 KiB.
 */
struct AttemptCompletion {
    /// Identity supplied in the matching start request; executors must not alter it.
    AttemptKey                            key;
    /// Terminal classification observed by the executor.
    AttemptOutcome                        outcome{AttemptOutcome::Failed};
    /// Retry classification required only for failed outcomes.
    std::optional<FailureDisposition>     failure_disposition;
    /// Optional executor-imposed retry lower bound, valid only for retryable failures.
    std::optional<jb::core::UtcTimePoint> retry_not_before;
    /// User-safe owning result metadata encoded as a bounded JSON object.
    jb::rpc::JsonValue                    result;
};

/** Owning callback retained by an executor until one asynchronous completion.
 *
 * After a successful AttemptExecutor::start(), the executor must invoke the handler exactly once on the shared
 * executor/scheduler owner event-loop thread, with the same AttemptKey. It must not invoke the handler synchronously
 * from inside `start()`, after a failed `start()`, or after the executor has been destroyed.
 */
using AttemptCompletionHandler = std::function<void(AttemptCompletion)>;

/** Asynchronous execution boundary borrowed by the owner-thread scheduler.
 *
 * The scheduler and executor share one owner event-loop thread. The scheduler borrows its executor, so the executor
 * must outlive the scheduler and every successful start's completion. Implementations may own external work, but
 * expose only project-owned values through this interface.
 */
class AttemptExecutor {
public:
    /** Destroys the executor and its retained completion handlers.
     *
     * No retained handler may be invoked after destruction.
     */
    virtual ~AttemptExecutor() = default;

    /** Reports whether this executor can currently accept a runner family.
     *
     * The scheduler uses this notification-free query before capacity admission. An unavailable type remains pending
     * and is not treated as an execution failure.
     *
     * @param type Runner family to query.
     * @return True when this executor can currently attempt to start the type.
     * @warning Call only on the executor/scheduler owner thread.
     */
    [[nodiscard]] virtual auto is_available(JobType type) const noexcept -> bool = 0;

    /** Starts external work for an already durable running attempt.
     *
     * This call takes ownership of `request` and `completion`. On success, the executor retains the handler as needed
     * and invokes it exactly once, asynchronously, on the owner event-loop thread with a valid completion carrying the
     * same key. On failure, it invokes the handler zero times and retains neither obligation nor callback. A start
     * failure does not undo durable running state; the scheduler converts the safe returned error into terminal attempt
     * completion.
     *
     * @param request Owning immutable attempt snapshot whose running transition has committed.
     * @param completion Owning exactly-once completion handler.
     * @return Success after accepting the work, or a user-safe operational error before acceptance.
     * @warning Call only on the executor/scheduler owner thread. The handler must not run inside this call.
     */
    [[nodiscard]] virtual auto start(AttemptStartRequest request, AttemptCompletionHandler completion)
        -> jb::core::Result<void, jb::core::Error> = 0;

    /** Requests cancellation of an active attempt.
     *
     * A successful request does not complete the attempt or discharge the original completion obligation. The executor
     * must still invoke the retained handler exactly once when the external operation reaches an observed terminal
     * state. A returned error leaves the attempt active and available for a later cancellation request.
     *
     * @param key Active attempt identity borrowed for this call.
     * @return Success when cancellation was accepted, or a user-safe operational error.
     * @warning Call only on the executor/scheduler owner thread.
     */
    [[nodiscard]] virtual auto cancel(AttemptKey const& key) -> jb::core::Result<void, jb::core::Error> = 0;
};

} // namespace jb::jobu
