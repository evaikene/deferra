#include "process.hpp"

#include "event_loop.hpp"
#include "process_priv.hpp"
#include "process_request_priv.hpp"

#include <unistd.h>

#include <utility>

namespace jb::core {

Process::Process(Object* parent)
    : Object(*new Private, parent)
{
    // Bind only after Object owns the block and has established parenting and affinity.
    d_ptr<Private>()->owner = this;
}

Process::~Process() = default;

auto Process::start(ProcessStartInfo start_info) -> Result<void, Error>
{
    if (state() != ProcessState::NotRunning) {
        return Result<void, Error>::failure({.category = ErrorCategory::Conflict,
                                             .code     = "core.process.invalid_state",
                                             .message  = "Process is not idle"});
    }
    auto* loop = event_loop();
    if (!loop || loop != EventLoop::current() || !loop->is_valid()) {
        return Result<void, Error>::failure({.category = ErrorCategory::Unavailable,
                                             .code     = "core.process.event_loop_unavailable",
                                             .message  = "Process requires a valid current owner EventLoop"});
    }
    // Capture once before preparation; future launch stages retain this exact checked absolute deadline.
    auto const launch_time = Clock::now();
    auto       prepared    = priv::prepare_process_request(std::move(start_info), launch_time, sysconf(_SC_ARG_MAX));
    if (!prepared) {
        return Result<void, Error>::failure(prepared.error());
    }
    auto signal_configuration = priv::validate_process_signal_configuration();
    if (!signal_configuration) {
        return signal_configuration;
    }
    // Stage 6.1 fixes policy without accepting work or creating native resources before backend implementation.
    return Result<void, Error>::failure({.category = ErrorCategory::Unsupported,
                                         .code     = "core.process.monitor_unsupported",
                                         .message  = "Process execution backend is unavailable",
                                         .detail   = "backend.not_implemented"});
}

auto Process::stop(ProcessStopReason /*reason*/) -> Result<void, Error>
{
    return Result<void, Error>::failure({.category = ErrorCategory::Conflict,
                                         .code     = "core.process.invalid_state",
                                         .message  = "Process has no accepted operation"});
}

auto Process::state() const noexcept -> ProcessState
{
    return d_ptr<Private const>()->state;
}

auto Process::process_id() const noexcept -> std::optional<std::int64_t>
{
    return std::nullopt;
}

} // namespace jb::core
