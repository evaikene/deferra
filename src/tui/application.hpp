#pragma once

#include <jb/core/application.hpp>

namespace jb::tui {

class Widget;

/// The TUI application and its main-widget registration.
/// Access the registration and its widget on the application thread.
class Application : public jb::core::Application {
public:

    /// Initializes the core application and its event loop.
    explicit Application(int argc, char const* argv[]);
    ~Application() override;

    /// Returns the core application when it is a TUI application, or nullptr otherwise.
    [[nodiscard]] static auto instance() -> Application*;

    /// Returns the borrowed main widget, or nullptr when the slot is empty.
    /// The registration clears when the widget or application is destroyed.
    [[nodiscard]] auto main_widget() const -> Widget*;

    /// Registers a parentless widget, or clears the registration for nullptr.
    /// The caller retains ownership and control of visibility. A parented widget
    /// logs a fatal error and returns false with the existing registration intact.
    auto set_main_widget(Widget* widget) -> bool;

private:

    friend class Widget;

    auto        register_shown_top_level(Widget* widget) -> bool;
    static auto is_registered_main_widget(Widget const* widget) noexcept -> bool;
    static void unregister_destroyed_widget(Widget* widget) noexcept;
};

} // namespace jb::tui
