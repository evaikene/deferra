#include "event_loop.hpp"

namespace jb::core {

EventLoop::EventLoop() = default;

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
    _running.store(true, std::memory_order_relaxed);
    while (_running.load(std::memory_order_relaxed)) {
        Task task;
        {
            std::unique_lock lock(_queue_mx);
            _queue_cv.wait(lock, [this] () -> bool {
                return !_queue.empty() || !_running.load(std::memory_order_relaxed);
            });
            if (!_running.load(std::memory_order_relaxed) && _queue.empty()) {
                break;
            }
            task = std::move(_queue.front());
            _queue.pop();
        }
        task();
    }
}

auto EventLoop::process_events() -> bool
{
    _running.store(true, std::memory_order_relaxed);

    // empty the queue first to allow new tasks to be posted while processing
    std::queue<Task> tasks;
    {
        std::lock_guard lock{_queue_mx};
        while (!_queue.empty()) {
            tasks.push(std::move(_queue.front()));
            _queue.pop();
        }
    }

    // process the tasks outside the lock to allow new tasks to be posted while processing
    while (!tasks.empty()) {
        tasks.front()();
        tasks.pop();
    }

    return _running.load(std::memory_order_relaxed);
}

} // namespace jb::core
