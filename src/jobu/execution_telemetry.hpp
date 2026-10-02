/// @file execution_telemetry.hpp
/// @brief Owner-thread activation and failure boundary for observed runnable-wait accounting.
#pragma once

#include "error.hpp"
#include "job.hpp"
#include "object.hpp"
#include "result.hpp"
#include "signal.hpp"
#include "uuid.hpp"

#include <chrono>
#include <cstddef>

namespace jb::db {
class Database;
}

namespace jb::core {
class TimeSource;
class UuidGenerator;
} // namespace jb::core

namespace jb::jobu {

class AttributeRegistry;
class AttemptExecutor;

namespace detail {
struct TelemetryAccess;
}

/// Immutable accounting policy, copied by ExecutionTelemetry.
struct TelemetryOptions {
    std::chrono::seconds checkpoint_interval{30}; ///< 1..86400 seconds between checkpoint sweeps.
    std::size_t          batch_size{200};         ///< 1..1000 rows per checkpoint or stop transaction.
};

/// Owning delay diagnostic. IDs identify the observed run and its current owners; runnable_wait
/// is a monotonic observed lower bound for Partial measurements. Contains no execution payload.
struct DelayedRun {
    jb::core::Uuid            run_id;
    jb::core::Uuid            job_id;
    jb::core::Uuid            queue_id;
    JobType                   type{JobType::Cli};
    std::chrono::microseconds runnable_wait{0};
    std::chrono::milliseconds threshold{0};
};

/// Owns one monotonic accounting epoch without selecting or executing work.
/// Borrows an open, schema-prepared database, immutable registry, clock and UUID source;
/// all must outlive the service. Construct, activate, use and destroy on their owner thread
/// with an installed event loop. The optional parent owns the service.
///
/// Activation arms bounded checkpoint maintenance without observing new eligibility. Private transaction adapters
/// perform accounting in their caller's transaction and return owning results without signals.
/// New covered runs may start Complete; previously unmeasured work becomes Partial when observed.
/// UTC determines observed eligibility elsewhere; only monotonic elapsed time contributes to wait.
/// Destruction and request_stop() perform no SQL and do not settle durable open intervals.
/// Healthy finish_stop() must run after all borrower callbacks/transactions unwind and before database close.
/// A crash retains only checkpointed wait; startup recovery marks abandoned open intervals Partial.
/// A large sweep can exceed checkpoint_interval, which is not a maximum crash-loss guarantee.
/// Integrated management requires the borrowing Scheduler to register its actual executor
/// capabilities before activation. Registration is fixed for this owner's lifetime.
class ExecutionTelemetry final : public jb::core::Object {
public:
    /// Constructs an inactive owner without reading clocks, generating an epoch or performing SQL.
    /// Copies options for start() validation; collaborator lifetimes and affinity follow the class contract.
    ExecutionTelemetry(jb::db::Database&        database,
                       AttributeRegistry const& attributes,
                       jb::core::TimeSource&    time_source,
                       jb::core::UuidGenerator& uuid_generator,
                       TelemetryOptions         options = TelemetryOptions(),
                       jb::core::Object*        parent  = nullptr);
    /// Invalidates accounting without changing durable rows or emitting a failure.
    ~ExecutionTelemetry() override;

    ExecutionTelemetry(ExecutionTelemetry const&)                    = delete;
    ExecutionTelemetry(ExecutionTelemetry&&)                         = delete;
    auto operator=(ExecutionTelemetry const&) -> ExecutionTelemetry& = delete;
    auto operator=(ExecutionTelemetry&&) -> ExecutionTelemetry&      = delete;

    /// Validates options/affinity and generates one non-nil epoch with a monotonic origin.
    /// Arms one maintenance timer. Already active succeeds without replacing the epoch or timer.
    /// Performs no SQL or signal emission; recovery must already have repaired abandoned intervals.
    /// Invalid policy returns jobu.telemetry.invalid_options; invalid owner/event-loop affinity,
    /// a stopped owner or an invalid generated epoch returns jobu.telemetry.invalid_state.
    /// UUID/backend failures preserve their trusted codes with sanitized diagnostics.
    /// A stopped instance cannot reactivate; a failed instance returns its latched error.
    [[nodiscard]] auto start() -> jb::core::Result<void, jb::core::Error>;

    /// Captures the first owner-thread stop boundary, disarms maintenance and invalidates ordinary
    /// samples without SQL or signals. Idempotent and safe on a mutation/failure stack. Clock or
    /// affinity errors are deferred to finish_stop(); a failed owner does not sample again.
    void request_stop() noexcept;

    /// After request_stop() and stack/transaction unwind, closes open intervals at the captured
    /// boundary in synchronous transactions of at most batch_size rows. Repeated success performs
    /// no SQL. Dormant owners perform no SQL; failed/poisoned owners never attempt cleanup writes.
    /// Does not finalize attempts, emit delay warnings or include shutdown/downtime in wait.
    /// Failure can leave earlier batches committed; restart recovery repairs the remaining tails.
    /// Returns the latched failure or reports a cleanup failure through failed after SQL cleanup.
    /// Direct failure receivers may destroy this owner. Calling while active is an invalid state.
    [[nodiscard]] auto finish_stop() -> jb::core::Result<void, jb::core::Error>;

    /// Emitted on the owner thread only after a successful durable warning claim and SQL cleanup.
    /// At most once per run across retries, moves and restarts; a crash or stop between claim and
    /// delivery may lose the diagnostic. Queued delivery owns its value. Direct receivers may
    /// request_stop(), request scheduler shutdown, or destroy this owner; scheduler destruction
    /// must wait until its active call stack unwinds. No further observation follows a stopped owner.
    jb::core::Signal<DelayedRun> delayed;

    /// Emitted once by the private post-transaction failure boundary, after all SQL cleanup.
    /// Storage errors retain their enclosing operation/origin; clock/accounting failures also
    /// fail this owner explicitly. Queued receivers get owning values; direct receivers may
    /// destroy the owner. Subsequent accounting and activation remain disabled.
    jb::core::Signal<jb::core::Error> failed;

private:
    friend struct detail::TelemetryAccess;
    struct Private;
};

} // namespace jb::jobu
