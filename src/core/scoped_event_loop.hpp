/// @file scoped_event_loop.hpp
/// @brief Lexically scoped event-loop ownership for hosts that do not use Application.
///
#pragma once

#include "error.hpp"
#include "result.hpp"

#include <memory>

namespace jb::core {

class EventLoop;
class ThreadCtx;

namespace priv {
struct ScopedEventLoopTestAccess;
}

/// Owns and binds one event loop to the calling thread without creating a process-global Application.
/// Construct dependent Objects after create(); independent clients can share loop() with separate sessions.
/// Destroy every dependent Object (including pending deferred deletes) before this scope. Queued generic tasks
/// must own their captures or be cancelled by their owners. Destruction drops remaining work without invoking it.
/// A prior dormant loop may be temporarily replaced, but it and its binding must outlive this scope. Nested
/// scopes must be destroyed in reverse order; do not independently replace the thread binding while attached.
/// Creation, use and destruction belong to one thread. Copying and moving are forbidden.
class ScopedEventLoop final {
public:
    /// Creates a valid loop and attaches it to the current thread.
    /// @return Owner, core.event_loop.unavailable (Unavailable) for backend initialization failure, or
    ///         core.event_loop.reentrant_wait (Conflict) when called during a wait or event-loop dispatch.
    [[nodiscard]] static auto create() -> Result<std::unique_ptr<ScopedEventLoop>, Error>;

    /// Restores the previous thread binding and releases the loop without dispatching callbacks.
    ~ScopedEventLoop();

    ScopedEventLoop(ScopedEventLoop const&)                    = delete;
    ScopedEventLoop(ScopedEventLoop&&)                         = delete;
    auto operator=(ScopedEventLoop const&) -> ScopedEventLoop& = delete;
    auto operator=(ScopedEventLoop&&) -> ScopedEventLoop&      = delete;

    /// Returns the owned loop; include event_loop.hpp to drive it with run_until().
    [[nodiscard]] auto loop() const noexcept -> EventLoop& { return *_loop; }

private:
    friend struct priv::ScopedEventLoopTestAccess;

    explicit ScopedEventLoop(std::unique_ptr<EventLoop> loop);
    static auto attach(std::unique_ptr<EventLoop> loop) -> Result<std::unique_ptr<ScopedEventLoop>, Error>;

    std::unique_ptr<EventLoop> _loop;
    ThreadCtx*                 _context;
    EventLoop*                 _previous;
};

} // namespace jb::core
