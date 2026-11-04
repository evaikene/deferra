#include "scoped_event_loop.hpp"

#include "event_loop.hpp"
#include "thread_context.hpp"

#include <utility>

namespace jb::core {

auto ScopedEventLoop::create() -> Result<std::unique_ptr<ScopedEventLoop>, Error>
{
    // Reject callback reentrancy before allocating a native backend or changing the host's binding.
    if (EventLoop::s_dispatching) {
        return Result<std::unique_ptr<ScopedEventLoop>, Error>::failure({
            .category = ErrorCategory::Conflict,
            .code     = "core.event_loop.reentrant_wait",
            .message  = "Cannot attach an event loop during event dispatch or a synchronous wait",
        });
    }
    return attach(std::make_unique<EventLoop>());
}

auto ScopedEventLoop::attach(std::unique_ptr<EventLoop> loop) -> Result<std::unique_ptr<ScopedEventLoop>, Error>
{
    if (!loop->is_valid()) {
        return Result<std::unique_ptr<ScopedEventLoop>, Error>::failure({
            .category = ErrorCategory::Unavailable,
            .code     = "core.event_loop.unavailable",
            .message  = "Cannot initialize the event-loop backend",
        });
    }
    return Result<std::unique_ptr<ScopedEventLoop>, Error>::success(
        std::unique_ptr<ScopedEventLoop>{new ScopedEventLoop(std::move(loop))});
}

ScopedEventLoop::ScopedEventLoop(std::unique_ptr<EventLoop> loop)
    : _loop(std::move(loop))
    , _context(ThreadCtx::current())
    , _previous(_context->event_loop())
{
    _context->set_event_loop(_loop.get());
}

ScopedEventLoop::~ScopedEventLoop()
{
    // All dependent objects are already gone. Never run arbitrary callbacks during host/request teardown.
    _context->set_event_loop(_previous);
}

} // namespace jb::core
