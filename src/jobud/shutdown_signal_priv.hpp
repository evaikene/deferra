#pragma once

#include "error.hpp"
#include "result.hpp"

#include <functional>
#include <memory>

namespace jb::core {
class EventLoop;
}

namespace jb::jobud::detail {

struct ShutdownSignalOperations;
struct ShutdownSignalTestAccess;
class ShutdownSignalWatch;

/// Process-main SIGTERM/SIGINT ownership, installed before creating any worker threads.
///
/// This is daemon infrastructure, not an embeddable signal API. Install, poll, attach, and close on
/// the main thread. Installation unblocks these two signals on that thread; subsequently created
/// workers deliberately inherit that mask and may execute the handler. All such workers must be
/// joined before close/destruction: blocking only the main thread cannot retire a live handler.
/// Other signal dispositions, including SIGCHLD, are untouched.
///
/// The request is monotonic for the process lifetime. Poll before entering the event loop and use
/// requested() as recovery's borrowed stop predicate. A pre-run EventLoop exit request is not sticky.
/// Destroy the watch before its loop, and keep this relay alive through watch and worker teardown.
class ShutdownSignalRelay final {
public:

    /// Saves dispositions/mask and creates a nonblocking CLOEXEC pipe. Failure unwinds partial setup.
    /// Returns safe jobud.signal.setup errors; another live relay is rejected with jobud.signal.active.
    [[nodiscard]] static auto install() -> jb::core::Result<std::unique_ptr<ShutdownSignalRelay>, jb::core::Error>;
    ~ShutdownSignalRelay();

    ShutdownSignalRelay(ShutdownSignalRelay const&)                    = delete;
    auto operator=(ShutdownSignalRelay const&) -> ShutdownSignalRelay& = delete;

    [[nodiscard]] auto requested() const noexcept -> bool;

    /// Attaches one owner-thread notification, coalescing all requests into at most one delivery.
    /// The notification must latch runtime gates and request exit, without destroying the relay,
    /// watch, or borrowed callback targets on its own stack. An empty callback or second watch fails.
    /// Callback targets must outlive the watch. No notification occurs synchronously during attach.
    [[nodiscard]] auto attach(jb::core::EventLoop& loop, std::function<void()> notify)
        -> jb::core::Result<std::unique_ptr<ShutdownSignalWatch>, jb::core::Error>;

    /// After watch destruction and worker joins, restores dispositions/mask and closes descriptors.
    /// Idempotent. A restoration failure returns jobud.signal.cleanup and retains handler resources
    /// where necessary for a safe retry; destruction logs an unrecovered cleanup error.
    [[nodiscard]] auto close() -> jb::core::Result<void, jb::core::Error>;

private:

    friend struct ShutdownSignalTestAccess;
    struct State;
    explicit ShutdownSignalRelay(std::unique_ptr<State> state);
    static auto install(ShutdownSignalOperations const& operations)
        -> jb::core::Result<std::unique_ptr<ShutdownSignalRelay>, jb::core::Error>;

    std::unique_ptr<State> _state;
};

/// Owner-thread watch scope, nested inside both the EventLoop and relay lifetimes.
/// Destruction invalidates retained callbacks before removing the native watch, even if removal fails.
class ShutdownSignalWatch final {
public:

    ~ShutdownSignalWatch();
    ShutdownSignalWatch(ShutdownSignalWatch const&)                    = delete;
    auto operator=(ShutdownSignalWatch const&) -> ShutdownSignalWatch& = delete;

private:

    friend class ShutdownSignalRelay;
    friend struct ShutdownSignalTestAccess;
    struct State;
    explicit ShutdownSignalWatch(std::shared_ptr<State> state);

    std::shared_ptr<State> _state;
};

} // namespace jb::jobud::detail
