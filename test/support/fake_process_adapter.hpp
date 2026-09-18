/** @file fake_process_adapter.hpp
 * @brief Defines deterministic process-adapter and effective-identity seams for CLI executor tests.
 */
#pragma once

#include "cli/process_adapter_priv.hpp"
#include "error.hpp"
#include "result.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace jb::test {

struct FakeProcessAdapterObservation {
    struct StartRecord {
        jobu::cli::detail::ProcessOperationId id{0};
        core::ProcessStartInfo                start_info;
    };

    struct StopRecord {
        jobu::cli::detail::ProcessOperationId id{0};
        core::ProcessStopReason               reason{core::ProcessStopReason::Cancelled};
    };

    std::vector<StartRecord>                           starts;
    std::vector<StopRecord>                            stops;
    std::vector<jobu::cli::detail::ProcessOperationId> retired;
    std::vector<jobu::cli::detail::ProcessOperationId> shutdown;
};

/** Test-only adapter whose owner thread explicitly drives every operation event. */
class FakeProcessAdapter final : public jobu::cli::detail::ProcessAdapter {
public:
    FakeProcessAdapter();

    void set_start_error(std::optional<core::Error> error);
    void set_stop_error(std::optional<core::Error> error);
    void set_next_operation_id(jobu::cli::detail::ProcessOperationId id) noexcept;

    [[nodiscard]] auto observation() const noexcept -> std::shared_ptr<FakeProcessAdapterObservation> const&;
    [[nodiscard]] auto pending_operation_ids() const -> std::vector<jobu::cli::detail::ProcessOperationId>;
    [[nodiscard]] auto snapshot_sink(jobu::cli::detail::ProcessOperationId id) const
        -> std::optional<jobu::cli::detail::ProcessEventSink>;

    [[nodiscard]] auto
    emit_standard_output(jobu::cli::detail::ProcessOperationId                id,
                         core::ByteView                                       bytes,
                         std::optional<jobu::cli::detail::ProcessOperationId> reported_id = std::nullopt)
        -> core::Result<void, core::Error>;
    [[nodiscard]] auto
    emit_standard_error(jobu::cli::detail::ProcessOperationId                id,
                        core::ByteView                                       bytes,
                        std::optional<jobu::cli::detail::ProcessOperationId> reported_id = std::nullopt)
        -> core::Result<void, core::Error>;
    [[nodiscard]] auto finish(jobu::cli::detail::ProcessOperationId                id,
                              core::ProcessExit                                    exit,
                              std::optional<jobu::cli::detail::ProcessOperationId> reported_id = std::nullopt)
        -> core::Result<void, core::Error>;

    [[nodiscard]] auto start(core::ProcessStartInfo start_info, jobu::cli::detail::ProcessEventSink sink)
        -> core::Result<std::unique_ptr<jobu::cli::detail::ProcessOperation>, core::Error> override;

private:
    class Operation;
    struct PendingOperation;

    [[nodiscard]] auto find_pending(jobu::cli::detail::ProcessOperationId id) const
        -> std::shared_ptr<PendingOperation>;
    [[nodiscard]] auto stop_operation(std::shared_ptr<PendingOperation> const& operation,
                                      core::ProcessStopReason reason) -> core::Result<void, core::Error>;
    void               retire_operation(std::shared_ptr<PendingOperation> const& operation) noexcept;
    void               shutdown_operation(std::shared_ptr<PendingOperation> const& operation) noexcept;

    std::shared_ptr<FakeProcessAdapterObservation> _observation;
    jobu::cli::detail::ProcessOperationId          _next_operation_id{1};
    std::optional<core::Error>                     _start_error;
    std::optional<core::Error>                     _stop_error;
    std::vector<std::shared_ptr<PendingOperation>> _pending;
};

/** Mutable deterministic effective-identity source. */
class FakeEffectiveIdentityProbe final : public jobu::cli::detail::EffectiveIdentityProbe {
public:
    explicit FakeEffectiveIdentityProbe(std::uint64_t effective_user_id = 1000U) noexcept;

    void set_effective_user_id(std::uint64_t effective_user_id) noexcept;
    void set_sequence(std::vector<std::uint64_t> sequence);

    [[nodiscard]] auto call_count() const noexcept -> std::size_t;
    [[nodiscard]] auto effective_user_id() const noexcept -> std::uint64_t override;

private:
    std::uint64_t              _effective_user_id{1000U};
    std::vector<std::uint64_t> _sequence;
    mutable std::size_t        _next_sequence_value{0};
    mutable std::size_t        _call_count{0};
};

} // namespace jb::test
