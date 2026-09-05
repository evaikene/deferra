#pragma once

#include "object_priv.hpp"
#include "process.hpp"

#if defined(__linux__)
#  include "process_posix_priv.hpp"
#  include "process_request_priv.hpp"

#  include <cstddef>
#  include <memory>
#endif

namespace jb::core {

/// Extends the single Object-owned allocation; the owner is bound only after Object construction.
struct Process::Private : priv::ObjectPrivate {
    Process*     owner{nullptr};
    ProcessState state{ProcessState::NotRunning};
#if defined(__linux__)
    /// Each registration gets a new anchor. Retiring a run never makes an old weak callback valid again.
    struct Anchor {
        Private*      data;
        std::uint64_t generation;
    };

    std::shared_ptr<priv::ProcessOperations>      operations{std::make_shared<priv::ProcessOperations>()};
    std::unique_ptr<priv::PreparedProcessRequest> request;
    priv::ProcessDescriptors                      descriptors;
    std::shared_ptr<Anchor>                       status_anchor;
    std::shared_ptr<Anchor>                       process_anchor;
    std::uint64_t                                 generation{0};
    pid_t                                         pid{-1};
    bool                                          group_established{false};
    bool                                          process_watched{false};
    FdWatch                                       status_watch;
    bool                                          status_resolved{false};
    bool                                          gate_released{false};
    bool                                          reaped{false};
    priv::ProcessChildError                       child_error;
    std::size_t                                   status_bytes{0};
    ProcessExit                                   exit;

    /// @throws std::exception from an injected parent-side test adapter; rolls back before propagation.
    auto launch() -> Result<void, Error>;
    void read_status();
    void child_ready();
    void finish_if_ready();
    void retire_status();
    void retire_process();
    /// Destructor and unwind boundary: cleanup cannot propagate a second exception.
    void cleanup() noexcept;
#endif
};

} // namespace jb::core
