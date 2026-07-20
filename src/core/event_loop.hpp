#pragma once

#include "event_loop_backend.hpp"
#include "event_loop_types.hpp"
#include "timer_heap.hpp"

#include <atomic>
#include <mutex>
#include <queue>
#include <unordered_map>

namespace jb::core {

class Application;
class Event;
class Object;
struct ThreadCtx;

namespace priv {
struct EventLoopTestAccess;
struct ObjectLifetime;
} // namespace priv

/// EventLoop is a single-threaded event loop that supports posting tasks and object
/// events, timers, and file descriptor watches.
///
/// EventLoop class is not meant to be used directly by most users. Instead, it is used
/// internally by Application and EventThread classes. However, it can be used directly
/// in some cases where a custom event loop is needed. It provides a simple and efficient
/// way to run an event loop on a single thread.
///
/// Threading contract:
///
/// * post(), Application::post_event(), and quit() are thread-safe and may be
///   called from any thread.
/// * All other methods (timers, fd watching) MUST be called from the thread that
///   owns this loop - i.e. the thread currently inside run().
/// * To schedule a timer or watch an fd from another thread, wrap the call
///   in post():
///     loop.post([&loop, fd] {
///         loop.watch_fd(fd, FdEvent::Read, ...);
///     });
class EventLoop {
public:

    /// Returns the EventLoop instance running in the current thread
    /// @return EventLoop instance (can be nullptr if none are running)
    static auto current() noexcept -> EventLoop*;

    /// Constructs an event loop for the current thread.
    ///
    /// Native poller initialization failures do not throw. Instead, the
    /// constructed loop is invalid and `is_valid()` returns false. Allocation
    /// failures such as `std::bad_alloc` are not converted.
    EventLoop();

    /// Destructor
    ~EventLoop();

    /// Returns whether the native event-loop backend initialized successfully.
    /// @return true when event-loop operations can be performed, false otherwise
    [[nodiscard]] auto is_valid() const noexcept -> bool;

    /// Posts a task to the event queue.
    /// @param[in] task Task to post
    /// @return true when the task was queued and the poller was signalled; false
    ///         when the task was not queued
    ///
    /// This method is thread-safe and can be called from any thread. The posted
    /// task will be executed on the thread running the event loop.
    auto post(Task task) -> bool;

    /// Post a task that will be executed after the given delay
    /// @param[in] delay Delay after which the task will be executed
    /// @param[in] task Task to post
    /// @return Timer handle
    ///
    /// This method is NOT thread-safe and must be called from the thread running the event loop.
    auto post_delayed(Duration delay, Task task) -> TimerHandle;

    /// Post a task that will be executed at the given time point
    /// @param[in] when Time point at which the task will be executed
    /// @param[in] task Task to post
    /// @return Timer handle
    ///
    /// This method is NOT thread-safe and must be called from the thread running the event loop.
    auto post_at(TimePoint when, Task task) -> TimerHandle;

    /// Post a repeating task that will be executed repeatedly at the given interval
    /// @param[in] interval Interval at which the task will be executed
    /// @param[in] task Task to post
    /// @return Timer handle
    ///
    /// This method is NOT thread-safe and must be called from the thread running the event loop.
    auto post_repeating(Duration interval, Task task) -> TimerHandle;

    /// Cancels a timer by its handle. If the timer is already executed or cancelled, this is a no-op.
    /// @param[in] handle Timer handle to cancel
    ///
    /// This method is NOT thread-safe and must be called from the thread running the event loop.
    void cancel_timer(TimerHandle handle);

    /// Add a file descriptor to be monitored for the specified events. The callback
    /// will be called when the events are ready.
    /// @param[in] fd File descriptor to monitor
    /// @param[in] events Events to monitor (read/write)
    /// @param[in] callback Callback to call when the events are ready
    /// @return Watch handle that can be used to remove the watch later, or an
    ///         invalid handle when validation or backend registration fails
    ///
    /// The descriptor must be non-negative, @p events and @p callback must be
    /// nonempty, and replacing an existing watch changes its callback only after
    /// the backend registration succeeds.
    ///
    /// This method is NOT thread-safe and must be called from the thread running the event loop.
    auto watch_fd(int fd, FdEvents events, FdCallback callback) -> FdWatch;

    /// Removes a file descriptor watch by its handle.
    /// @param[in] handle Watch handle to remove
    /// @return true when the watch is already invalid or absent, or backend
    ///         removal succeeds; false when an active registration could not be removed
    ///
    /// On failure the public watch entry is retained so removal can be retried or
    /// the descriptor can be closed.
    ///
    /// This method is NOT thread-safe and must be called from the thread running the event loop.
    auto unwatch_fd(FdWatch handle) -> bool;

