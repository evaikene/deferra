#include "event_loop.hpp"

#include "thread_context.hpp"

#include <cassert>
#include <chrono>
#include <limits>

namespace jb::core {

EventLoop::EventLoop()
    : _thread_ctx(ThreadCtx::current())
    , _backend(priv::make_backend())
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
    if (!assert_on_loop_thread()) {
        return {}; // can only be called from the thread running the event loop
    }

    auto h = _timers.start(std::move(task), Clock::now() + delay);
    _backend->wakeup();
    return h;
}

auto EventLoop::post_at(TimePoint when, Task task) -> TimerHandle
{
    if (!assert_on_loop_thread()) {
        return {}; // can only be called from the thread running the event loop
    }

    auto h = _timers.start(std::move(task), when);
    _backend->wakeup();
    return h;
}

auto EventLoop::post_repeating(Duration interval, Task task) -> TimerHandle
{
    if (!assert_on_loop_thread()) {
        return {}; // can only be called from the thread running the event loop
    }

    auto h = _timers.start(std::move(task), Clock::now() + interval, interval);
    _backend->wakeup();
    return h;
}

void EventLoop::cancel_timer(TimerHandle h)
{
    if (!assert_on_loop_thread()) {
        return; // can only be called from the thread running the event loop
    }

    _timers.cancel(h);
    _backend->wakeup();
}

auto EventLoop::watch_fd(int fd, FdEvents events, FdCallback callback) -> FdWatch
{
    if (!assert_on_loop_thread()) {
        return {};
    }

    _watchers[fd] = { .callback=std::move(callback), .events=events };
    _backend->add_fd(fd, events);

    return { fd };
}

void EventLoop::unwatch_fd(FdWatch handle)
{
    if (!assert_on_loop_thread()) {
        return;
    }

    _backend->remove_fd(handle.fd);
    _watchers.erase(handle.fd);
}

void EventLoop::quit()
{
    post([this]() -> void {
        _running.store(false, std::memory_order_relaxed);
    });
}

void EventLoop::run()
{
    // set the thread context for this event loop
    auto* orig_ctx = _thread_ctx.load(std::memory_order_relaxed);
    _thread_ctx.store(ThreadCtx::current(), std::memory_order_relaxed);

    _running.store(true, std::memory_order_relaxed);

    while (process_events(EventFlag::All)) {}

    // drain the queue one more time to ensure all the posted tasks are processed before exiting
    drain_task_queue();

    // restore the original thread context (if any)
    _thread_ctx.store(orig_ctx, std::memory_order_relaxed);
}

auto EventLoop::process_events(EventFlags flags) -> bool
{
    return process_events(flags, -1);
}

auto EventLoop::process_events(EventFlags flags, int ms) -> bool
{
    if (!assert_on_loop_thread()) {
        return false; // can only be called from the thread running the event loop
    }

    // 1. poll and dispatch fd events
    if (flags.test(EventFlag::Watchers)) {
        static constexpr int kMaxEvents{64};
        priv::ReadyEvent events[kMaxEvents];

        auto timeout_ms = compute_timeout_ms(ms);
        auto n = _backend->poll(events, kMaxEvents, timeout_ms);

        for (int i = 0; i < n; ++i) {
            dispatch_fd(events[i]);
        }
    }

    // 2. fire expired timers
    if (flags.test(EventFlag::Timers)) {
        _timers.fire_expired(Clock::now());
    }

    // 3. drain the task queue
    if (flags.test(EventFlag::Tasks)) {
        drain_task_queue();
    }

    return _running.load(std::memory_order_relaxed);
}

auto EventLoop::assert_on_loop_thread() const -> bool
{
    assert(_thread_ctx.load(std::memory_order_relaxed) == ThreadCtx::current());
    return (_thread_ctx.load(std::memory_order_relaxed) == ThreadCtx::current());
}

auto EventLoop::compute_timeout_ms(int max_timeout_ms) -> int
{
    auto next = _timers.next_deadline();
    if (!next.has_value()) {
        return max_timeout_ms;
    }

    if (max_timeout_ms < 0) {
        max_timeout_ms = std::numeric_limits<int>::max();
    }

    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(*next - Clock::now()).count();

    if (ms > std::numeric_limits<int>::max()) {
        return std::min(std::numeric_limits<int>::max(), max_timeout_ms);
    }
    if (ms < 0) {
        return 0;
    }
    return std::min(static_cast<int>(ms), max_timeout_ms);
}

void EventLoop::dispatch_fd(priv::ReadyEvent const& ev) const
{
    auto it = _watchers.find(ev.fd);
    if (it != _watchers.end()) {
        it->second.callback(ev.fd, ev.events);
    }
}

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
