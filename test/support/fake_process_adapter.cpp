#include "fake_process_adapter.hpp"

#include <algorithm>
#include <limits>
#include <string>
#include <utility>

namespace jb::test {

namespace {

template <typename T = void>
using FakeResult = core::Result<T, core::Error>;

auto fake_error(core::ErrorCategory category, std::string code, std::string message) -> core::Error
{
    return {
        .category = category,
        .code     = std::move(code),
        .message  = std::move(message),
    };
}

} // anonymous namespace

struct FakeProcessAdapter::PendingOperation {
    jobu::cli::detail::ProcessOperationId id{0};
    jobu::cli::detail::ProcessEventSink   sink;
    bool                                  active{true};
};

class FakeProcessAdapter::Operation final : public jobu::cli::detail::ProcessOperation {
public:
    Operation(FakeProcessAdapter& owner, std::shared_ptr<PendingOperation> operation)
        : _owner{owner}
        , _operation{std::move(operation)}
    {}

    [[nodiscard]] auto id() const noexcept -> jobu::cli::detail::ProcessOperationId override { return _operation->id; }

    [[nodiscard]] auto stop(core::ProcessStopReason reason) -> FakeResult<> override
    {
        return _owner.stop_operation(_operation, reason);
    }

    void retire() noexcept override { _owner.retire_operation(_operation); }

