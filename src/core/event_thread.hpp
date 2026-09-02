#pragma once

#include "object.hpp"
#include "signal.hpp"

namespace jb::core {

class EventLoop;

namespace priv {
struct EventThreadPrivate; // defined in event_thread_priv.hpp
} // namespace priv

/// Event loop class running in a separate thread
class EventThread : public Object {
public:

    /// Constructor
    /// @param[in] parent Optional parent object.
    explicit EventThread(Object* parent = nullptr);

    /// Destructor
    ~EventThread() override;

    /// Starts the thread and runs the event loop
    /// @param[in] event_loop_running If true, this method will block until the event
    ///            loop is running. If false, it will block until the thread has
    ///            started executing, but not necessarily until the event loop is running.
    ///
    /// This method blocks the calling thread until the thread represented by this
    /// class has started executing and optionally until the event loop is running.
    /// @return true when the requested start state is observed, false when the
    ///         thread cannot start or the event loop finishes before it is observed running
    ///
    /// WARNING: Do not call this method more than once on the same `Thread` instance.
    auto exec(bool event_loop_running = false) -> bool;

    /// Starts the thread and runs the event loop without blocking the calling thread.
    ///
    /// Use the `is_running()` method to check if the thread has started
    /// executing and the event loop is running.
    /// @return true when the worker thread was created, false when the event loop
    ///         is invalid or this instance was already started
    ///
    /// WARNING: Do not call this method more than once on the same `Thread` instance.
    auto start() -> bool;

    /// Returns true if the thread has started executing and the event loop is running
    auto is_running() const -> bool;

    /// Returns the event loop this thread is running
    auto as_event_loop() const -> EventLoop*;

    /// Signals the thread to quit
    /// @param[in] exit_code Exit code to quit with (default: 0)
    ///
    /// This method signals the thread to quit by queuing a quit event in the
    /// thread's event loop. The thread will process this event and exit with the
    /// specified exit code.
    /// @return true when the stop task was queued, false when the caller must not
    ///         assume the event loop was woken
    ///
    /// The requested exit code is stored even if the stop task cannot be queued.
    ///
    /// Use the `wait()` method after calling this method to ensure that the
    /// thread has finished executing before the program continues.
    ///
    /// This method is thread-safe.
    auto quit(int exit_code = 0) -> bool;

    /// Waits until the thread finishes execution
    ///
    /// This method blocks the calling thread until the thread represented by this
    /// class has finished executing. It is important to call this method after
    /// signaling the thread to quit using the `quit()` method to ensure that all
    /// resources are properly released and that the thread has completed its work
    /// before the program continues.
    void wait();

    /// Returns the exit code of the thread after it has finished executing
    /// @return Exit code of the thread
    ///
    /// This method should be called after the thread has finished executing
    /// (i.e., after calling `wait()`) to retrieve the exit code that the thread
    /// returned upon quitting. Backend failure produces `EXIT_FAILURE`.
    auto exit_code() const -> int;

    //--- SIGNALS ---

    /// Signal emitted when the thread is about to start.
    Signal<> about_to_start;

    /// Signal emitted when the thread is about to quit.
    Signal<> about_to_quit;

protected:

    /// Constructor for subclasses that supply their own private data.
    /// @param[in] dd Reference to a heap-allocated struct that inherits directly
    ///               or transitively from priv::EventThreadPrivate. EventThread takes
    ///               ownership; do NOT delete @p dd elsewhere.
    /// @param[in] parent Optional parent object.
    explicit EventThread(priv::EventThreadPrivate& dd, Object* parent = nullptr);
};

} // namespace jb::core
