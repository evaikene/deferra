#pragma once

#include "object_priv.hpp"
#include "process.hpp"

#if defined(__linux__) || defined(__APPLE__)
#  include "process_posix_priv.hpp"
#  include "process_request_priv.hpp"

#  include <array>
#  include <cstddef>
#  include <memory>
#endif

namespace jb::core {

/// Extends the single Object-owned allocation; the owner is bound only after Object construction.
struct Process::Private : priv::ObjectPrivate {
    Process*     owner{nullptr};
    ProcessState state{ProcessState::NotRunning};
#if defined(__linux__) || defined(__APPLE__)
    /// Each registration gets a new anchor. Retiring a run never makes an old weak callback valid again.
    struct Anchor {
        Private*      data;
        std::uint64_t generation;
    };

    struct OutputChannel {
        std::shared_ptr<Anchor> anchor;
        FdWatch                 watch;
        bool                    terminal{true};
        bool                    continuation_pending{false};
        bool                    draining{false};
        bool                    final_drain_pending{false};
    };

    static constexpr std::size_t kPipeReadChunkBytes{std::size_t{64} * 1024};
    static constexpr std::size_t kPipeReadBudgetBytes{std::size_t{256} * 1024};
    static constexpr Duration    kPostReapDrainTimeout{std::chrono::seconds{1}};
    std::array<OutputChannel, 2> channels;

    std::shared_ptr<priv::ProcessOperations>      operations{std::make_shared<priv::ProcessOperations>()};
    std::unique_ptr<priv::PreparedProcessRequest> request;
    priv::ProcessDescriptors                      descriptors;
    std::shared_ptr<Anchor>                       status_anchor;
    std::shared_ptr<Anchor>                       process_anchor;
    std::uint64_t                                 generation{0};
    pid_t                                         pid{-1};
    pid_t                                         process_group{-1};
    bool                                          process_watched{false};
    FdWatch                                       status_watch;
    bool                                          status_resolved{false};
    bool                                          gate_released{false};
    bool                                          reaped{false};
    bool                                          leader_exit_observed{false};
    bool                                          group_kill_attempted{false};
    std::optional<ProcessExitKind>                stop_kind;
    TimerHandle                                   timeout_timer;
    TimerHandle                                   termination_timer;
    TimerHandle                                   post_reap_timer;
    TimePoint                                     termination_deadline;
    priv::ProcessChildError                       child_error;
    std::size_t                                   status_bytes{0};
    ProcessExit                                   exit;

    /// @throws std::exception from an injected parent-side test adapter; rolls back before propagation.
    auto launch() -> Result<void, Error>;
    auto stop(ProcessStopReason reason) -> Result<void, Error>;
    void read_status();
    void output_ready(std::size_t index);
    void drain_output(std::size_t index, bool final_drain = false);
    void invalidate_output_work(std::size_t index);
    void retire_output(std::size_t index);
    void timeout_expired();
    void termination_grace_expired();
    void post_reap_drain_expired();
    void begin_stopping(ProcessExitKind kind);
    void attempt_group_kill(char const* stage) noexcept;
    void reap_child(int options);
    void enter_finishing(int status);
    void cancel_timer(TimerHandle& timer) noexcept;
    void child_ready();
    void finish_if_ready();
    void retire_status();
    void retire_process();
    /// Destructor and unwind boundary: cleanup cannot propagate a second exception.
    void cleanup() noexcept;
#endif
};

} // namespace jb::core
