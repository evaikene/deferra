#include "application.hpp"

#include "application_priv.hpp"
#include "event.hpp"
#include "event_loop.hpp"
#include "event_thread.hpp"
#include "logging.hpp"
#include "thread_context.hpp"

#include <cstdlib>

namespace jb::core {

Application* Application::s_instance = nullptr;

Application::Application(int argc, char const* argv[])
    : Application(*new priv::ApplicationPrivate, argc, argv)
{}

Application::Application(priv::ApplicationPrivate& dd, int argc, char const* argv[])
    : Object(dd)
{
    auto* data = d_ptr<priv::ApplicationPrivate>();
    data->argc = argc;
    data->argv = argv;

    // Object owns the private block before EventThread allocation can throw.
    data->event_thread = std::make_unique<EventThread>();

    // enforce singleton
    if (s_instance) {
        log_fatal("Application instance already exists");
        return;
    }

    s_instance = this;
    ThreadCtx::current()->set_event_loop(data->event_thread->as_event_loop());
    move_to_thread(data->event_thread.get());
}

Application::~Application()
{
    // enforce singleton
    if (s_instance != this) {
        log_fatal("Application instance mismatch");
        return;
    }

    // Clear process-global references before Object deletes the private EventThread.
    ThreadCtx::current()->set_event_loop(nullptr);
    s_instance = nullptr;
}

auto Application::thread() const -> EventThread*
{
    return d_ptr<priv::ApplicationPrivate const>()->event_thread.get();
}

auto Application::send_event(Object* receiver, Event& event) -> bool
{
    if (!receiver) {
        return false;
    }

    return receiver->event(event);
}

void Application::post_event(Object* receiver, std::unique_ptr<Event> event)
{
    if (!receiver || !event) {
        return;
    }

    auto lifetime = receiver->lifetime().lock();
    if (!lifetime) {
        return;
    }

    std::lock_guard lock{lifetime->event_loop_mx};
    if (!lifetime->alive.load(std::memory_order_acquire)) {
        return;
    }

    auto* event_loop = lifetime->event_loop;
    if (!event_loop) {
        log_error("Application::post_event: receiver must have an event loop");
        return;
    }

    if (!event_loop->post_event(receiver, lifetime, std::move(event))) {
        log_error("Application::post_event: event-loop wakeup failed; dropping event");
    }
}

auto Application::exec() -> int
{
    auto* data = d_ptr<priv::ApplicationPrivate>();

    emit(about_to_start);
    if (!data->event_thread->as_event_loop()->run()) {
        data->exit_code = EXIT_FAILURE;
    }
    emit(about_to_quit);

    return data->exit_code;
}

auto Application::process_events(EventFlags flags) -> ProcessEventsResult
{
    return d_ptr<priv::ApplicationPrivate>()->event_thread->as_event_loop()->process_events(flags);
}

auto Application::process_events(EventFlags flags, int ms) -> ProcessEventsResult
{
    return d_ptr<priv::ApplicationPrivate>()->event_thread->as_event_loop()->process_events(flags, ms);
}

auto Application::quit(int exit_code) -> bool
{
    auto* data      = d_ptr<priv::ApplicationPrivate>();
    data->exit_code = exit_code;
    return data->event_thread->quit();
}

auto Application::exit_code() const -> int
{
    return d_ptr<priv::ApplicationPrivate const>()->exit_code;
}

} // namespace jb::core
