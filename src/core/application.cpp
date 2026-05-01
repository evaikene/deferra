#include "application.hpp"

#include "event_loop.hpp"
#include "event_thread.hpp"
#include "logging.hpp"
#include "thread_context.hpp"

#include <cassert>

namespace jb::core {

Application* Application::s_instance = nullptr;

Application::Application(int argc, char const* argv[])
    : _argc(argc)
    , _argv(argv)
    , _event_loop(std::make_unique<EventThread>())
{
    // enforce singleton
    assert(s_instance == nullptr);
    if (s_instance) {
        log_error("Application instance already exists");
        return;
    }

    s_instance = this;
    ThreadCtx::current()->set_event_loop(_event_loop->as_event_loop());
    move_to_thread(_event_loop.get());
}

Application::~Application()
{
    // enforce singleton
    assert(s_instance == this);
    if (s_instance != this) {
        log_error("Application instance mismatch");
        return;
    }

    ThreadCtx::current()->set_event_loop(nullptr);
    s_instance = nullptr;
}

auto Application::exec() -> int
{
    emit(about_to_start);
    _event_loop->as_event_loop()->run();
    emit(about_to_quit);

    return _exit_code;
}

auto Application::process_events(EventFlags flags) -> bool
{
    return _event_loop->as_event_loop()->process_events(flags);
}

auto Application::process_events(EventFlags flags, int ms) -> bool
{
    return _event_loop->as_event_loop()->process_events(flags, ms);
}

void Application::quit(int exit_code)
{
    _exit_code = exit_code;
    _event_loop->quit();
}

} // namespace jb::core
