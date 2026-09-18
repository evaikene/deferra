/// @file attempt_executor_group.hpp
/// @brief Defines an owned router for runner-specific attempt executors.
///
#pragma once

#include "attempt_executor.hpp"

#include <memory>

namespace jb::jobu {

/// Owns runner-specific executors behind one scheduler-facing execution boundary.
///
/// Register at most one executor for each JobType before lending this group to a Scheduler. Registration always
/// consumes the supplied `unique_ptr`: a rejected executor is destroyed before add() returns. Object-derived executors
/// are accepted only while unparented and must remain unparented for their complete group-owned lifetime.
///
/// The group has no Object affinity of its own. Construct, register, use, and destroy it on the shared owner thread of
/// its executors and scheduler. Each accepted start retains one exact completion wrapper until the child completes or
/// the group is destroyed. The wrapper retires routing state before invoking the original handler, so that handler may
/// re-enter the group. Child completion values, including an incorrect key, are forwarded unchanged for Scheduler
/// validation.
///
/// @par Stable error codes
/// `jobu.executor.invalid_registration` rejects a null or already-parented executor;
/// `jobu.executor.duplicate_type` rejects a second executor for one type;
/// `jobu.executor.unsupported_type` rejects an unknown or unregistered type;
/// `jobu.executor.duplicate_attempt` rejects an already-routed key; and
/// `jobu.executor.attempt_not_found` rejects cancellation of an inactive key. Child start and cancellation errors are
/// returned unchanged. No error contains job payload, environment, path, or output data.
///
class AttemptExecutorGroup final : public AttemptExecutor {
public:
    /// Constructs an empty executor group.
    AttemptExecutorGroup();

    /// Destroys owned executors before discarding active routing state.
    ///
    /// Owned executors must suppress their retained completion handlers as required by AttemptExecutor. The group does
    /// not invoke an attempt completion while being destroyed.
    ///
    ~AttemptExecutorGroup() override;

    /// Prevents copying exclusive executor and callback ownership.
    AttemptExecutorGroup(AttemptExecutorGroup const&)                    = delete;
    /// Prevents moving state captured by active completion wrappers.
    AttemptExecutorGroup(AttemptExecutorGroup&&)                         = delete;
    /// Prevents copy assignment of exclusive executor ownership.
    auto operator=(AttemptExecutorGroup const&) -> AttemptExecutorGroup& = delete;
    /// Prevents move assignment of state captured by active completion wrappers.
    auto operator=(AttemptExecutorGroup&&) -> AttemptExecutorGroup&      = delete;

    /// Registers one exclusively owned executor for a runner family.
    ///
    /// An Object-derived executor must have no parent when passed and must not be reparented through a retained raw
    /// pointer after acceptance. Whether registration succeeds or fails, this call consumes and ultimately destroys
    /// @p executor.
    ///
    /// @param type Runner family routed to the executor.
    /// @param executor Exclusively owned executor, or null for a rejected registration.
    /// @return Success after ownership transfer, or a safe `jobu.executor.*` registration error.
    /// @warning Call only on the shared executor/scheduler owner thread.
    ///
    [[nodiscard]] auto add(JobType type, std::unique_ptr<AttemptExecutor> executor)
        -> jb::core::Result<void, jb::core::Error>;

    /// Reports the selected child executor's current availability.
    /// @param type Runner family to query.
    /// @return The registered child's current result, or false for an unregistered or unknown type.
    /// @warning Call only on the shared executor/scheduler owner thread.
    ///
    [[nodiscard]] auto is_available(JobType type) const noexcept -> bool override;

    /// Routes an already durable running attempt to its registered child executor.
    ///
    /// On child acceptance, the group remembers the exact key-to-executor route until completion. The original handler
    /// is invoked exactly as required by AttemptExecutor, after that route has been retired. A child start rejection
    /// creates no route and preserves no callback obligation.
    ///
    /// @param request Owning immutable attempt snapshot whose type selects the child executor.
    /// @param completion Owning exactly-once completion handler.
    /// @return Success after child acceptance, a safe group routing error, or the child's safe rejection unchanged.
    /// @warning Call only on the shared owner thread. The handler must not run inside this call.
    ///
    [[nodiscard]] auto start(AttemptStartRequest request, AttemptCompletionHandler completion)
        -> jb::core::Result<void, jb::core::Error> override;

    /// Routes cancellation through the executor that accepted the attempt.
    ///
    /// Current type availability is not consulted. Success preserves the original completion obligation and a child
    /// rejection leaves the route active.
    ///
    /// @param key Active attempt identity borrowed for this call.
    /// @return Success when the child accepts cancellation, a safe not-found error, or the child's safe error
    /// unchanged.
    /// @warning Call only on the shared executor/scheduler owner thread.
    ///
    [[nodiscard]] auto cancel(AttemptKey const& key) -> jb::core::Result<void, jb::core::Error> override;

private:
    struct Private;
    std::unique_ptr<Private> _data;
};

} // namespace jb::jobu
