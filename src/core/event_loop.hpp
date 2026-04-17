#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>

namespace jb::core {

struct ThreadCtx;

class EventLoop {
public:

    using Task = std::function<void()>;

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

    /// Processes queued events
    /// @return true if the event loop is still running, false if it has been
    /// signaled to quit.
    ///
    /// This method can be used to run the event loop manually. All the queued tasks
    /// will be called in the context of the calling thread.
    auto process_events() -> bool;

protected:
    std::queue<Task>        _queue;
    std::mutex              _queue_mx;
    std::condition_variable _queue_cv;
    std::atomic_bool        _running{false};
    std::atomic<ThreadCtx*> _thread_ctx;
};

} // namespace jb::core
