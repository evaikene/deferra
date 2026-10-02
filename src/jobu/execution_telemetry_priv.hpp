#pragma once

#include "execution_telemetry.hpp"
#include "run_eligibility_priv.hpp"
#include "run_repository_priv.hpp"
#include "storage_failure_priv.hpp"
#include "uuid.hpp"
#include "wait_repository_priv.hpp"

#include <memory>

namespace jb::core::priv {
struct ObjectLifetime;
}

namespace jb::jobu::detail {

/// Immutable owning sample. Identity fences older boundaries even when their ticks are equal.
using TelemetrySample = std::shared_ptr<WaitSample const>;

/// One mutation's shared clock/owner boundary. Its helpers borrow the caller's transaction,
/// consume bounded pages synchronously and emit nothing. Roll back the whole mutation on failure.
struct MutationTiming {
    ExecutionTelemetry*    owner{};
    jb::db::Database*      database{};
    TelemetrySample        sample;
    jb::core::UtcTimePoint utc_now;
    AvailableJobTypes      available;

    [[nodiscard]] auto measurement() const noexcept -> InitialRunMeasurement;
    [[nodiscard]] auto settle_scope(EligibilityScope scope) const -> TelemetryResult<void>;
    [[nodiscard]] auto reconcile_scope(EligibilityScope scope) const -> TelemetryResult<void>;

    /// The caller has revalidated selection. First observation establishes Partial for old
    /// unmeasured work, then closes its tail before the run becomes Running. No external calls.
    [[nodiscard]] auto claim_run(jb::core::Uuid const& run_id) const -> TelemetryResult<void>;

    /// Validates the required timing row, including closed Running state. Inactive borrowers
    /// reject abandoned intervals rather than comparing ticks from an unknown owner.
    [[nodiscard]] auto validate_closed_run(jb::core::Uuid const& run_id) const -> TelemetryResult<void>;
};

/// The sole private access seam for enclosing management/scheduler transactions.
/// Borrow the owner synchronously; never retain this seam or call it from another thread.
struct TelemetryAccess {
    /// Registers the actual fixed executor set before activation, validating collaborator identity.
    /// A second distinct registration or registration after activation is rejected without SQL.
    [[nodiscard]] static auto register_executor(ExecutionTelemetry&      owner,
                                                jb::db::Database&        database,
                                                AttributeRegistry const& attributes,
                                                jb::core::TimeSource&    clock,
                                                AttemptExecutor const&   executor) -> TelemetryResult<void>;
    [[nodiscard]] static auto available_types(ExecutionTelemetry& owner) -> TelemetryResult<AvailableJobTypes>;

    /// Null/fresh/stopped owners permit unmeasured operation only over closed timing rows.
    /// Active owners require registered capabilities and matching borrowed collaborators.
    [[nodiscard]] static auto mutation_boundary(ExecutionTelemetry*      owner,
                                                jb::db::Database&        database,
                                                AttributeRegistry const& attributes,
                                                jb::core::TimeSource&    clock) -> TelemetryResult<MutationTiming>;

    /// Captured before notification so a preceding receiver cannot leave a dangling failure target.
    [[nodiscard]] static auto lifetime(ExecutionTelemetry& owner) -> std::weak_ptr<jb::core::priv::ObjectLifetime>;

    /// Captures UTC/monotonic once and invalidates the prior boundary, without SQL or signals.
    [[nodiscard]] static auto sample(ExecutionTelemetry& owner) -> TelemetryResult<TelemetrySample>;

    /// Selects Complete only while this owner actively covers creation. Performs no SQL.
    [[nodiscard]] static auto initial_measurement(ExecutionTelemetry& owner) -> TelemetryResult<InitialRunMeasurement>;

    /// Accounting calls require an active owner, its latest sample and a caller-owned transaction.
    /// Reuse the same sample to settle, mutate domain state and reopen. Roll back on any failure;
    /// these calls neither emit signals nor report failure reentrantly.
    [[nodiscard]] static auto open_interval(ExecutionTelemetry&    owner,
                                            jb::core::Uuid const&  run_id,
                                            TelemetrySample const& sample) -> TelemetryResult<WaitTiming>;
    [[nodiscard]] static auto settle(ExecutionTelemetry&    owner,
                                     jb::core::Uuid const&  run_id,
                                     TelemetrySample const& sample) -> TelemetryResult<WaitTiming>;
    [[nodiscard]] static auto rebase(ExecutionTelemetry&    owner,
                                     jb::core::Uuid const&  run_id,
                                     TelemetrySample const& sample) -> TelemetryResult<WaitTiming>;

    /// Call only after destroying every query and transaction guard in the enclosing operation.
    /// Latches once, gates accounting, sanitizes using that operation and emits failed last.
    /// Clock/accounting failures are fatal here even when the storage classifier calls them
    /// OperationError. An original fatal failure outranks a secondary rollback-poison error.
    static void report_failure(ExecutionTelemetry& owner, TelemetryFailure failure, StorageOperation operation);
};

} // namespace jb::jobu::detail
