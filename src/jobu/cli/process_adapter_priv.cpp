#include "process_adapter_priv.hpp"

#include <limits>
#include <memory>
#include <utility>

#include <unistd.h>

#if defined(__linux__)
#  include "process_posix_priv.hpp"
#endif

namespace jb::jobu::cli::detail {

namespace {

#if defined(__linux__)
struct SystemProcessOperationState {
    ProcessOperationId id{0};
    jb::core::Process* process{nullptr};
    ProcessEventSink   sink;
    bool               accepts_events{true};

    void disable_events() noexcept { accepts_events = false; }
};

auto inactive_operation_error() -> jb::core::Error
{
    return {
        .category = jb::core::ErrorCategory::Conflict,
        .code     = "core.process.invalid_state",
        .message  = "The process operation is no longer active",
    };
}

auto operation_identity_error() -> jb::core::Error
{
    return {
        .category = jb::core::ErrorCategory::ResourceExhausted,
        .code     = "core.process.resource_setup_failed",
        .message  = "The process operation identity space is exhausted",
        .detail   = "operation.identity_exhausted",
    };
}

class SystemProcessOperation final : public ProcessOperation {
public:
    explicit SystemProcessOperation(std::shared_ptr<SystemProcessOperationState> state)
        : _state{std::move(state)}
    {}

    ~SystemProcessOperation() override { shutdown(); }

    [[nodiscard]] auto id() const noexcept -> ProcessOperationId override { return _state->id; }

    [[nodiscard]] auto stop(jb::core::ProcessStopReason reason) -> jb::core::Result<void, jb::core::Error> override
    {
        if (_state->process == nullptr) {
            return jb::core::Result<void, jb::core::Error>::failure(inactive_operation_error());
        }
        return _state->process->stop(reason);
    }

    void retire() noexcept override
    {
        // Process emits finished directly. Keep its sender alive until that signal and its readiness callback return.
        _state->disable_events();
        auto* process   = _state->process;
        _state->process = nullptr;
        if (process != nullptr) {
            process->delete_later();
        }
    }

    void shutdown() noexcept override
    {
        // Disable forwarding before Process destruction kills/reaps active work and suppresses its own signals.
        _state->disable_events();
        auto* process   = _state->process;
        _state->process = nullptr;
        delete process;
    }

private:
    std::shared_ptr<SystemProcessOperationState> _state;
};

class SystemProcessAdapter final : public ProcessAdapter {
public:
    explicit SystemProcessAdapter(CliAttemptExecutor&                                owner,
                                  std::shared_ptr<jb::core::priv::ProcessOperations> process_operations = {})
        : _owner{owner}
        , _process_operations{std::move(process_operations)}
    {}

    [[nodiscard]] auto start(jb::core::ProcessStartInfo start_info, ProcessEventSink sink)
        -> jb::core::Result<std::unique_ptr<ProcessOperation>, jb::core::Error> override
    {
        if (_identity_exhausted) {
            return jb::core::Result<std::unique_ptr<ProcessOperation>, jb::core::Error>::failure(
                operation_identity_error());
        }

        auto const operation_id = _next_operation_id;
        if (_next_operation_id == std::numeric_limits<ProcessOperationId>::max()) {
            _identity_exhausted = true;
        }
        else {
            ++_next_operation_id;
        }

        // Guard the parented Process until the operation handle can assume immediate-cleanup responsibility.
        auto process = std::make_unique<jb::core::Process>(&_owner);
        if (_process_operations) {
            // The private seam changes only parent-prepared operations. Process freezes child-callable identity state
            // before creation, so the real production launch path remains under test.
            jb::core::priv::ProcessTestAccess::set_operations(*process, _process_operations);
        }
        auto state = std::make_shared<SystemProcessOperationState>(SystemProcessOperationState{
            .id      = operation_id,
            .process = process.get(),
            .sink    = std::move(sink),
        });

        // The executor is the receiver so Object lifetime tracking deactivates every slot before its private data
        // disappears. Output views borrow the owning signal chunk only for the synchronous sink call.
        process->standard_output.connect(&_owner, [state](jb::core::ByteBuffer const& bytes) {
            if (state->accepts_events && state->sink.standard_output) {
                state->sink.standard_output(state->id, jb::core::ByteView{bytes});
            }
        });
        process->standard_error.connect(&_owner, [state](jb::core::ByteBuffer const& bytes) {
            if (state->accepts_events && state->sink.standard_error) {
                state->sink.standard_error(state->id, jb::core::ByteView{bytes});
            }
        });
        process->finished.connect(&_owner, [state](jb::core::ProcessExit const& exit) {
            if (state->accepts_events && state->sink.finished) {
                // The sink may retire this operation. Do not inspect state after this reentrant call returns.
                state->sink.finished(state->id, exit);
            }
        });

        auto  operation     = std::make_unique<SystemProcessOperation>(state);
        auto* owned_process = process.release();

        auto accepted = owned_process->start(std::move(start_info));
        if (!accepted) {
            auto error = std::move(accepted).error();
            operation->shutdown();
            return jb::core::Result<std::unique_ptr<ProcessOperation>, jb::core::Error>::failure(std::move(error));
        }

        return jb::core::Result<std::unique_ptr<ProcessOperation>, jb::core::Error>::success(std::move(operation));
    }

private:
    CliAttemptExecutor&                                _owner;
    std::shared_ptr<jb::core::priv::ProcessOperations> _process_operations;
    ProcessOperationId                                 _next_operation_id{1};
    bool                                               _identity_exhausted{false};
};
#endif

class SystemEffectiveIdentityProbe final : public EffectiveIdentityProbe {
public:
    [[nodiscard]] auto effective_user_id() const noexcept -> std::uint64_t override
    {
        return static_cast<std::uint64_t>(::geteuid());
    }
};

} // anonymous namespace

auto make_system_process_adapter(CliAttemptExecutor& owner) -> std::unique_ptr<ProcessAdapter>
{
#if defined(__linux__)
    return std::make_unique<SystemProcessAdapter>(owner);
#else
    static_cast<void>(owner);
    return nullptr;
#endif
}

#if defined(__linux__)
auto make_system_process_adapter_for_test(CliAttemptExecutor&                                owner,
                                          std::shared_ptr<jb::core::priv::ProcessOperations> process_operations)
    -> std::unique_ptr<ProcessAdapter>
{
    return std::make_unique<SystemProcessAdapter>(owner, std::move(process_operations));
}
#endif

auto make_system_identity_probe() -> std::unique_ptr<EffectiveIdentityProbe>
{
    return std::make_unique<SystemEffectiveIdentityProbe>();
}

} // namespace jb::jobu::cli::detail
