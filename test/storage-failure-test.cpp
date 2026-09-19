#include "storage_failure_priv.hpp"

#include "error.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>
#include <string_view>

namespace {

using jb::core::Error;
using jb::core::ErrorCategory;
using jb::jobu::detail::classify_storage_failure;
using jb::jobu::detail::sanitized_storage_error;
using jb::jobu::detail::StorageFailureDisposition;
using jb::jobu::detail::StorageFailureOrigin;
using jb::jobu::detail::StorageOperation;

constexpr std::array operations{
    StorageOperation::Validation,
    StorageOperation::Read,
    StorageOperation::Mutation,
    StorageOperation::Dispatch,
    StorageOperation::Completion,
    StorageOperation::Recovery,
};

constexpr std::array categories{
    ErrorCategory::InvalidArgument,
    ErrorCategory::NotFound,
    ErrorCategory::Conflict,
    ErrorCategory::PermissionDenied,
    ErrorCategory::Unavailable,
    ErrorCategory::ResourceExhausted,
    ErrorCategory::Cancelled,
    ErrorCategory::Timeout,
    ErrorCategory::Io,
    ErrorCategory::Unsupported,
    ErrorCategory::Internal,
};

auto error_with_code(std::string_view code, ErrorCategory category = ErrorCategory::Internal) -> Error
{
    return {.category = category, .code = std::string{code}, .message = "Original operation error"};
}

} // namespace

TEST_CASE("Expected domain errors do not trigger runtime storage shutdown", "[jobu][storage-failure]")
{
    struct DomainError {
        std::string_view code;
        ErrorCategory    category;
    };

    constexpr std::array errors{
        DomainError{.code = "jobu.job.invalid_payload",     .category = ErrorCategory::InvalidArgument},
        DomainError{.code = "jobu.queue.invalid_name",      .category = ErrorCategory::InvalidArgument},
        DomainError{.code = "jobu.attribute.invalid_value", .category = ErrorCategory::InvalidArgument},
        DomainError{.code = "jobu.storage.invalid_limit",   .category = ErrorCategory::InvalidArgument},
        DomainError{.code = "jobu.job.not_found",           .category = ErrorCategory::NotFound       },
        DomainError{.code = "jobu.queue.not_found",         .category = ErrorCategory::NotFound       },
        DomainError{.code = "jobu.job.revision_conflict",   .category = ErrorCategory::Conflict       },
        DomainError{.code = "jobu.job.state_conflict",      .category = ErrorCategory::Conflict       },
        DomainError{.code = "jobu.queue.name_conflict",     .category = ErrorCategory::Conflict       },
        DomainError{.code = "jobu.run.schedule_conflict",   .category = ErrorCategory::Conflict       },
        DomainError{.code = "jobu.run.manual_conflict",     .category = ErrorCategory::Conflict       },
        DomainError{.code = "jobu.idempotency.conflict",    .category = ErrorCategory::Conflict       },
        DomainError{.code = "jobu.secret.in_use",           .category = ErrorCategory::Conflict       },
    };

    for (auto const& expected : errors) {
        auto error   = error_with_code(expected.code, expected.category);
        // Translation may retain a backend cause. Policy uses the translated identity,
        // never diagnostic text, even when that text resembles a fatal code.
        error.detail = "cause=db.constraint.unique backend-text=db.corrupt";
        for (auto const operation : operations) {
            CAPTURE(expected.code, operation);
            auto const disposition = operation == StorageOperation::Recovery
                                       ? StorageFailureDisposition::Fatal
                                       : StorageFailureDisposition::OperationError;
            CHECK(classify_storage_failure(error, operation) == disposition);
        }
    }
}

TEST_CASE("Corruption and raw constraints are fatal regardless of category or operation", "[jobu][storage-failure]")
{
    constexpr std::array codes{
        "db.corrupt",
        "db.constraint",
        "db.constraint.unique",
        "db.constraint.foreign_key",
        "db.constraint.check",
    };
    for (auto const* code : codes) {
        for (auto const category : categories) {
            auto const error = error_with_code(code, category);
            for (auto const operation : operations) {
                CAPTURE(code, category, operation);
                CHECK(classify_storage_failure(error, operation) == StorageFailureDisposition::Fatal);
            }
        }
    }
}

