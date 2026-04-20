#include "event_thread.hpp"

#include "event_loop.hpp"
#include "thread_context.hpp"

namespace jb::core {

EventThread::EventThread(Object* parent)
    : Object(parent)
    , _event_loop(std::make_unique<EventLoop>())
{}

EventThread::~EventThread()
{
    quit();
    wait();
}

void EventThread::exec(bool event_loop_running)
{
    // start the thread and run the event loop
    start();

    // wait for the thread or the event loop to start running
    if (event_loop_running) {
        while (!_event_loop->is_running()) {
            std::this_thread::yield();
        }
    }
    else {
        while (!_started.load(std::memory_order_relaxed)) {
            std::this_thread::yield();
        }
    }
}

void EventThread::start()
{
    // ensure the thread is not already running
    assert(!_thread);
    if (_thread) {
        return;
    }

    _thread = std::make_unique<std::thread>([this]() -> void {
        // signal that the thread has started running
        _started.store(true, std::memory_order_relaxed);

        // initialize the thread context
        auto* ctx = ThreadCtx::current();
        ctx->set_event_loop(_event_loop.get());

        about_to_start.emit();

        // run the event loop until quit is signaled
        _event_loop->run();

        // signal that the thread has finished running
        about_to_quit.emit();

        // cleanup
        ctx->set_event_loop(nullptr);
    });
}

auto EventThread::is_running() const -> bool
{
    return _event_loop->is_running();
}

void EventThread::quit(int exit_code)
{
    _exit_code.store(exit_code, std::memory_order_relaxed);
    _event_loop->quit();
}

void EventThread::wait()
{
    if (!_thread || !_thread->joinable()) {
        return;
    }
    _thread->join();
    _thread.reset();
}

} // namespace jb::core
