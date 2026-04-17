#pragma once

#include "object.hpp"
#include "signal.hpp"

#include <atomic>
#include <memory>
#include <thread>

namespace jb::core {

class EventLoop;

/// Event loop class running in a separate thread
class EventThread : public Object {
public:

    /// Constructor
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
    ///
    /// WARNING: Do not call this method more than once on the same `Thread` instance.
    void exec(bool event_loop_running = false);

    /// Starts the thread and runs the event loop without blocking the calling thread.
    ///
    /// Use the `is_running()` method to check if the thread has started
    /// executing and the event loop is running.
    ///
    /// WARNING: Do not call this method more than once on the same `Thread` instance.
    void start();

    /// Returns true if the thread has started executing and the event loop is running
    auto is_running() const -> bool;

    /// Returns the event loop this thread is running
    auto as_event_loop() const -> EventLoop* { return _event_loop.get(); }

    /// Signals the thread to quit
    /// @param[in] exit_code Exit code to quit with (default: 0)
    ///
    /// This method signals the thread to quit by queuing a quit event in the
    /// thread's event loop. The thread will process this event and exit with the
    /// specified exit code.
    ///
    /// Use the `wait()` method after calling this method to ensure that the
    /// thread has finished executing before the program continues.
    ///
    /// This method is thread-safe.
    void quit(int exit_code = 0);

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
    /// returned upon quitting.
    auto exit_code() const -> int { return _exit_code.load(std::memory_order_relaxed); }

    //--- SIGNALS ---

    /// Signal emitted when the thread is about to start.
    Signal<> about_to_start;

    /// Signal emitted when the thread is about to quit.
    Signal<> about_to_quit;

private:

    std::unique_ptr<EventLoop>   _event_loop;
    std::unique_ptr<std::thread> _thread;
    std::atomic_bool             _started{false};
    std::atomic<int>             _exit_code{0};
};

} // namespace jb::core
