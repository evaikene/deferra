#include "event_loop.hpp"

#include "application.hpp"
#include "event.hpp"
#include "logging.hpp"
#include "object_priv.hpp"
#include "thread_context.hpp"

#include <chrono>
#include <limits>

namespace jb::core {

auto EventLoop::current() noexcept -> EventLoop*
{
    return ThreadCtx::current()->event_loop();
}

EventLoop::EventLoop()
    : EventLoop(priv::make_backend())
{}

EventLoop::EventLoop(std::unique_ptr<priv::Backend> backend)
    : _thread_ctx(ThreadCtx::current())
    , _backend(std::move(backend))
{}

EventLoop::~EventLoop() = default;

auto EventLoop::is_valid() const noexcept -> bool
{
    return _backend != nullptr;
}

auto EventLoop::post(Task task) -> bool
{
    std::lock_guard lock{_task_queue_mx};
    // Wake before insertion while holding the queue lock so a consumer cannot
    // pass the queue before the successfully signalled entry is visible.
    if (!_backend || !_backend->wakeup()) {
        return false;
    }
    _task_queue.push(std::move(task));
    return true;
}

auto EventLoop::post_event(Object* receiver, std::weak_ptr<priv::ObjectLifetime> lifetime, std::unique_ptr<Event> event)
    -> bool
{
    std::lock_guard lock{_event_queue_mx};
    // Wake before insertion while holding the queue lock so a consumer cannot
    // pass the queue before the successfully signalled entry is visible.
    if (!_backend || !_backend->wakeup()) {
        return false;
    }
    _event_queue.push({receiver, std::move(lifetime), std::move(event), {}});
    return true;
}

auto EventLoop::post_event_delivery(Object* receiver, std::weak_ptr<priv::ObjectLifetime> lifetime, Task delivery)
    -> bool
{
    std::lock_guard lock{_event_queue_mx};
    // Wake before insertion while holding the queue lock so a consumer cannot
    // pass the queue before the successfully signalled entry is visible.
    if (!_backend || !_backend->wakeup()) {
        return false;
    }
    _event_queue.push({receiver, std::move(lifetime), {}, std::move(delivery)});
    return true;
}

auto EventLoop::defer_delete(Object* object, std::weak_ptr<priv::ObjectLifetime> lifetime) -> bool
{
    std::lock_guard lock{_deferred_delete_queue_mx};
    // Wake before insertion while holding the queue lock so a consumer cannot
    // pass the queue before the successfully signalled entry is visible.
    if (!_backend || !_backend->wakeup()) {
        return false;
    }
    _deferred_delete_queue.push({object, std::move(lifetime)});
    return true;
}

auto EventLoop::post_delayed(Duration delay, Task task) -> TimerHandle
{
    if (!assert_on_loop_thread() || !_backend) {
        return {};
    }

    return _timers.start(std::move(task), Clock::now() + delay);
}

auto EventLoop::post_at(TimePoint when, Task task) -> TimerHandle
{
    if (!assert_on_loop_thread() || !_backend) {
        return {};
    }

    return _timers.start(std::move(task), when);
}

auto EventLoop::post_repeating(Duration interval, Task task) -> TimerHandle
{
    if (!assert_on_loop_thread() || !_backend) {
        return {};
    }

    return _timers.start(std::move(task), Clock::now() + interval, interval);
}

void EventLoop::cancel_timer(TimerHandle h)
{
    if (!assert_on_loop_thread()) {
        return; // can only be called from the thread running the event loop
    }

    _timers.cancel(h);
}

auto EventLoop::watch_fd(int fd, FdEvents events, FdCallback callback) -> FdWatch
{
    if (!assert_on_loop_thread() || !_backend || fd < 0 || events.none() || !callback) {
        return {};
    }

    if (!_backend->add_fd(fd, events)) {
        return {};
    }

    _watchers[fd] = {.callback = std::move(callback), .events = events};
    return {fd};
}

auto EventLoop::unwatch_fd(FdWatch handle) -> bool
{
    if (!assert_on_loop_thread()) {
        return false;
    }

    auto const it = _watchers.find(handle.fd);
    if (!handle || it == _watchers.end()) {
        return true;
    }

    if (!_backend || !_backend->remove_fd(handle.fd)) {
        return false;
    }

    _watchers.erase(it);
    return true;
}

auto EventLoop::quit() -> bool
{
    return post([this]() -> void { _running.store(false, std::memory_order_relaxed); });
}

auto EventLoop::run() -> bool
{
    // set the thread context for this event loop
    auto* orig_ctx = _thread_ctx.load(std::memory_order_relaxed);
    _thread_ctx.store(ThreadCtx::current(), std::memory_order_relaxed);

    _running.store(true, std::memory_order_relaxed);

    auto success = true;
    for (;;) {
        auto const result = process_events(EventFlag::All);
        if (result == ProcessEventsResult::Running) {
            continue;
        }
        if (result == ProcessEventsResult::Failed) {
            success = false;
        }
        break;
    }

    // Do not deliver object events after quit. Finish generic tasks first so any
    // deferred deletions they schedule are included in the final delete phase.
    drain_task_queue();
    drain_deferred_delete_queue();
    discard_event_queue();

    // restore the original thread context (if any)
    _thread_ctx.store(orig_ctx, std::memory_order_relaxed);
    return success;
}

auto EventLoop::process_events(EventFlags flags) -> ProcessEventsResult
{
    return process_events(flags, -1);
}

auto EventLoop::process_events(EventFlags flags, int ms) -> ProcessEventsResult
{
    if (!assert_on_loop_thread()) {
        return ProcessEventsResult::Failed; // can only be called from the thread running the event loop
    }
    if (!_backend) {
        _running.store(false, std::memory_order_relaxed);
        return ProcessEventsResult::Failed;
    }

    auto const was_running = _running.load(std::memory_order_relaxed);

    // 1. poll and dispatch fd events
    if (flags.test(EventFlag::Watchers)) {
        static constexpr int kMaxEvents{64};
        priv::ReadyEvent     events[kMaxEvents];

        auto timeout_ms = compute_timeout_ms(ms);
        auto n          = _backend->poll(events, kMaxEvents, timeout_ms);
        if (n < 0) {
            _running.store(false, std::memory_order_relaxed);
            return ProcessEventsResult::Failed;
        }

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

    // 4. drain the object event queue
    auto const stopped_during_processing = was_running && !_running.load(std::memory_order_relaxed);
    if (flags.test(EventFlag::Events) && !stopped_during_processing) {
        drain_event_queue();
    }

    // 5. process deferred deletes after task or object-event processing
    if (flags.test_any(EventFlags{EventFlag::Tasks, EventFlag::Events})) {
        drain_deferred_delete_queue();
    }

    return _running.load(std::memory_order_relaxed) ? ProcessEventsResult::Running : ProcessEventsResult::Stopped;
}

auto EventLoop::assert_on_loop_thread() const -> bool
{
    auto const is_current_thread = _thread_ctx.load(std::memory_order_relaxed) == ThreadCtx::current();
    if (!is_current_thread) {
        log_fatal("EventLoop methods must be called from the thread running the event loop");
    }
    return is_current_thread;
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

void EventLoop::drain_event_queue()
{
    std::queue<EventEntry> local;
    {
        std::lock_guard lock{_event_queue_mx};
        local.swap(_event_queue);
    }

    while (!local.empty()) {
        auto& entry    = local.front();
        auto  lifetime = entry.lifetime.lock();
        if (lifetime && lifetime->alive.load(std::memory_order_acquire)) {
            if (entry.delivery) {
                entry.delivery();
            }
            else {
                Application::send_event(entry.receiver, *entry.event);
            }
        }
        local.pop();
    }
}

void EventLoop::discard_event_queue()
{
    std::queue<EventEntry> discarded;
    {
        std::lock_guard lock{_event_queue_mx};
        discarded.swap(_event_queue);
    }
}

void EventLoop::drain_deferred_delete_queue()
{
    std::queue<DeferredDeleteEntry> local;
    {
        std::lock_guard lock{_deferred_delete_queue_mx};
        local.swap(_deferred_delete_queue);
    }

    while (!local.empty()) {
        auto& entry    = local.front();
        auto  lifetime = entry.lifetime.lock();
        if (lifetime && lifetime->alive.load(std::memory_order_acquire)) {
            delete entry.object;
        }
        local.pop();
    }
}

} // namespace jb::core
