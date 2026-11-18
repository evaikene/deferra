#include <jb/tui/events.hpp>

#include <jb/core/application.hpp>
#include <jb/core/event.hpp>
#include <jb/core/object.hpp>
#include <jb/tui/geometry.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>

using jb::core::Application;
using jb::core::Event;
using jb::core::Object;
using jb::tui::EventTypes;
using jb::tui::HideEvent;
using jb::tui::MoveEvent;
using jb::tui::Point;
using jb::tui::ResizeEvent;
using jb::tui::ShowEvent;
using jb::tui::Size;

namespace {

class EventReceiver : public Object {
public:

    auto event(Event& notification) -> bool override
    {
        received = &notification;
        return true;
    }

    Event const* received = nullptr;
};

} // namespace

TEST_CASE("TUI notifications have distinct reserved user event IDs", "[tui][event]")
{
    STATIC_CHECK(EventTypes::TuiBase == Event::User + 10'000);
    STATIC_CHECK(EventTypes::Move == EventTypes::TuiBase + 1);
    STATIC_CHECK(EventTypes::Resize == EventTypes::TuiBase + 2);
    STATIC_CHECK(EventTypes::Show == EventTypes::TuiBase + 3);
    STATIC_CHECK(EventTypes::Hide == EventTypes::TuiBase + 4);
}

TEST_CASE("TUI move events retain copies of both positions", "[tui][event]")
{
    Point           old_pos{.x = -3, .y = 2};
    Point           new_pos{.x = 5, .y = -4};
    MoveEvent const event{old_pos, new_pos};

    old_pos = {};
    new_pos = {};

    CHECK(event.type() == EventTypes::Move);
    CHECK_FALSE(event.accepted());
    CHECK(event.old_pos() == Point{.x = -3, .y = 2});
    CHECK(event.new_pos() == Point{.x = 5, .y = -4});
}

TEST_CASE("TUI resize events retain copies of both sizes", "[tui][event]")
{
    Size              old_size{.width = 10, .height = 4};
    Size              new_size{.width = 6, .height = 8};
    ResizeEvent const event{old_size, new_size};

    old_size = {};
    new_size = {};

    CHECK(event.type() == EventTypes::Resize);
    CHECK_FALSE(event.accepted());
    CHECK(event.old_size() == Size{.width = 10, .height = 4});
    CHECK(event.new_size() == Size{.width = 6, .height = 8});
}

TEST_CASE("TUI visibility events carry their type and start ignored", "[tui][event]")
{
    ShowEvent const show;
    HideEvent const hide;

    CHECK(show.type() == EventTypes::Show);
    CHECK(hide.type() == EventTypes::Hide);
    CHECK_FALSE(show.accepted());
    CHECK_FALSE(hide.accepted());
}

TEST_CASE("Core delivers TUI notifications synchronously while they are ignored", "[tui][event]")
{
    MoveEvent   move{Point{}, Point{.x = 1}};
    ResizeEvent resize{
        Size{},
        Size{.width = 1, .height = 1}
    };
    ShowEvent  show;
    HideEvent  hide;
    auto const notifications = std::array<Event*, 4>{&move, &resize, &show, &hide};

    for (auto* notification : notifications) {
        EventReceiver receiver;

        CHECK(Application::send_event(&receiver, *notification));
        CHECK(receiver.received == notification);
        CHECK_FALSE(notification->accepted());
    }
}
