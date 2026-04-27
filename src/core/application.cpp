#include "application.hpp"

#include "event_loop.hpp"
#include "event_thread.hpp"
#include "thread_context.hpp"

#include <cassert>

namespace jb::core {

Application* Application::s_instance = nullptr;

Application::Application(int argc, char const* argv[])
    : _argc(argc)
    , _argv(argv)
    , _event_loop(std::make_unique<EventThread>())
{
    assert(s_instance == nullptr);

    s_instance = this;
    ThreadCtx::current()->set_event_loop(_event_loop->as_event_loop());
    set_event_loop(_event_loop->as_event_loop());
}

Application::~Application()
{
    ThreadCtx::current()->set_event_loop(nullptr);
    s_instance = nullptr;
}

auto Application::exec() -> int
{
    about_to_start.emit();
    _event_loop->as_event_loop()->run();
    about_to_quit.emit();

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
