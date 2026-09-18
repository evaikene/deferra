#pragma once

#include "byte_buffer.hpp"
#include "cli_attempt_executor.hpp"
#include "error.hpp"
#include "process.hpp"
#include "result.hpp"

#include <cstdint>
#include <functional>
#include <memory>

namespace jb::core::priv {
class ProcessOperations;
}

namespace jb::jobu::cli::detail {

using ProcessOperationId = std::uint64_t;

/// Owner-thread event sinks installed before an adapter accepts an operation.
struct ProcessEventSink {
    std::function<void(ProcessOperationId, jb::core::ByteView)>    standard_output;
    std::function<void(ProcessOperationId, jb::core::ByteView)>    standard_error;
    std::function<void(ProcessOperationId, jb::core::ProcessExit)> finished;
};

/// Private handle for one accepted process-like operation.
class ProcessOperation {
public:
    virtual ~ProcessOperation() = default;

    [[nodiscard]] virtual auto id() const noexcept -> ProcessOperationId                                           = 0;
    [[nodiscard]] virtual auto stop(jb::core::ProcessStopReason reason) -> jb::core::Result<void, jb::core::Error> = 0;

    /// Retires an already-terminal operation and permanently disables its event sinks.
    virtual void retire() noexcept   = 0;
    /// Immediately cleans up active work and permanently disables its event sinks without a completion event.
    virtual void shutdown() noexcept = 0;
};

/// Private strategy that starts independently identifiable process-like operations.
class ProcessAdapter {
public:
    virtual ~ProcessAdapter() = default;

    /// Attempts to accept one prepared Process request.
    /// @param start_info Complete owning request with no ambient-environment dependency.
    /// @param sink Complete event sink installed before the operation can report events.
    /// @return A non-null, positive-ID operation handle, or a safe `core.process.*` rejection.
    /// @warning Event sinks must not be invoked from inside this call.
    ///
    [[nodiscard]] virtual auto start(jb::core::ProcessStartInfo start_info, ProcessEventSink sink)
        -> jb::core::Result<std::unique_ptr<ProcessOperation>, jb::core::Error> = 0;
};

/// Private effective-identity seam used for deterministic root-policy checks.
class EffectiveIdentityProbe {
public:
    virtual ~EffectiveIdentityProbe()                                              = default;
    [[nodiscard]] virtual auto effective_user_id() const noexcept -> std::uint64_t = 0;
};

/// Creates the platform production adapter after the executor owner is fully constructed.
/// @return A Linux Process-backed adapter, or null until the current platform backend is integrated.
///
[[nodiscard]] auto make_system_process_adapter(CliAttemptExecutor& owner) -> std::unique_ptr<ProcessAdapter>;

#if defined(__linux__)
/// Creates the production adapter with parent-side Process operations injected before each launch.
[[nodiscard]] auto
make_system_process_adapter_for_test(CliAttemptExecutor&                                owner,
                                     std::shared_ptr<jb::core::priv::ProcessOperations> process_operations)
    -> std::unique_ptr<ProcessAdapter>;
#endif

[[nodiscard]] auto make_system_identity_probe() -> std::unique_ptr<EffectiveIdentityProbe>;

/// Private construction access for deterministic executor-only tests.
struct CliAttemptExecutorTestAccess {
    [[nodiscard]] static auto create(CliAttemptExecutorOptions               options,
                                     std::unique_ptr<ProcessAdapter>         adapter,
                                     std::unique_ptr<EffectiveIdentityProbe> identity)
        -> std::unique_ptr<CliAttemptExecutor>;

#if defined(__linux__)
    /// Creates an executor with the production adapter and injected child-identity/process operations.
    [[nodiscard]] static auto
    create_with_system_process_adapter(CliAttemptExecutorOptions                          options,
                                       std::unique_ptr<EffectiveIdentityProbe>            identity,
                                       std::shared_ptr<jb::core::priv::ProcessOperations> process_operations)
        -> std::unique_ptr<CliAttemptExecutor>;
#endif
};

} // namespace jb::jobu::cli::detail
