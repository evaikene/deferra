#include "event_loop.hpp"

#include "application.hpp"
#include "event.hpp"
#include "logging.hpp"
#include "object_priv.hpp"
#include "thread_context.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <utility>

namespace jb::core {
namespace {

/// Preserve outer dispatch state when existing asynchronous callers process events recursively.
class DispatchScope final {
public:
    explicit DispatchScope(bool& dispatching)
        : _dispatching(dispatching)
        , _previous(std::exchange(dispatching, true))
    {}

    ~DispatchScope() { _dispatching = _previous; }

    DispatchScope(DispatchScope const&)                    = delete;
    auto operator=(DispatchScope const&) -> DispatchScope& = delete;

private:
    bool& _dispatching;
    bool  _previous;
};

/// A bounded wait leaves the loop idle on every return without performing run()'s shutdown drains.
class RunningScope final {
public:
    explicit RunningScope(std::atomic_bool& running)
        : _running(running)
    {
        _running.store(true, std::memory_order_relaxed);
    }

    ~RunningScope() { _running.store(false, std::memory_order_relaxed); }

    RunningScope(RunningScope const&)                    = delete;
    auto operator=(RunningScope const&) -> RunningScope& = delete;

private:
    std::atomic_bool& _running;
};

auto wait_error(ErrorCategory category, char const* code, char const* message) -> Result<void, Error>
{
    return Result<void, Error>::failure({.category = category, .code = code, .message = message});
}

} // namespace

thread_local bool EventLoop::s_dispatching = false;

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
    std::scoped_lock lock{_task_queue_mx};
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
    std::scoped_lock lock{_event_queue_mx};
    // Wake before insertion while holding the queue lock so a consumer cannot
    // pass the queue before the successfully signalled entry is visible.
    if (!_backend || !_backend->wakeup()) {
        return false;
    }
    _event_queue.push({
        .receiver = receiver,
        .lifetime = std::move(lifetime),
        .event    = std::move(event),
        .delivery = {},
    });
    return true;
}

auto EventLoop::post_event_delivery(Object* receiver, std::weak_ptr<priv::ObjectLifetime> lifetime, Task delivery)
    -> bool
{
    std::scoped_lock lock{_event_queue_mx};
    // Wake before insertion while holding the queue lock so a consumer cannot
    // pass the queue before the successfully signalled entry is visible.
    if (!_backend || !_backend->wakeup()) {
        return false;
    }
    _event_queue.push({
        .receiver = receiver,
        .lifetime = std::move(lifetime),
        .event    = {},
        .delivery = std::move(delivery),
    });
    return true;
}

auto EventLoop::defer_delete(Object* object, std::weak_ptr<priv::ObjectLifetime> lifetime) -> bool
{
    std::scoped_lock lock{_deferred_delete_queue_mx};
    // Wake before insertion while holding the queue lock so a consumer cannot
    // pass the queue before the successfully signalled entry is visible.
    if (!_backend || !_backend->wakeup()) {
        return false;
    }
    _deferred_delete_queue.push({
        .object   = object,
        .lifetime = std::move(lifetime),
    });
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

auto EventLoop::watch_fd(int fd, FdEvents events, FdTriggerMode trigger_mode, FdCallback callback) -> FdWatch
{
    if (!assert_on_loop_thread() || !_backend || fd < 0 || events.none() || !callback) {
        return {};
    }

    auto const existing = _watchers.find(fd);
    if (existing != _watchers.end() && !existing->second.refresh_required &&
        existing->second.events.bits() == events.bits() && existing->second.trigger_mode == trigger_mode) {
        // An unchanged native registration replaces only the callback and has
        // no backend-specific rearm or readiness-refresh side effect.
        existing->second.callback = std::move(callback);
        return {fd};
    }

    if (!_backend->add_fd(fd, events, trigger_mode)) {
        return {};
    }

    _watchers[fd] = {
        .callback     = std::move(callback),
        .events       = events,
        .trigger_mode = trigger_mode,
    };
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
        // Closing after failed removal can retire native state while retaining this numeric fd entry.
        // A later watch must reach the backend rather than assume that the old registration still exists.
        it->second.refresh_required = true;
        return false;
    }

    _watchers.erase(it);
    return true;
}

auto EventLoop::watch_process(std::int64_t process_id, Task callback) -> Result<void, Error>
{
    if (!assert_on_loop_thread() || !_backend) {
        return Result<void, Error>::failure({.category = ErrorCategory::Unavailable,
                                             .code     = "core.process.event_loop_unavailable",
                                             .message  = "Process watch requires a valid owner EventLoop"});
    }
    if (process_id <= 0 || !callback) {
        return Result<void, Error>::failure({.category = ErrorCategory::InvalidArgument,
                                             .code     = "core.process.invalid_request",
                                             .message  = "Invalid process watch",
                                             .detail   = "watch.invalid_registration"});
    }
    if (auto const existing = _process_watchers.find(process_id); existing != _process_watchers.end()) {
        if (!existing->second.removal_failed) {
            // Ordinary callback replacement must not rearm an exited child's persistently readable pidfd.
            existing->second.callback = std::move(callback);
            return Result<void, Error>::success();
        }
        // An invalidated registration can outlive its child and numeric PID. Do not accept another child
        // against that old one-shot pidfd: remove it first, or reject while Process still holds its gate.
        if (!unwatch_process(process_id)) {
            return Result<void, Error>::failure({.category = ErrorCategory::Unavailable,
                                                 .code     = "core.process.watch_failed",
                                                 .message  = "Retained child watch could not be removed",
                                                 .detail   = "watch.retained_registration"});
        }
    }

    // Allocate callback storage before installing native state. Backend failure is transactional.
    auto const entry  = _process_watchers.emplace(process_id, ProcessWatchEntry{.callback = std::move(callback)}).first;
    auto const result = _backend->add_process(process_id);
    if (result == priv::ProcessRegistrationResult::Added) {
        return Result<void, Error>::success();
    }
    _process_watchers.erase(entry);
    if (result == priv::ProcessRegistrationResult::Unsupported) {
        return Result<void, Error>::failure({.category = ErrorCategory::Unsupported,
                                             .code     = "core.process.monitor_unsupported",
                                             .message  = "Native child monitoring is unavailable"});
    }
    return Result<void, Error>::failure({.category = ErrorCategory::Unavailable,
                                         .code     = "core.process.watch_failed",
                                         .message  = "Native child watch registration failed"});
}

