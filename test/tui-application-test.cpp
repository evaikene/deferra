#include <jb/tui/application.hpp>

#include "support/operational_log_capture.hpp"

#include <jb/core/application.hpp>
#include <jb/core/event.hpp>
#include <jb/core/logging.hpp>
#include <jb/tui/events.hpp>
#include <jb/tui/widget.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>

using jb::core::Event;
using jb::core::LogLevel;
using jb::test::OperationalLogGuard;
using jb::tui::Application;
using jb::tui::EventTypes;
using jb::tui::Widget;

namespace {

class RegistrationProbe : public Widget {
public:

    auto event(Event& event) -> bool override
    {
        CHECK(event.type() != EventTypes::Show);
        CHECK(event.type() != EventTypes::Hide);
        return Widget::event(event);
    }
};

} // namespace

TEST_CASE("TUI application lookup follows the core singleton", "[tui][application]")
{
    CHECK(Application::instance() == nullptr);
    CHECK(jb::core::Application::instance() == nullptr);

    {
        jb::core::Application core{0, nullptr};
        CHECK(jb::core::Application::instance() == &core);
        CHECK(Application::instance() == nullptr);
    }

    {
        Application app{0, nullptr};
        CHECK(jb::core::Application::instance() == &app);
        CHECK(Application::instance() == &app);
        CHECK(app.main_widget() == nullptr);
    }

    CHECK(Application::instance() == nullptr);
    CHECK(jb::core::Application::instance() == nullptr);
}

TEST_CASE("TUI application replaces and clears registration without taking ownership", "[tui][application]")
{
    Application       app{0, nullptr};
    auto              first = std::make_unique<RegistrationProbe>();
    RegistrationProbe second;

    REQUIRE(app.set_main_widget(first.get()));
    CHECK(app.main_widget() == first.get());
    CHECK(first->parent() == nullptr);

    REQUIRE(app.set_main_widget(&second));
    CHECK(app.main_widget() == &second);
    CHECK(second.parent() == nullptr);

    first.reset();
    CHECK(app.main_widget() == &second);

    REQUIRE(app.set_main_widget(nullptr));
    CHECK(app.main_widget() == nullptr);
    CHECK(second.parent() == nullptr);
    CHECK(app.children().empty());
}

TEST_CASE("TUI application rejects a parented main widget and preserves registration", "[tui][application]")
{
    OperationalLogGuard logs;
    logs.capture->set_level(LogLevel::Fatal);

    Application app{0, nullptr};
    Widget      root;
    auto*       child = new Widget{&root};
    REQUIRE(app.set_main_widget(&root));

    CHECK_FALSE(app.set_main_widget(child));

    CHECK(logs.capture->records().size() == 1);
    CHECK(app.main_widget() == &root);
    CHECK(child->parent_widget() == &root);
}

TEST_CASE("TUI application clears the main widget before its destruction signal", "[tui][application]")
{
    bool        destroyed = false;
    Application app{0, nullptr};
    auto        widget = std::make_unique<Widget>();
    widget->destroyed.connect(&app, [&app, &destroyed]() -> void {
        CHECK(app.main_widget() == nullptr);
        destroyed = true;
    });

    REQUIRE(app.set_main_widget(widget.get()));
    widget.reset();

    CHECK(destroyed);
    CHECK(app.main_widget() == nullptr);
}

TEST_CASE("A main widget survives app teardown and a later app starts with an empty slot", "[tui][application]")
{
    bool    destroyed = false;
    Widget* survivor  = nullptr;
    {
        Application app{0, nullptr};
        survivor = new Widget;
        survivor->destroyed.connect([&destroyed]() -> void { destroyed = true; });

        CHECK(app.set_main_widget(survivor));
        CHECK(app.main_widget() == survivor);
    }
    REQUIRE_FALSE(destroyed);
    auto widget = std::unique_ptr<Widget>{survivor};

    Application next{0, nullptr};
    CHECK(next.main_widget() == nullptr);

    Widget replacement;
    REQUIRE(next.set_main_widget(&replacement));
    widget.reset();

    CHECK(destroyed);
    CHECK(next.main_widget() == &replacement);
}