TEST_CASE("Database failure severity follows the enclosing operation", "[jobu][storage-failure]")
{
    constexpr std::array codes{
        "db.busy",
        "db.locked",
        "db.io",
        "db.permission_denied",
        "db.transaction_begin_failed",
        "db.prepare_failed",
        "db.execute_failed",
        "db.fetch_failed",
        "db.transaction_commit_failed",
        "db.transaction_rollback_failed",
        "db.query_active",
        "db.future_driver_failure",
    };
    for (auto const* code : codes) {
        // Driver categories vary, including InvalidArgument for prepare failures.
        // None of those categories can downgrade a failure inside a state operation.
        for (auto const category : categories) {
            auto const error = error_with_code(code, category);
            CAPTURE(code, category);
            CHECK(classify_storage_failure(error, StorageOperation::Validation) ==
                  StorageFailureDisposition::OperationError);
            CHECK(classify_storage_failure(error, StorageOperation::Read) == StorageFailureDisposition::OperationError);
            CHECK(classify_storage_failure(error, StorageOperation::Mutation) == StorageFailureDisposition::Fatal);
            CHECK(classify_storage_failure(error, StorageOperation::Dispatch) == StorageFailureDisposition::Fatal);
            CHECK(classify_storage_failure(error, StorageOperation::Completion) == StorageFailureDisposition::Fatal);
            CHECK(classify_storage_failure(error, StorageOperation::Recovery) == StorageFailureDisposition::Fatal);
        }
    }
}

TEST_CASE("Durable invariants are fatal even on ordinary reads", "[jobu][storage-failure]")
{
    constexpr std::array codes{
        "jobu.storage.invariant",
        "jobu.storage.invalid_blob",
        "jobu.storage.invalid_boolean",
        "jobu.storage.invalid_enum",
        "jobu.storage.invalid_integer",
        "jobu.storage.invalid_json",
        "jobu.storage.invalid_queue",
        "jobu.storage.invalid_text",
        "jobu.storage.invalid_time",
        "jobu.storage.invalid_uuid",
        "jobu.idempotency.invalid_record",
        "jobu.recovery.invariant",
    };
    for (auto const* code : codes) {
        // Repository affected-row mismatches use the same invariant identity.
        auto error   = error_with_code(code, ErrorCategory::InvalidArgument);
        error.detail = "reason=invalid_dispatch_affected_rows";
        for (auto const operation : operations) {
            CAPTURE(code, operation);
            CHECK(classify_storage_failure(error, operation) == StorageFailureDisposition::Fatal);
        }
    }
}

TEST_CASE("Shared validation codes require durable-data provenance", "[jobu][storage-failure]")
{
    auto const error = error_with_code("jobu.attribute.invalid_value", ErrorCategory::InvalidArgument);
    CHECK(classify_storage_failure(error, StorageOperation::Mutation) == StorageFailureDisposition::OperationError);

    // The same attribute decoder can validate a request or decode stored defaults.
    // Only the caller knows which input failed; no message parsing can recover that fact.
    for (auto const operation : operations) {
        CAPTURE(operation);
        CHECK(classify_storage_failure(error, operation, StorageFailureOrigin::PersistedData) ==
              StorageFailureDisposition::Fatal);
    }
    auto const sanitized = sanitized_storage_error(error, StorageOperation::Read, StorageFailureOrigin::PersistedData);
    CHECK(sanitized.code == error.code);
    CHECK(sanitized.detail == "operation=read reason=durable_invariant");
}

TEST_CASE("Only requested recovery cancellation is a nonfatal startup stop", "[jobu][storage-failure]")
{
    auto const cancelled = error_with_code("jobu.recovery.cancelled", ErrorCategory::Cancelled);
    CHECK(classify_storage_failure(cancelled, StorageOperation::Recovery) ==
          StorageFailureDisposition::RecoveryCancelled);
    CHECK(classify_storage_failure(cancelled, StorageOperation::Read) == StorageFailureDisposition::OperationError);
    CHECK(classify_storage_failure(cancelled, StorageOperation::Recovery, StorageFailureOrigin::PersistedData) ==
          StorageFailureDisposition::Fatal);

    for (auto const* code :
         {"db.interrupted", "core.cancelled", "jobu.recovery.cancelled_extra", "jobu.job.not_found"}) {
        CAPTURE(code);
        CHECK(classify_storage_failure(error_with_code(code, ErrorCategory::Cancelled), StorageOperation::Recovery) ==
              StorageFailureDisposition::Fatal);
    }
}