    /// Runs the event loop until quit is signaled
    ///
    /// This method must be called from the thread running the event loop. It runs
    /// the event loop until quit is signaled. The event loop will process posted
    /// tasks and can be signaled to quit by calling the `quit()` method from any thread.
    /// After quit is signaled, the event loop will finish processing remaining
    /// generic tasks and deferred deletes, but will not deliver remaining object
    /// events.
    /// @return true after an ordinary stop, false after invalid initialization
    ///         or a backend polling failure
    auto run() -> bool;

    /// Returns true if the event loop is running, false otherwise
    ///
    /// This method is thread-safe and can be called from any thread.
    auto is_running() const -> bool { return _running.load(std::memory_order_relaxed); }

    /// Returns the thread context this event loop is running on
    /// @return Thread context this event loop is running on
    ///
    /// This method is thread-safe and can be called from any thread.
    auto thread_ctx() const -> ThreadCtx const* { return _thread_ctx.load(std::memory_order_relaxed); }

    /// Signals the event loop to quit
    ///
    /// This method is thread-safe and can be called from any thread. It signals
    /// the event loop to quit.
    /// @return true when the stop task was queued, false otherwise
    auto quit() -> bool;

    /// Processes specified events until there are no more events to process
    /// @param[in] flags Events to process (tasks, object events, timers, watchers)
    /// @return `Running` or `Stopped` after successful processing according to
    ///         the loop state, or `Failed` for an invalid loop, thread-contract
    ///         violation, or backend polling failure
    ///
    /// Deferred deletes are processed after task or object-event phases. Timer-only
    /// and watcher-only processing does not perform deferred deletion.
    ///
    /// This method is NOT thread-safe and must be called from the thread running the event loop.
    auto process_events(EventFlags flags) -> ProcessEventsResult;

    /// Processes specified events for `ms` milliseconds, or until there are no more events
    /// to process, whichever comes first.
    /// @param[in] flags Events to process (tasks, object events, timers, watchers)
    /// @param[in] ms Maximum time to process events in milliseconds (negative means no timeout)
    /// @return `Running` or `Stopped` after successful processing according to
    ///         the loop state, or `Failed` for an invalid loop, thread-contract
    ///         violation, or backend polling failure
    ///
    /// The `ms` timeout applies only to fd events. If `EventFlag::Watchers` is not set in `flags`,
    /// then `ms` is ignored and this method behaves the same as `process_events(flags)`.
    ///
    /// Deferred deletes are processed after task or object-event phases. Timer-only
    /// and watcher-only processing does not perform deferred deletion.
    ///
    /// This method is NOT thread-safe and must be called from the thread running the event loop.
    auto process_events(EventFlags flags, int ms) -> ProcessEventsResult;

private:

    friend class Application;
    friend class Object;
    friend struct priv::EventLoopTestAccess;

    explicit EventLoop(std::unique_ptr<priv::Backend> backend);

    struct WatchEntry {
        FdCallback callback;
        FdEvents   events;
    };

    struct EventEntry {
        Object*                             receiver;
        std::weak_ptr<priv::ObjectLifetime> lifetime;
        std::unique_ptr<Event>              event;
        Task                                delivery;
    };

    struct DeferredDeleteEntry {
        Object*                             object;
        std::weak_ptr<priv::ObjectLifetime> lifetime;
    };

    std::atomic<ThreadCtx*> _thread_ctx;
    std::atomic_bool        _running{false};

    // Cross-thread inbox - thread-safe
    std::queue<Task> _task_queue;
    std::mutex       _task_queue_mx;

    std::queue<EventEntry> _event_queue;
    std::mutex             _event_queue_mx;

    std::queue<DeferredDeleteEntry> _deferred_delete_queue;
    std::mutex                      _deferred_delete_queue_mx;

    // Loop-thread-only state - NOT thread-safe
    std::unique_ptr<priv::Backend>      _backend;
    priv::TimerHeap                     _timers;
    std::unordered_map<int, WatchEntry> _watchers;

    auto assert_on_loop_thread() const -> bool;
    auto compute_timeout_ms(int max_timeout_ms) -> int;
    void dispatch_fd(priv::ReadyEvent const& ev) const;
    auto post_event(Object* receiver, std::weak_ptr<priv::ObjectLifetime> lifetime, std::unique_ptr<Event> event)
        -> bool;
    auto post_event_delivery(Object* receiver, std::weak_ptr<priv::ObjectLifetime> lifetime, Task delivery) -> bool;
    auto defer_delete(Object* object, std::weak_ptr<priv::ObjectLifetime> lifetime) -> bool;
    void drain_task_queue();
    void drain_event_queue();
    void discard_event_queue();
    void drain_deferred_delete_queue();
};

} // namespace jb::core