auto EventLoop::unwatch_process(std::int64_t process_id) -> bool
{
    if (!assert_on_loop_thread()) {
        return false;
    }
    auto const entry = _process_watchers.find(process_id);
    if (entry == _process_watchers.end()) {
        return true;
    }
    if (!_backend || !_backend->remove_process(process_id)) {
        entry->second.removal_failed = true;
        return false;
    }
    _process_watchers.erase(entry);
    return true;
}

auto EventLoop::quit() -> bool
{
    return post([this]() -> void { _running.store(false, std::memory_order_relaxed); });
}

void EventLoop::request_quit() noexcept
{
    _running.store(false, std::memory_order_relaxed);
}

auto EventLoop::run() -> bool
{
    DispatchScope dispatch{s_dispatching};

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

auto EventLoop::run_until(TimePoint deadline, std::function<bool()> const& complete) -> Result<void, Error>
{
    // A thread-wide guard also covers predicates and callbacks on other loops, not just this loop's run state.
    if (s_dispatching) {
        return wait_error(ErrorCategory::Conflict,
                          "core.event_loop.reentrant_wait",
                          "Cannot start a synchronous wait during event dispatch or another wait");
    }
    if (thread_ctx() != ThreadCtx::current() || current() != this || is_running()) {
        return wait_error(ErrorCategory::Conflict,
                          "core.event_loop.invalid_context",
                          "Synchronous wait requires the current idle owner event loop");
    }
    if (!complete) {
        return wait_error(ErrorCategory::InvalidArgument,
                          "core.event_loop.invalid_predicate",
                          "Synchronous wait requires a completion predicate");
    }
    if (!is_valid()) {
        return wait_error(ErrorCategory::Io, "core.event_loop.processing_failed", "Event-loop backend is unavailable");
    }

    DispatchScope dispatch{s_dispatching};
    RunningScope  running{_running};

    // All phases retain the caller's original deadline. A completed result needs no additional poll.
    for (;;) {
        if (complete()) {
            return Result<void, Error>::success();
        }
        auto const now = Clock::now();
        if (now >= deadline) {
            return wait_error(ErrorCategory::Timeout, "core.event_loop.timeout", "Synchronous wait deadline expired");
        }

        auto const remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - now).count();
        auto const timeout =
            static_cast<int>(std::min(remaining, static_cast<decltype(remaining)>(std::numeric_limits<int>::max())));
        auto const result = process_events(EventFlag::All, timeout);
        if (result == ProcessEventsResult::Failed) {
            return wait_error(ErrorCategory::Io, "core.event_loop.processing_failed", "Event-loop processing failed");
        }
        if (result == ProcessEventsResult::Stopped) {
            return wait_error(ErrorCategory::Cancelled,
                              "core.event_loop.stopped",
                              "Event loop stopped before completion");
        }
    }
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

    DispatchScope dispatch{s_dispatching};

    auto const was_running = _running.load(std::memory_order_relaxed);

    // 1. poll and dispatch native events in their separate identifier namespaces
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
            switch (events[i].kind) {
                case priv::ReadyEventKind::FileDescriptor:
                    dispatch_fd(events[i]);
                    break;
                case priv::ReadyEventKind::Process:
                    dispatch_process(events[i].ident);
                    break;
            }
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
    if (ev.ident < 0 || ev.ident > std::numeric_limits<int>::max()) {
        return;
    }
    auto const fd = static_cast<int>(ev.ident);
    auto       it = _watchers.find(fd);
    if (it != _watchers.end()) {
        it->second.callback(fd, ev.events);
    }
}

void EventLoop::dispatch_process(std::int64_t process_id) const
{
    auto const entry = _process_watchers.find(process_id);
    if (entry != _process_watchers.end()) {
        // Keep the active callable alive if it removes or replaces its own map entry.
        auto callback = entry->second.callback;
        callback();
    }
}

void EventLoop::drain_task_queue()
{
    std::queue<Task> local;
    {
        std::scoped_lock lock{_task_queue_mx};
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
        std::scoped_lock lock{_event_queue_mx};
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
        std::scoped_lock lock{_event_queue_mx};
        discarded.swap(_event_queue);
    }
}

void EventLoop::drain_deferred_delete_queue()
{
    std::queue<DeferredDeleteEntry> local;
    {
        std::scoped_lock lock{_deferred_delete_queue_mx};
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
