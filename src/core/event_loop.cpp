#include "event_loop.hpp"

#include "thread_context.hpp"

#include <cassert>
#include <chrono>
#include <limits>

namespace jb::core {

EventLoop::EventLoop()
    : _backend(priv::make_backend())
    , _thread_ctx(ThreadCtx::current())
{}

EventLoop::~EventLoop() = default;

void EventLoop::post(Task task)
{
    {
        std::lock_guard lock{_task_queue_mx};
        _task_queue.push(std::move(task));
    }
    _backend->wakeup();
}

auto EventLoop::post_delayed(Duration delay, Task task) -> TimerHandle
{
    auto h = _timers.start(std::move(task), Clock::now() + delay);
    _backend->wakeup();
    return h;
}

auto EventLoop::post_at(TimePoint when, Task task) -> TimerHandle
{
    auto h = _timers.start(std::move(task), when);
    _backend->wakeup();
    return h;
}

auto EventLoop::post_repeating(Duration interval, Task task) -> TimerHandle
{
    auto h = _timers.start(std::move(task), Clock::now() + interval, interval);
    _backend->wakeup();
    return h;
}

void EventLoop::cancel_timer(TimerHandle h)
{
    _timers.cancel(h);
    _backend->wakeup();
}

void EventLoop::quit()
{
    post([this]() -> void {
        _running.store(false, std::memory_order_relaxed);
    });
}

void EventLoop::run()
{
    static constexpr int kMaxEvents{64};

    // set the thread context for this event loop
    auto* orig_ctx = _thread_ctx.load(std::memory_order_relaxed);
    _thread_ctx.store(ThreadCtx::current(), std::memory_order_relaxed);

    _running.store(true, std::memory_order_relaxed);
    priv::ReadyEvent events[kMaxEvents];

    while (_running.load(std::memory_order_relaxed)) {

        auto timeout_ms = compute_timeout_ms();
        auto n = _backend->poll(events, kMaxEvents, timeout_ms);

        // 1. dispatch fd events
        for (int i = 0; i < n; ++i) {
            dispatch_fd(events[i]);
        }

        // 2. fire expired timers
        _timers.fire_expired(Clock::now());

        // 3. drain the task queue
        drain_task_queue();
    }

    // drain the queue one more time to ensure all the posted tasks are processed before exiting
    drain_task_queue();

    // restore the original thread context (if any)
    _thread_ctx.store(orig_ctx, std::memory_order_relaxed);
}

auto EventLoop::process_events() -> bool
{
    assert(_thread_ctx.load(std::memory_order_relaxed) == ThreadCtx::current());
    if (_thread_ctx.load(std::memory_order_relaxed) != ThreadCtx::current()) {
        return false; // can only be called from the thread running the event loop
    }

    _timers.fire_expired(Clock::now());

    drain_task_queue();

    return _running.load(std::memory_order_relaxed);
}

auto EventLoop::compute_timeout_ms() -> int
{
    auto next = _timers.next_deadline();
    if (!next.has_value()) {
        return -1;
    }

    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(*next - Clock::now()).count();

    if (ms > std::numeric_limits<int>::max()) {
        return std::numeric_limits<int>::max();
    }
    if (ms < 0) {
        return 0;
    }
    return static_cast<int>(ms);
}

void EventLoop::dispatch_fd(priv::ReadyEvent& ev) const
{}

void EventLoop::drain_task_queue()
{
    std::queue<Task> local;
    {
        std::lock_guard lock{_task_queue_mx};
        local.swap(_task_queue);
    }
    while (!local.empty()) {
        local.front()();
        local.pop();
    }
}

} // namespace jb::core
