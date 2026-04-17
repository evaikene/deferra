#include "event_loop.hpp"

#include "thread_context.hpp"

#include <cassert>

namespace jb::core {

EventLoop::EventLoop()
    : _thread_ctx(ThreadCtx::current())
{}

EventLoop::~EventLoop() = default;

void EventLoop::post(Task task)
{
    {
        std::lock_guard lock{_queue_mx};
        _queue.push(std::move(task));
    }
    _queue_cv.notify_one();
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
    while (_running.load(std::memory_order_relaxed)) {
        Task task;
        {
            std::unique_lock lock(_queue_mx);
            _queue_cv.wait(lock, [this] () -> bool {
                return !_queue.empty();
            });
            task = std::move(_queue.front());
            _queue.pop();
        }
        task();
    }

    // drain the queue to ensure all posted tasks are processed before exiting
    process_events();

    // restore the original thread context (if any)
    _thread_ctx.store(orig_ctx, std::memory_order_relaxed);
}

auto EventLoop::process_events() -> bool
{
    assert(_thread_ctx.load(std::memory_order_relaxed) == ThreadCtx::current());
    if (_thread_ctx.load(std::memory_order_relaxed) != ThreadCtx::current()) {
        return false; // can only be called from the thread running the event loop
    }

    // use a local copy for processing so that the event queue can be unlocked
    std::queue<Task> tasks;
    {
        std::lock_guard lock{_queue_mx};
        tasks.swap(_queue);
    }

    // process the tasks outside the lock to allow new tasks to be posted while processing
    while (!tasks.empty()) {
        tasks.front()();
        tasks.pop();
    }

    return _running.load(std::memory_order_relaxed);
}

} // namespace jb::core
