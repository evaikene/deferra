#pragma once

#include "event_loop_backend.hpp"
#include "event_loop_types.hpp"
#include "timer_heap.hpp"

#include <atomic>
#include <mutex>
#include <queue>

namespace jb::core {

struct ThreadCtx;

class EventLoop {
public:

    /// Constructor
    EventLoop();

    /// Destructor
    ~EventLoop();

    /// Posts a task to the event queue
    /// @param[in] task Task to post
    ///
    /// This method is thread-safe and can be called from any thread. The posted
    /// task will be executed on the thread running the event loop.
    void post(Task task);

    /// Post a task that will be executed after the given delay
    /// @param[in] delay Delay after which the task will be executed
    /// @param[in] task Task to post
    /// @return Timer handle
    auto post_delayed(Duration delay, Task task) -> TimerHandle;

    /// Post a task that will be executed at the given time point
    /// @param[in] when Time point at which the task will be executed
    /// @param[in] task Task to post
    /// @return Timer handle
    auto post_at(TimePoint when, Task task) -> TimerHandle;

    /// Post a repeating task that will be executed repeatedly at the given interval
    /// @param[in] interval Interval at which the task will be executed
    /// @param[in] task Task to post
    /// @return Timer handle
    auto post_repeating(Duration interval, Task task) -> TimerHandle;

    /// Cancels a timer by its handle. If the timer is already executed or cancelled, this is a no-op.
    /// @param[in] handle Timer handle to cancel
    void cancel_timer(TimerHandle handle);

    /// Runs the event loop until quit is signaled
    ///
    /// This method must be called from the thread running the event loop. It runs
    /// the event loop until quit is signaled. The event loop will process posted
    /// tasks and can be signaled to quit by calling the `quit()` method from any thread.
    /// After quit is signaled, the event loop will finish processing any remaining
    /// tasks and then exit.
    void run();

    /// Returns true if the event loop is running, false otherwise
    ///
    /// This method is thread-safe
    auto is_running() const -> bool { return _running.load(std::memory_order_relaxed); }

    /// Returns the thread context this event loop is running on
    /// @return Thread context this event loop is running on
    auto thread_ctx() const -> ThreadCtx const* { return _thread_ctx.load(std::memory_order_relaxed); }

    /// Signals the event loop to quit
    ///
    /// This method is thread-safe and can be called from any thread. It signals
    /// the event loop to quit.
    void quit();

    /// Processes queued tasks
    /// @return true if the event loop is still running, false if it has been
    /// signaled to quit.
    ///
    /// This method can be used to run the event loop manually. All the queued tasks
    /// will be called in the context of the calling thread.
    auto process_events() -> bool;

private:

    std::unique_ptr<priv::Backend> _backend;
    std::queue<Task>               _task_queue;
    std::mutex                     _task_queue_mx;

    priv::TimerHeap _timers;

    std::atomic_bool           _running{false};
    std::atomic<std::uint32_t> _next_watch_id{1};

    std::atomic<ThreadCtx*> _thread_ctx;

    auto compute_timeout_ms() -> int;
    void dispatch_fd(priv::ReadyEvent& ev) const;
    void drain_task_queue();
};

} // namespace jb::core
