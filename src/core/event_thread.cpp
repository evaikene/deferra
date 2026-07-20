#include "event_thread.hpp"

#include "event_loop.hpp"
#include "thread_context.hpp"

#include <cstdlib>

namespace jb::core {

EventThread::EventThread(Object* parent)
    : Object(parent)
    , _event_loop(std::make_unique<EventLoop>())
{
    move_to_thread(this);
}

EventThread::~EventThread()
{
    quit();
    wait();
}

auto EventThread::exec(bool event_loop_running) -> bool
{
    // start the thread and run the event loop
    if (!start()) {
        return false;
    }

    // wait for the thread or the event loop to start running
    if (event_loop_running) {
        while (!_event_loop->is_running() && !_finished.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        return _event_loop->is_running();
    }

    while (!_started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    return true;
}

auto EventThread::start() -> bool
{
    // ensure the thread is not already running
    if (_thread || _started.load(std::memory_order_acquire)) {
        log_fatal("Event thread is already running");
        return false;
    }
    if (!_event_loop->is_valid()) {
        return false;
    }

    _thread = std::make_unique<std::thread>([this]() -> void {
        // signal that the thread has started running
        _started.store(true, std::memory_order_release);

        // initialize the thread context
        auto* ctx = ThreadCtx::current();
        ctx->set_event_loop(_event_loop.get());

        emit(about_to_start);

        // run the event loop until quit is signaled
        if (!_event_loop->run()) {
            _exit_code.store(EXIT_FAILURE, std::memory_order_relaxed);
        }

        // signal that the thread has finished running
        emit(about_to_quit);

        // cleanup
        ctx->set_event_loop(nullptr);
        _finished.store(true, std::memory_order_release);
    });

    return true;
}

auto EventThread::is_running() const -> bool
{
    return _event_loop->is_running();
}

auto EventThread::quit(int exit_code) -> bool
{
    _exit_code.store(exit_code, std::memory_order_relaxed);
    return _event_loop->quit();
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
