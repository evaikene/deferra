#include "storage_failure_priv.hpp"

#include <string>
#include <string_view>

namespace jb::jobu::detail {

namespace {

struct FailurePolicy {
    StorageFailureDisposition disposition;
    std::string_view          reason;
};

auto durable_invariant(std::string_view code) noexcept -> bool
{
    // invalid_limit is caller validation despite sharing the storage namespace.
    // Other storage codes describe invalid row values or repository invariants.
    return (code.starts_with("jobu.storage.") && code != "jobu.storage.invalid_limit") ||
           code == "jobu.idempotency.invalid_record" || code == "jobu.recovery.invariant";
}

auto failure_policy(jb::core::Error const& error, StorageOperation operation, StorageFailureOrigin origin) noexcept
    -> FailurePolicy
{
    using Disposition = StorageFailureDisposition;
    auto const code   = std::string_view{error.code};

    // These identities outrank both operation context and broad ErrorCategory values.
    // In particular, a raw constraint with Conflict category is not an expected conflict.
    if (code == "db.corrupt") {
        return {.disposition = Disposition::Fatal, .reason = "corrupt"};
    }
    if (code == "db.constraint" || code.starts_with("db.constraint.")) {
        return {.disposition = Disposition::Fatal, .reason = "unexpected_constraint"};
    }
    if (origin == StorageFailureOrigin::PersistedData || durable_invariant(code)) {
        return {.disposition = Disposition::Fatal, .reason = "durable_invariant"};
    }

    // No failed recovery can admit service. Only its deliberate stop predicate
    // produces a nonfatal cancellation; a driver interruption still fails startup.
    if (operation == StorageOperation::Recovery) {
        if (code == "jobu.recovery.cancelled") {
            return {.disposition = Disposition::RecoveryCancelled, .reason = "recovery_cancelled"};
        }
        return {.disposition = Disposition::Fatal, .reason = "recovery_failed"};
    }

    if (code.starts_with("db.")) {
        if (operation == StorageOperation::Validation || operation == StorageOperation::Read) {
            return {.disposition = Disposition::OperationError, .reason = "read_failed"};
        }
        return {.disposition = Disposition::Fatal, .reason = "state_operation_failed"};
    }

    // Expected domain conflicts have already lost their raw db.constraint identity.
    // Do not inspect nested cause text, which may retain diagnostics from that conflict.
    return {.disposition = Disposition::OperationError, .reason = "operation_failed"};
}

auto operation_name(StorageOperation operation) noexcept -> std::string_view
{
    switch (operation) {
        case StorageOperation::Validation:
            return "validation";
        case StorageOperation::Read:
            return "read";
        case StorageOperation::Mutation:
            return "mutation";
        case StorageOperation::Dispatch:
            return "dispatch";
        case StorageOperation::Completion:
            return "completion";
        case StorageOperation::Recovery:
            return "recovery";
    }
    return "unknown";
}

auto failure_message(StorageFailureDisposition disposition) noexcept -> std::string_view
{
    switch (disposition) {
        case StorageFailureDisposition::OperationError:
            return "JobU operation failed";
        case StorageFailureDisposition::Fatal:
            return "JobU storage operation cannot safely continue";
        case StorageFailureDisposition::RecoveryCancelled:
            return "JobU startup recovery was cancelled";
    }
    return "JobU operation failed";
}

} // namespace

auto classify_storage_failure(jb::core::Error const& error,
                              StorageOperation       operation,
                              StorageFailureOrigin   origin) noexcept -> StorageFailureDisposition
{
    return failure_policy(error, operation, origin).disposition;
}

auto sanitized_storage_error(jb::core::Error const& error, StorageOperation operation, StorageFailureOrigin origin)
    -> jb::core::Error
{
    // Rebuild both human-readable fields: repository errors can carry backend
    // details even after translating an expected conflict to a safe domain code.
    auto const policy = failure_policy(error, operation, origin);
    return {
        .category = error.category,
        .code     = error.code,
        .message  = std::string{failure_message(policy.disposition)},
        .detail   = "operation=" + std::string{operation_name(operation)} + " reason=" + std::string{policy.reason},
    };
}

} // namespace jb::jobu::detail
