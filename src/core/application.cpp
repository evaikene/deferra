#include "application.hpp"

#include "thread_context.hpp"

#include <cassert>

namespace jb::core {

Application* Application::s_instance = nullptr;

Application::Application(int argc, char const* argv[])
    : _argc(argc)
    , _argv(argv)
{
    assert(s_instance == nullptr);

    s_instance = this;
    ThreadCtx::current()->set_event_loop(this);
}

Application::~Application()
{
    ThreadCtx::current()->set_event_loop(nullptr);
    s_instance = nullptr;
}

auto Application::exec() -> int
{
    about_to_start.emit();
    EventLoop::run();
    about_to_quit.emit();

    return _exit_code;
}

void Application::quit(int exit_code)
{
    _exit_code = exit_code;
    EventLoop::quit();
}

} // namespace jb::core