    void shutdown() noexcept override { _owner.shutdown_operation(_operation); }

private:
    FakeProcessAdapter&               _owner;
    std::shared_ptr<PendingOperation> _operation;
};

FakeProcessAdapter::FakeProcessAdapter()
    : _observation{std::make_shared<FakeProcessAdapterObservation>()}
{}

void FakeProcessAdapter::set_start_error(std::optional<core::Error> error)
{
    _start_error = std::move(error);
}

void FakeProcessAdapter::set_stop_error(std::optional<core::Error> error)
{
    _stop_error = std::move(error);
}

void FakeProcessAdapter::set_next_operation_id(jobu::cli::detail::ProcessOperationId id) noexcept
{
    _next_operation_id = id;
}

auto FakeProcessAdapter::observation() const noexcept -> std::shared_ptr<FakeProcessAdapterObservation> const&
{
    return _observation;
}

auto FakeProcessAdapter::pending_operation_ids() const -> std::vector<jobu::cli::detail::ProcessOperationId>
{
    auto ids = std::vector<jobu::cli::detail::ProcessOperationId>{};
    for (auto const& pending : _pending) {
        if (pending->active) {
            ids.push_back(pending->id);
        }
    }
    return ids;
}

auto FakeProcessAdapter::snapshot_sink(jobu::cli::detail::ProcessOperationId id) const
    -> std::optional<jobu::cli::detail::ProcessEventSink>
{
    auto operation = find_pending(id);
    return operation ? std::optional{operation->sink} : std::nullopt;
}

auto FakeProcessAdapter::emit_standard_output(jobu::cli::detail::ProcessOperationId                id,
                                              core::ByteView                                       bytes,
                                              std::optional<jobu::cli::detail::ProcessOperationId> reported_id)
    -> FakeResult<>
{
    auto operation = find_pending(id);
    if (!operation) {
        return FakeResult<>::failure(fake_error(core::ErrorCategory::NotFound,
                                                "test.process_adapter.operation_not_found",
                                                "The fake process operation is not active"));
    }
    operation->sink.standard_output(reported_id.value_or(id), bytes);
    return FakeResult<>::success();
}

auto FakeProcessAdapter::emit_standard_error(jobu::cli::detail::ProcessOperationId                id,
                                             core::ByteView                                       bytes,
                                             std::optional<jobu::cli::detail::ProcessOperationId> reported_id)
    -> FakeResult<>
{
    auto operation = find_pending(id);
    if (!operation) {
        return FakeResult<>::failure(fake_error(core::ErrorCategory::NotFound,
                                                "test.process_adapter.operation_not_found",
                                                "The fake process operation is not active"));
    }
    operation->sink.standard_error(reported_id.value_or(id), bytes);
    return FakeResult<>::success();
}

auto FakeProcessAdapter::finish(jobu::cli::detail::ProcessOperationId                id,
                                core::ProcessExit                                    exit,
                                std::optional<jobu::cli::detail::ProcessOperationId> reported_id) -> FakeResult<>
{
    auto operation = find_pending(id);
    if (!operation) {
        return FakeResult<>::failure(fake_error(core::ErrorCategory::NotFound,
                                                "test.process_adapter.operation_not_found",
                                                "The fake process operation is not active"));
    }
    operation->sink.finished(reported_id.value_or(id), std::move(exit));
    return FakeResult<>::success();
}

auto FakeProcessAdapter::start(core::ProcessStartInfo start_info, jobu::cli::detail::ProcessEventSink sink)
    -> FakeResult<std::unique_ptr<jobu::cli::detail::ProcessOperation>>
{
    if (_start_error) {
        return FakeResult<std::unique_ptr<jobu::cli::detail::ProcessOperation>>::failure(*_start_error);
    }

    auto const operation_id = _next_operation_id;
    if (_next_operation_id != std::numeric_limits<jobu::cli::detail::ProcessOperationId>::max()) {
        ++_next_operation_id;
    }
    _observation->starts.push_back({.id = operation_id, .start_info = std::move(start_info)});
    auto pending = std::make_shared<PendingOperation>(PendingOperation{
        .id   = operation_id,
        .sink = std::move(sink),
    });
    _pending.push_back(pending);
    return FakeResult<std::unique_ptr<jobu::cli::detail::ProcessOperation>>::success(
        std::make_unique<Operation>(*this, std::move(pending)));
}

auto FakeProcessAdapter::find_pending(jobu::cli::detail::ProcessOperationId id) const
    -> std::shared_ptr<PendingOperation>
{
    auto const found = std::ranges::find_if(_pending, [id](std::shared_ptr<PendingOperation> const& operation) {
        return operation->active && operation->id == id;
    });
    return found == _pending.end() ? nullptr : *found;
}

auto FakeProcessAdapter::stop_operation(std::shared_ptr<PendingOperation> const& operation,
                                        core::ProcessStopReason                  reason) -> FakeResult<>
{
    _observation->stops.push_back({.id = operation->id, .reason = reason});
    if (_stop_error) {
        return FakeResult<>::failure(*_stop_error);
    }
    if (!operation->active) {
        return FakeResult<>::failure(fake_error(core::ErrorCategory::NotFound,
                                                "core.process.invalid_state",
                                                "The fake process operation is not active"));
    }
    return FakeResult<>::success();
}

void FakeProcessAdapter::retire_operation(std::shared_ptr<PendingOperation> const& operation) noexcept
{
    if (!operation->active) {
        return;
    }
    operation->active = false;
    operation->sink   = {};
    _observation->retired.push_back(operation->id);
}

void FakeProcessAdapter::shutdown_operation(std::shared_ptr<PendingOperation> const& operation) noexcept
{
    if (!operation->active) {
        return;
    }
    operation->active = false;
    operation->sink   = {};
    _observation->shutdown.push_back(operation->id);
}

FakeEffectiveIdentityProbe::FakeEffectiveIdentityProbe(std::uint64_t effective_user_id) noexcept
    : _effective_user_id{effective_user_id}
{}

void FakeEffectiveIdentityProbe::set_effective_user_id(std::uint64_t effective_user_id) noexcept
{
    _effective_user_id = effective_user_id;
    _sequence.clear();
    _next_sequence_value = 0;
}

void FakeEffectiveIdentityProbe::set_sequence(std::vector<std::uint64_t> sequence)
{
    _sequence            = std::move(sequence);
    _next_sequence_value = 0;
}

auto FakeEffectiveIdentityProbe::call_count() const noexcept -> std::size_t
{
    return _call_count;
}

auto FakeEffectiveIdentityProbe::effective_user_id() const noexcept -> std::uint64_t
{
    ++_call_count;
    if (_sequence.empty()) {
        return _effective_user_id;
    }
    auto const index = std::min(_next_sequence_value, _sequence.size() - 1U);
    ++_next_sequence_value;
    return _sequence[index];
}

} // namespace jb::test
