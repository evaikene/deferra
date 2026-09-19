#pragma once

#include "error.hpp"

#include <cstdint>

namespace jb::jobu::detail {

/// The enclosing operation, not the individual SQL statement that failed.
/// Reads performed inside a mutation/dispatch/completion use that enclosing context.
enum class StorageOperation : std::uint8_t {
    Validation,
    Read,
    Mutation,
    Dispatch,
    Completion,
    Recovery,
};

/// Some decoders share error codes with user-input validation. Their caller must
/// identify a failed durable-data decode explicitly; error text cannot establish provenance.
enum class StorageFailureOrigin : std::uint8_t {
    Operation,
    PersistedData,
};

enum class StorageFailureDisposition : std::uint8_t {
    OperationError,    ///< No terminal action required by the shared storage policy.
    Fatal,             ///< Runtime must stop, or startup must fail without serving.
    RecoveryCancelled, ///< Requested recovery stop; still forbids serving this invocation.
};

/// Classifies an error without performing I/O, changing runtime state, or inspecting message/detail.
/// Expected constraint conflicts must already have been translated to domain errors by the
/// owning operation. Raw constraints, corruption and durable-data invariants are always fatal.
/// Other database failures are fatal in mutation/dispatch/completion/recovery contexts.
/// Recovery treats every failure as fatal except the exact requested-stop code
/// jobu.recovery.cancelled from an Operation origin. Database cancellation is not that exception.
/// Non-storage errors outside recovery remain OperationError; subsystem-specific fatal policy
/// still applies. PersistedData is for decode/invariant failures, not an ordinary failed fetch.
[[nodiscard]] auto classify_storage_failure(jb::core::Error const& error,
                                            StorageOperation       operation,
                                            StorageFailureOrigin   origin = StorageFailureOrigin::Operation) noexcept
    -> StorageFailureDisposition;

/// Preserves category and the module-owned, non-sensitive stable code. Replaces message/detail
/// with fixed policy text and operation/reason tokens, never copying backend diagnostics or data.
/// As required by core::Error, code must be a trusted machine identifier, not user/backend text.
/// Does not mutate error; use the same context/origin as classification at the result boundary.
[[nodiscard]] auto sanitized_storage_error(jb::core::Error const& error,
                                           StorageOperation       operation,
                                           StorageFailureOrigin   origin = StorageFailureOrigin::Operation)
    -> jb::core::Error;

} // namespace jb::jobu::detail