TEST_CASE("Classification respects namespace boundaries and ignores diagnostic text", "[jobu][storage-failure]")
{
    for (auto const* code :
         {"external.db.io", "dbx.io", "jobu.storagex.invariant", "jobu.idempotency.invalid_record_extra"}) {
        auto error    = error_with_code(code);
        error.message = "db.corrupt";
        error.detail  = "jobu.storage.invariant";
        CAPTURE(code);
        CHECK(classify_storage_failure(error, StorageOperation::Mutation) == StorageFailureDisposition::OperationError);
    }
    // A similarly named database code still follows database read/write policy,
    // but is not mistaken for a known corruption/constraint identity.
    for (auto const* code : {"db.corrupt_extra", "db.constraints"}) {
        auto const error = error_with_code(code);
        CAPTURE(code);
        CHECK(classify_storage_failure(error, StorageOperation::Read) == StorageFailureDisposition::OperationError);
        CHECK(classify_storage_failure(error, StorageOperation::Mutation) == StorageFailureDisposition::Fatal);
    }
}

TEST_CASE("Sanitized errors preserve identity and emit only controlled context", "[jobu][storage-failure]")
{
    struct Scenario {
        StorageOperation operation;
        std::string_view code;
        ErrorCategory    category;
        std::string_view detail;
        std::string_view message;
    };

    constexpr std::array scenarios{
        Scenario{.operation = StorageOperation::Validation,
                 .code      = "jobu.storage.invalid_limit",
                 .category  = ErrorCategory::InvalidArgument,
                 .detail    = "operation=validation reason=operation_failed",
                 .message   = "JobU operation failed"                        },
        Scenario{.operation = StorageOperation::Read,
                 .code      = "db.io",
                 .category  = ErrorCategory::Io,
                 .detail    = "operation=read reason=read_failed",
                 .message   = "JobU operation failed"                        },
        Scenario{.operation = StorageOperation::Mutation,
                 .code      = "jobu.queue.name_conflict",
                 .category  = ErrorCategory::Conflict,
                 .detail    = "operation=mutation reason=operation_failed",
                 .message   = "JobU operation failed"                        },
        Scenario{.operation = StorageOperation::Dispatch,
                 .code      = "db.busy",
                 .category  = ErrorCategory::Unavailable,
                 .detail    = "operation=dispatch reason=state_operation_failed",
                 .message   = "JobU storage operation cannot safely continue"},
        Scenario{.operation = StorageOperation::Completion,
                 .code      = "db.constraint.unique",
                 .category  = ErrorCategory::Conflict,
                 .detail    = "operation=completion reason=unexpected_constraint",
                 .message   = "JobU storage operation cannot safely continue"},
        Scenario{.operation = StorageOperation::Read,
                 .code      = "db.corrupt",
                 .category  = ErrorCategory::Internal,
                 .detail    = "operation=read reason=corrupt",
                 .message   = "JobU storage operation cannot safely continue"},
        Scenario{.operation = StorageOperation::Read,
                 .code      = "jobu.storage.invariant",
                 .category  = ErrorCategory::Internal,
                 .detail    = "operation=read reason=durable_invariant",
                 .message   = "JobU storage operation cannot safely continue"},
        Scenario{.operation = StorageOperation::Recovery,
                 .code      = "db.io",
                 .category  = ErrorCategory::Io,
                 .detail    = "operation=recovery reason=recovery_failed",
                 .message   = "JobU storage operation cannot safely continue"},
        Scenario{.operation = StorageOperation::Recovery,
                 .code      = "jobu.recovery.cancelled",
                 .category  = ErrorCategory::Cancelled,
                 .detail    = "operation=recovery reason=recovery_cancelled",
                 .message   = "JobU startup recovery was cancelled"          },
    };

    for (auto const& scenario : scenarios) {
        auto error           = error_with_code(scenario.code, scenario.category);
        error.message        = "SELECT payload-secret FROM credentials-secret";
        error.detail         = "environment-secret output-secret cause=db.constraint.unique";
        auto const original  = error;
        auto const sanitized = sanitized_storage_error(error, scenario.operation);
        CAPTURE(scenario.code, scenario.operation);
        CHECK(error == original);
        CHECK(sanitized.category == original.category);
        CHECK(sanitized.code == original.code);
        CHECK(sanitized.message == scenario.message);
        CHECK(sanitized.detail == scenario.detail);
        CHECK(classify_storage_failure(sanitized, scenario.operation) ==
              classify_storage_failure(original, scenario.operation));
        CHECK(sanitized_storage_error(sanitized, scenario.operation) == sanitized);

        auto const visible = sanitized.code + sanitized.message + sanitized.detail;
        for (auto const* secret :
             {"SELECT", "payload-secret", "credentials-secret", "environment-secret", "output-secret"}) {
            CAPTURE(secret);
            CHECK(visible.find(secret) == std::string::npos);
        }
    }
}
