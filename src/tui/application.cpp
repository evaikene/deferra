#include "application.hpp"

#include "widget.hpp"

#include <jb/core/application.hpp>
#include <jb/core/logging.hpp>

#include <atomic>

namespace jb::tui {
namespace {

// This slot belongs to the active core application's lifetime. Widget teardown
// checks it even for unregistered widgets that live on other threads.
constinit std::atomic<Widget*> registered_main_widget{nullptr};

} // namespace

Application::Application(int argc, char const* argv[])
    : jb::core::Application(argc, argv)
{}

Application::~Application()
{
    if (jb::core::Application::instance() == this) {
        registered_main_widget.store(nullptr, std::memory_order_relaxed);
    }
}

auto Application::instance() -> Application*
{
    return dynamic_cast<Application*>(jb::core::Application::instance());
}

auto Application::main_widget() const -> Widget*
{
    return registered_main_widget.load(std::memory_order_relaxed);
}

auto Application::set_main_widget(Widget* widget) -> bool
{
    if (widget && widget->parent()) {
        jb::core::log_fatal("Application::set_main_widget: main widget must be parentless");
        return false;
    }

    registered_main_widget.store(widget, std::memory_order_relaxed);
    return true;
}

auto Application::register_shown_top_level(Widget* widget) -> bool
{
    if (auto const* current = main_widget(); current && current != widget) {
        jb::core::log_fatal("Application::register_shown_top_level: another main widget is already registered");
        return false;
    }

    return set_main_widget(widget);
}

auto Application::is_registered_main_widget(Widget const* widget) noexcept -> bool
{
    return registered_main_widget.load(std::memory_order_relaxed) == widget;
}

void Application::unregister_destroyed_widget(Widget* widget) noexcept
{
    // Destroying an earlier main widget must preserve its replacement.
    registered_main_widget.compare_exchange_strong(widget, nullptr, std::memory_order_relaxed);
}

} // namespace jb::tui
