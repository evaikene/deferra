#pragma once

#include "execution_telemetry.hpp"
#include "run_repository_priv.hpp"
#include "storage_failure_priv.hpp"
#include "uuid.hpp"
#include "wait_repository_priv.hpp"

#include <memory>

namespace jb::jobu::detail {

/// Immutable owning sample. Identity fences older boundaries even when their ticks are equal.
using TelemetrySample = std::shared_ptr<WaitSample const>;

/// The sole private access seam for enclosing management/scheduler transactions.
/// Borrow the owner synchronously; never retain this seam or call it from another thread.
struct TelemetryAccess {
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
