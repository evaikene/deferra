#include "event_thread.hpp"

#include "event_loop.hpp"
#include "event_thread_priv.hpp"
#include "thread_context.hpp"

#include <cstdlib>

namespace jb::core {

EventThread::EventThread(Object* parent)
    : EventThread(*new priv::EventThreadPrivate, parent)
{}

EventThread::EventThread(priv::EventThreadPrivate& dd, Object* parent)
    : Object(dd, parent)
{
    // Object owns the private block before EventLoop allocation can throw.
    d_ptr<priv::EventThreadPrivate>()->event_loop = std::make_unique<EventLoop>();
    move_to_thread(this);
}

EventThread::~EventThread()
{
    // Finish the worker while EventThread's signals and private data still exist.
    quit();
    wait();
}

auto EventThread::exec(bool event_loop_running) -> bool
{
    // start the thread and run the event loop
    if (!start()) {
        return false;
    }

    auto* data = d_ptr<priv::EventThreadPrivate>();

    // wait for the thread or the event loop to start running
    if (event_loop_running) {
        while (!data->event_loop->is_running() && !data->finished.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        return data->event_loop->is_running();
    }

    while (!data->started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    return true;
}

auto EventThread::start() -> bool
{
    auto* data = d_ptr<priv::EventThreadPrivate>();

    // ensure the thread is not already running
    if (data->thread || data->started.load(std::memory_order_acquire)) {
        log_fatal("Event thread is already running");
        return false;
    }
    if (!data->event_loop->is_valid()) {
        return false;
    }

    data->thread = std::make_unique<std::thread>([this]() -> void {
        auto* data = d_ptr<priv::EventThreadPrivate>();

        // signal that the thread has started running
        data->started.store(true, std::memory_order_release);

        // initialize the thread context
        auto* ctx = ThreadCtx::current();
        ctx->set_event_loop(data->event_loop.get());

        emit(about_to_start);

        // run the event loop until quit is signaled
        if (!data->event_loop->run()) {
            data->exit_code.store(EXIT_FAILURE, std::memory_order_relaxed);
        }

        // signal that the thread has finished running
        emit(about_to_quit);

        // cleanup
        ctx->set_event_loop(nullptr);
        data->finished.store(true, std::memory_order_release);
    });

    return true;
}

auto EventThread::is_running() const -> bool
{
    return d_ptr<priv::EventThreadPrivate const>()->event_loop->is_running();
}

auto EventThread::as_event_loop() const -> EventLoop*
{
    return d_ptr<priv::EventThreadPrivate const>()->event_loop.get();
}

auto EventThread::quit(int exit_code) -> bool
{
    auto* data = d_ptr<priv::EventThreadPrivate>();
    data->exit_code.store(exit_code, std::memory_order_relaxed);
    return data->event_loop->quit();
}

void EventThread::wait()
{
    auto* data = d_ptr<priv::EventThreadPrivate>();
    if (!data->thread || !data->thread->joinable()) {
        return;
    }
    data->thread->join();
    data->thread.reset();
}

auto EventThread::exit_code() const -> int
{
    return d_ptr<priv::EventThreadPrivate const>()->exit_code.load(std::memory_order_relaxed);
}

} // namespace jb::core
