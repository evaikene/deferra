#include <jb/tui/widget.hpp>

#include "support/operational_log_capture.hpp"

#include <jb/core/application.hpp>
#include <jb/core/event.hpp>
#include <jb/core/logging.hpp>
#include <jb/core/object.hpp>
#include <jb/core/signal.hpp>
#include <jb/tui/application.hpp>
#include <jb/tui/events.hpp>
#include <jb/tui/geometry.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

using jb::core::Application;
using jb::core::Event;
using jb::core::LogLevel;
using jb::core::Object;
using jb::core::Signal;
using jb::test::OperationalLogGuard;
using jb::tui::EventTypes;
using jb::tui::HideEvent;
using jb::tui::MoveEvent;
using jb::tui::Point;
using jb::tui::Rect;
using jb::tui::ResizeEvent;
using jb::tui::ShowEvent;
using jb::tui::Size;
using jb::tui::Widget;
using TuiApplication = jb::tui::Application;

namespace {

class TrackedWidget : public Widget {
public:

    using Widget::Widget;

    ~TrackedWidget() override
    {
        ++destroyed_count;
        if (!parent_widget()) {
            ++detached_destruction_count;
        }
    }

    static inline int destroyed_count            = 0;
    static inline int detached_destruction_count = 0;
};

class RecordingWidget : public Widget {
public:

    using Widget::Widget;

    Signal<MoveEvent>   moved;
    Signal<ResizeEvent> resized;
    Signal<bool>        visibility_notified;

protected:

    auto move_event(MoveEvent& event) -> void override { emit(moved, event); }

    auto resize_event(ResizeEvent& event) -> void override { emit(resized, event); }

    auto show_event(ShowEvent& /* event */) -> void override { emit(visibility_notified, true); }

    auto hide_event(HideEvent& /* event */) -> void override { emit(visibility_notified, false); }
};

class TeardownWidget : public Widget {
public:

    using Widget::Widget;

    ~TeardownWidget() override { emit(tearing_down); }

    Signal<> tearing_down;
};

enum class GeometryNotification : std::uint8_t {
    Changed,
    Move,
    Resize,
};

struct VisibilityObservation {
    Widget const* widget;
    bool          visible;

    auto operator==(VisibilityObservation const&) const -> bool = default;
};

void observe_effective_visibility(RecordingWidget& widget, std::vector<VisibilityObservation>& observations)
{
    widget.visibility_notified.connect(&widget, [&widget, &observations](bool visible) -> void {
        CHECK(widget.is_visible_to_root() == visible);
        observations.push_back({.widget = &widget, .visible = visible});
    });
}

} // namespace

TEST_CASE("Widget parentage uses the core child order", "[tui][widget]")
{
    Widget root;
    auto*  first        = new Widget{&root};
    auto*  object_child = new Object{&root};
    auto*  second       = new Widget{&root};

    CHECK(root.parent_widget() == nullptr);
    CHECK(first->parent_widget() == &root);
    CHECK(first->parent() == &root);
    CHECK(root.child_widgets() == std::vector<Widget*>{first, second});

    REQUIRE(first->set_parent(&root));

    CHECK(root.children() == std::vector<Object*>{first, object_child, second});
    CHECK(first->parent_widget() == &root);
}

TEST_CASE("Widget reparenting and detaching update the core and typed parent links", "[tui][widget]")
{
    Widget old_parent;
    Widget new_parent;
    auto*  existing_child = new Widget{&new_parent};
    auto*  child          = new Widget{&old_parent};

    REQUIRE(child->set_parent(&new_parent));

    CHECK(old_parent.children().empty());
    CHECK(new_parent.child_widgets() == std::vector<Widget*>{existing_child, child});
    CHECK(child->parent_widget() == &new_parent);
    CHECK(child->parent() == &new_parent);

    REQUIRE(child->set_parent(nullptr));
    auto const detached_child = std::unique_ptr<Widget>{child};

    CHECK(detached_child->parent_widget() == nullptr);
    CHECK(detached_child->parent() == nullptr);
    CHECK(new_parent.child_widgets() == std::vector<Widget*>{existing_child});
}

TEST_CASE("Widget rejects parenting cycles without changing the tree", "[tui][widget]")
{
    OperationalLogGuard logs;
    logs.capture->set_level(LogLevel::Fatal);
    Widget root;
    auto*  child      = new Widget{&root};
    auto*  grandchild = new Widget{child};

    SECTION("The proposed parent is the widget itself")
    {
        CHECK_FALSE(child->set_parent(child));
    }
    SECTION("The proposed parent is a descendant")
    {
        CHECK_FALSE(child->set_parent(grandchild));
    }

    CHECK(logs.capture->records().size() == 1);
    CHECK(child->parent_widget() == &root);
    CHECK(child->parent() == &root);
    CHECK(grandchild->parent_widget() == child);
    CHECK(root.child_widgets() == std::vector<Widget*>{child});
    CHECK(child->child_widgets() == std::vector<Widget*>{grandchild});
}

TEST_CASE("Widget leaves its typed parent intact when core rejects an affinity mismatch", "[tui][widget]")
{
    OperationalLogGuard logs;
    logs.capture->set_level(LogLevel::Fatal);
    Widget old_parent;
    auto*  child = new Widget{&old_parent};

    // The existing tree has no affinity because it predates the application event loop.
    Application app{0, nullptr};
    Widget      new_parent;
    REQUIRE(child->thread_ctx() != new_parent.thread_ctx());

    CHECK_FALSE(child->set_parent(&new_parent));

    CHECK(logs.capture->records().size() == 1);
    CHECK(child->parent_widget() == &old_parent);
    CHECK(child->parent() == &old_parent);
    CHECK(old_parent.child_widgets() == std::vector<Widget*>{child});
    CHECK(new_parent.children().empty());
}

TEST_CASE("Widget ancestry distinguishes ancestors, self, and unrelated widgets", "[tui][widget]")
{
    Widget root;
    auto*  child      = new Widget{&root};
    auto*  grandchild = new Widget{child};
    Widget other;

    CHECK(root.is_ancestor_of(grandchild));
    CHECK(child->is_ancestor_of(grandchild));
    CHECK(grandchild->is_descendant_of(&root));
    CHECK(grandchild->is_descendant_of_or_same(&root));
    CHECK(grandchild->is_descendant_of_or_same(grandchild));
    CHECK(root.is_descendant_of_or_same(&root));

    CHECK_FALSE(child->is_ancestor_of(child));
    CHECK_FALSE(child->is_descendant_of(child));
    CHECK_FALSE(grandchild->is_ancestor_of(&root));
    CHECK_FALSE(child->is_ancestor_of(&other));
    CHECK_FALSE(child->is_descendant_of_or_same(&other));
    CHECK_FALSE(child->is_ancestor_of(nullptr));
    CHECK_FALSE(child->is_descendant_of(nullptr));
    CHECK_FALSE(child->is_descendant_of_or_same(nullptr));
}

TEST_CASE("Widget ownership recursively detaches and deletes heap children", "[tui][widget]")
{
    TrackedWidget::destroyed_count            = 0;
    TrackedWidget::detached_destruction_count = 0;

    {
        Widget root;
        auto*  child = new TrackedWidget{&root};
        new TrackedWidget{child};
    }

    CHECK(TrackedWidget::destroyed_count == 2);
    CHECK(TrackedWidget::detached_destruction_count == 2);
}

TEST_CASE("A child can query and detach during widget parent teardown", "[tui][widget]")
{
    OperationalLogGuard logs;
    logs.capture->set_level(LogLevel::Fatal);

    auto       parent         = std::make_unique<Widget>();
    auto*      child          = new Widget{parent.get()};
    auto const child_geometry = Rect{.x = 2, .y = -3, .width = 4, .height = 5};
    parent->move({.x = 10, .y = 20});
    child->set_geometry(child_geometry);
    REQUIRE(child->show());
    CHECK_FALSE(child->is_visible_to_root());

    std::unique_ptr<Widget> detached;
    parent->destroyed.connect(child, [child, child_geometry, &detached]() -> void {
        CHECK(child->parent() != nullptr);
        auto* typed_parent = child->parent_widget();
        CHECK(typed_parent == nullptr);
        if (typed_parent) {
            return;
        }

        CHECK(child->screen_rect() == child_geometry);
        CHECK_FALSE(child->is_visible_to_root());
        CHECK(child->show());
        CHECK(child->set_parent(nullptr));
        if (!child->parent()) {
            detached.reset(child);
        }
    });

    parent.reset();

    REQUIRE(detached);
    CHECK(detached->parent_widget() == nullptr);
    CHECK(detached->screen_rect() == child_geometry);
    CHECK(detached->is_visible_to_root());
    CHECK(logs.capture->records().empty());
}

TEST_CASE("Widget geometry starts empty and exposes its signed local coordinates", "[tui][widget][geometry]")
{
    Widget widget;

    CHECK(widget.geometry() == Rect{});
    CHECK(widget.pos() == Point{});
    CHECK(widget.size() == Size{});
    CHECK(widget.width() == 0);
    CHECK(widget.height() == 0);
    CHECK(widget.rect() == Rect{});
    CHECK(widget.screen_rect() == Rect{});

    auto const geometry = Rect{.x = -3, .y = -2, .width = 7, .height = 9};
    widget.set_geometry(geometry);

    CHECK(widget.geometry() == geometry);
    CHECK(widget.pos() == Point{.x = -3, .y = -2});
    CHECK(widget.size() == Size{.width = 7, .height = 9});
    CHECK(widget.width() == 7);
    CHECK(widget.height() == 9);
    CHECK(widget.rect() == Rect{.width = 7, .height = 9});
    CHECK(widget.screen_rect() == geometry);
}

TEST_CASE("Widget screen geometry includes ancestor positions and keeps its dimensions", "[tui][widget][geometry]")
{
    Widget root;
    auto*  child      = new Widget{&root};
    auto*  grandchild = new Widget{child};

    root.set_geometry({.x = -4, .y = 7, .width = 2, .height = 1});
    child->set_geometry({.x = 5, .y = -3, .width = 1, .height = 2});
    grandchild->set_geometry({.x = -2, .y = -1, .width = 8, .height = 9});

    CHECK(grandchild->screen_rect() == Rect{.x = -1, .y = 3, .width = 8, .height = 9});

    root.move({.x = 1, .y = -8});
    CHECK(grandchild->screen_rect() == Rect{.x = 4, .y = -12, .width = 8, .height = 9});
}

TEST_CASE("Widget screen coordinates saturate only after adding all offsets", "[tui][widget][geometry]")
{
    constexpr auto maximum = std::numeric_limits<int>::max();
    constexpr auto minimum = std::numeric_limits<int>::min();

    Widget root;
    auto*  child      = new Widget{&root};
    auto*  grandchild = new Widget{child};
    child->move({.x = maximum, .y = minimum});
    grandchild->set_geometry({.x = 10, .y = -10, .width = 8, .height = 9});

    CHECK(grandchild->screen_rect() == Rect{.x = maximum, .y = minimum, .width = 8, .height = 9});

    root.move({.x = -12, .y = 12});
    CHECK(grandchild->screen_rect() == Rect{.x = maximum - 2, .y = minimum + 2, .width = 8, .height = 9});
}

TEST_CASE("Widget stores geometry before emitting the signal and changed component events", "[tui][widget][geometry]")
{
    auto const                        original = Rect{.x = -3, .y = 5, .width = 4, .height = 8};
    auto                              expected = original;
    std::vector<GeometryNotification> notifications;
    RecordingWidget                   widget;
    widget.set_geometry(original);

    widget.geometry_changed.connect(&widget, [&](Rect const& bounds) -> void {
        notifications.push_back(GeometryNotification::Changed);
        CHECK(bounds == expected);
        CHECK(widget.geometry() == expected);
    });
    widget.moved.connect(&widget, [&](MoveEvent const& event) -> void {
        notifications.push_back(GeometryNotification::Move);
        CHECK(event.old_pos() == Point{.x = original.x, .y = original.y});
        CHECK(event.new_pos() == Point{.x = expected.x, .y = expected.y});
        CHECK(widget.geometry() == expected);
    });
    widget.resized.connect(&widget, [&](ResizeEvent const& event) -> void {
        notifications.push_back(GeometryNotification::Resize);
        CHECK(event.old_size() == Size{.width = original.width, .height = original.height});
        CHECK(event.new_size() == Size{.width = expected.width, .height = expected.height});
        CHECK(widget.geometry() == expected);
    });

    SECTION("Changing position and size sends Move before Resize")
    {
        expected = {.x = 6, .y = -7, .width = 9, .height = 3};
        widget.set_geometry(expected);

        CHECK(notifications ==
              std::vector{GeometryNotification::Changed, GeometryNotification::Move, GeometryNotification::Resize});
    }
    SECTION("Moving preserves size and sends a Move event")
    {
        expected.x = 6;
        expected.y = -7;
        widget.move({.x = expected.x, .y = expected.y});

        CHECK(notifications == std::vector{GeometryNotification::Changed, GeometryNotification::Move});
    }
    SECTION("Resizing preserves position and sends a Resize event")
    {
        expected.width  = 9;
        expected.height = 3;
        widget.resize({.width = expected.width, .height = expected.height});

        CHECK(notifications == std::vector{GeometryNotification::Changed, GeometryNotification::Resize});
    }
    SECTION("An unchanged rectangle emits nothing")
    {
        widget.set_geometry(original);
        CHECK(notifications.empty());
    }

    CHECK(widget.geometry() == expected);
}

TEST_CASE("A nested geometry change preserves each notification's payload", "[tui][widget][geometry]")
{
    auto const         first  = Rect{.x = 7};
    auto const         second = Rect{.x = 11};
    std::vector<Rect>  observed_geometry;
    std::vector<Point> observed_moves;
    RecordingWidget    widget;

    widget.geometry_changed.connect(&widget, [&](Rect const& bounds) -> void {
        if (bounds == first) {
            widget.set_geometry(second);
        }
    });
    widget.geometry_changed.connect(
        [&observed_geometry](Rect const& bounds) -> void { observed_geometry.push_back(bounds); });
    widget.moved.connect(
        [&observed_moves](MoveEvent const& event) -> void { observed_moves.push_back(event.new_pos()); });

    widget.set_geometry(first);

    CHECK(widget.geometry() == second);
    CHECK(observed_geometry == std::vector{second, first});
    CHECK(observed_moves == std::vector{Point{.x = 11}, Point{.x = 7}});
}

TEST_CASE("Widget clamps negative dimensions before comparing or notifying", "[tui][widget][geometry]")
{
    OperationalLogGuard logs;
    logs.capture->set_level(LogLevel::Fatal);
    std::vector<Rect> notified_geometry;
    Widget            widget;
    widget.geometry_changed.connect(
        [&notified_geometry](Rect const& bounds) -> void { notified_geometry.push_back(bounds); });

    widget.set_geometry({.x = -4, .y = -2, .width = -1, .height = 5});
    widget.resize({.width = -2, .height = 5});
    widget.resize({.width = 6, .height = -3});

    CHECK(logs.capture->records().size() == 3);
    CHECK(notified_geometry == std::vector{
                                   Rect{.x = -4, .y = -2, .height = 5},
                                   Rect{.x = -4, .y = -2, .width = 6 }
    });
    CHECK(widget.geometry() == Rect{.x = -4, .y = -2, .width = 6});
}

TEST_CASE("Widget recognizes typed geometry events independently of acceptance", "[tui][widget][event]")
{
    Widget      widget;
    MoveEvent   move{Point{}, Point{.x = 1}};
    ResizeEvent resize{Size{}, Size{.width = 1}};
    resize.accept();

    CHECK(widget.event(move));
    CHECK_FALSE(move.accepted());
    CHECK(widget.event(resize));
    CHECK(resize.accepted());

    Event unrelated{Event::User};
    Event untyped_move{EventTypes::Move};
    CHECK_FALSE(widget.event(unrelated));
    CHECK_FALSE(widget.event(untyped_move));
}

TEST_CASE("Widget visibility events follow the effective subtree in both directions", "[tui][widget][visibility]")
{
    std::vector<VisibilityObservation> events;
    std::vector<bool>                  root_changes;
    std::vector<bool>                  child_changes;
    TuiApplication                     app{0, nullptr};
    RecordingWidget                    root;
    auto*                              child         = new RecordingWidget{&root};
    auto*                              grandchild    = new RecordingWidget{child};
    auto*                              sibling       = new RecordingWidget{&root};
    auto*                              hidden        = new RecordingWidget{&root};
    auto*                              behind_hidden = new RecordingWidget{hidden};

    for (auto* widget : {&root, child, grandchild, sibling, hidden, behind_hidden}) {
        observe_effective_visibility(*widget, events);
    }
    root.visible_changed.connect(&root, [&](bool visible) -> void {
        CHECK(root.visible() == visible);
        CHECK(root.is_visible_to_root() == visible);
        CHECK(app.main_widget() == &root);
        CHECK(events.empty());
        root_changes.push_back(visible);
    });
    child->visible_changed.connect(child, [&](bool visible) -> void {
        CHECK(child->visible() == visible);
        child_changes.push_back(visible);
    });

    CHECK_FALSE(root.visible());
    CHECK_FALSE(root.is_visible());
    CHECK_FALSE(root.is_visible_to_root());
    root.hide();
    REQUIRE(child->show());
    REQUIRE(grandchild->show());
    REQUIRE(sibling->show());
    REQUIRE(behind_hidden->show());

    CHECK(child->is_visible());
    CHECK_FALSE(child->is_visible_to_root());
    CHECK(child_changes == std::vector{true});
    CHECK(root_changes.empty());
    CHECK(events.empty());
    CHECK(app.main_widget() == nullptr);

    REQUIRE(root.show());
    REQUIRE(root.show());
    REQUIRE(child->show());

    CHECK(events == std::vector<VisibilityObservation>{
                        {&root,      true},
                        {child,      true},
                        {grandchild, true},
                        {sibling,    true}
    });
    CHECK(root_changes == std::vector{true});
    CHECK(child_changes == std::vector{true});
    CHECK(behind_hidden->visible());
    CHECK_FALSE(behind_hidden->is_visible_to_root());

    events.clear();
    root.hide();
    root.hide();

    CHECK(events == std::vector<VisibilityObservation>{
                        {grandchild, false},
                        {child,      false},
                        {sibling,    false},
                        {&root,      false}
    });
    CHECK(root_changes == std::vector{true, false});
    CHECK(child_changes == std::vector{true});
    CHECK(child->visible());
    CHECK_FALSE(child->is_visible_to_root());
    CHECK(app.main_widget() == &root);

    events.clear();
    child->hide();
    child->hide();

    CHECK(child_changes == std::vector{true, false});
    CHECK_FALSE(child->visible());
    CHECK(events.empty());
}

TEST_CASE("Showing a root without a TUI application leaves it hidden", "[tui][widget][visibility]")
{
    OperationalLogGuard logs;
    logs.capture->set_level(LogLevel::Fatal);
    std::vector<VisibilityObservation> events;
    std::vector<bool>                  changes;
    RecordingWidget                    root;
    observe_effective_visibility(root, events);
    root.visible_changed.connect([&changes](bool visible) -> void { changes.push_back(visible); });

    SECTION("There is no core application")
    {
        CHECK_FALSE(root.show());
    }
    SECTION("The core application is not a TUI application")
    {
        Application app{0, nullptr};
        CHECK_FALSE(root.show());
    }

    CHECK(logs.capture->records().size() == 1);
    CHECK_FALSE(root.visible());
    CHECK_FALSE(root.is_visible_to_root());
    CHECK(changes.empty());
    CHECK(events.empty());
}

TEST_CASE("Showing an already visible root revalidates main-widget registration", "[tui][widget][visibility]")
{
    OperationalLogGuard logs;
    logs.capture->set_level(LogLevel::Fatal);
    std::vector<VisibilityObservation> events;
    std::vector<bool>                  second_changes;
    TuiApplication                     app{0, nullptr};
    RecordingWidget                    first;
    RecordingWidget                    second;
    observe_effective_visibility(first, events);
    observe_effective_visibility(second, events);
    second.visible_changed.connect([&second_changes](bool visible) -> void { second_changes.push_back(visible); });

    REQUIRE(first.show());
    CHECK(app.main_widget() == &first);
    CHECK_FALSE(second.show());
    CHECK_FALSE(second.visible());
    CHECK(second_changes.empty());
    CHECK(app.main_widget() == &first);

    REQUIRE(app.set_main_widget(nullptr));
    CHECK(first.visible());
    CHECK(first.is_visible_to_root());
    REQUIRE(first.show());
    CHECK(app.main_widget() == &first);

    REQUIRE(app.set_main_widget(&second));
    CHECK_FALSE(first.show());
    CHECK(first.visible());
    CHECK(first.is_visible_to_root());
    CHECK_FALSE(second.visible());
    CHECK(app.main_widget() == &second);
    CHECK(second_changes.empty());
    CHECK(events == std::vector<VisibilityObservation>{
                        {&first, true}
    });
    CHECK(logs.capture->records().size() == 2);
}

TEST_CASE("Widget reparenting notifies effective visibility without changing local state", "[tui][widget][visibility]")
{
    std::vector<VisibilityObservation> events;
    std::vector<bool>                  local_changes;
    TuiApplication                     app{0, nullptr};
    Widget                             root;
    auto*                              first_parent  = new Widget{&root};
    auto*                              second_parent = new Widget{&root};
    auto*                              hidden_parent = new Widget{&root};
    auto*                              child         = new RecordingWidget{first_parent};
    auto*                              grandchild    = new RecordingWidget{child};
    REQUIRE(root.show());
    REQUIRE(first_parent->show());
    REQUIRE(second_parent->show());
    REQUIRE(child->show());
    REQUIRE(grandchild->show());

    observe_effective_visibility(*child, events);
    observe_effective_visibility(*grandchild, events);
    child->visible_changed.connect([&local_changes](bool visible) -> void { local_changes.push_back(visible); });

    REQUIRE(child->set_parent(hidden_parent));
    CHECK(child->parent_widget() == hidden_parent);
    CHECK(child->visible());
    CHECK(grandchild->visible());
    CHECK(events == std::vector<VisibilityObservation>{
                        {grandchild, false},
                        {child,      false}
    });

    events.clear();
    REQUIRE(child->set_parent(second_parent));
    CHECK(child->parent_widget() == second_parent);
    CHECK(events == std::vector<VisibilityObservation>{
                        {child,      true},
                        {grandchild, true}
    });

    events.clear();
    REQUIRE(child->set_parent(first_parent));
    REQUIRE(child->set_parent(first_parent));
    CHECK(child->parent_widget() == first_parent);
    CHECK(events.empty());
    CHECK(local_changes.empty());
}

TEST_CASE("Pending visibility events follow ancestry after reparenting", "[tui][widget][visibility]")
{
    std::vector<VisibilityObservation> events;
    TuiApplication                     app{0, nullptr};
    RecordingWidget                    root;
    auto*                              first  = new RecordingWidget{&root};
    auto*                              second = new RecordingWidget{&root};
    REQUIRE(first->show());
    REQUIRE(second->show());

    for (auto* widget : {&root, first, second}) {
        observe_effective_visibility(*widget, events);
    }

    SECTION("Show visits the new parent before its child")
    {
        auto const reparent = [first, second](bool visible) -> void {
            if (visible) {
                CHECK(first->set_parent(second));
            }
        };
        SECTION("Reparenting in the local visibility signal")
        {
            root.visible_changed.connect(first, reparent);
        }
        SECTION("Reparenting after delivery has started")
        {
            root.visibility_notified.connect(first, reparent);
        }
        first->visibility_notified.connect(first, [first, second](bool /* visible */) -> void {
            CHECK(first->parent_widget() == second);
        });

        REQUIRE(root.show());

        CHECK(events == std::vector<VisibilityObservation>{
                            {&root,  true},
                            {second, true},
                            {first,  true}
        });
    }

    SECTION("Hide visits the reparented child before its new parent")
    {
        REQUIRE(root.show());
        events.clear();
        root.visible_changed.connect(second, [first, second](bool visible) -> void {
            if (!visible) {
                CHECK(second->set_parent(first));
            }
        });
        second->visibility_notified.connect(second, [first, second](bool /* visible */) -> void {
            CHECK(second->parent_widget() == first);
        });

        root.hide();

        CHECK(events == std::vector<VisibilityObservation>{
                            {second, false},
                            {first,  false},
                            {&root,  false}
        });
    }
}

TEST_CASE("A registered main widget rejects a parent without changing visibility", "[tui][widget][visibility]")
{
    OperationalLogGuard logs;
    logs.capture->set_level(LogLevel::Fatal);
    std::vector<VisibilityObservation> events;
    TuiApplication                     app{0, nullptr};
    Widget                             candidate_parent;
    auto                               main = std::make_unique<RecordingWidget>();
    REQUIRE(main->show());
    observe_effective_visibility(*main, events);

    CHECK_FALSE(main->set_parent(&candidate_parent));

    CHECK(logs.capture->records().size() == 1);
    CHECK(app.main_widget() == main.get());
    CHECK(main->parent() == nullptr);
    CHECK(main->parent_widget() == nullptr);
    CHECK(candidate_parent.children().empty());
    CHECK(main->is_visible_to_root());
    CHECK(events.empty());
}

TEST_CASE("Detaching a visible child leaves main-widget registration to a later show", "[tui][widget][visibility]")
{
    std::vector<VisibilityObservation> events;
    TuiApplication                     app{0, nullptr};
    Widget                             hidden_root;
    auto*                              child = new RecordingWidget{&hidden_root};
    observe_effective_visibility(*child, events);
    REQUIRE(child->show());
    CHECK(events.empty());

    REQUIRE(child->set_parent(nullptr));
    auto detached = std::unique_ptr<RecordingWidget>{child};

    CHECK(app.main_widget() == nullptr);
    CHECK(detached->is_visible_to_root());
    CHECK(events == std::vector<VisibilityObservation>{
                        {detached.get(), true}
    });

    REQUIRE(detached->show());
    CHECK(app.main_widget() == detached.get());
    CHECK(events == std::vector<VisibilityObservation>{
                        {detached.get(), true}
    });
}

TEST_CASE("Nested visibility transitions supersede a pending event for the same widget", "[tui][widget][visibility]")
{
    std::vector<VisibilityObservation> events;
    TuiApplication                     app{0, nullptr};
    RecordingWidget                    root;
    auto*                              child = new RecordingWidget{&root};
    REQUIRE(child->show());

    observe_effective_visibility(root, events);
    observe_effective_visibility(*child, events);
    root.visibility_notified.connect(child, [child](bool visible) -> void {
        if (visible) {
            child->hide();
            CHECK(child->show());
        }
    });

    REQUIRE(root.show());

    CHECK(events == std::vector<VisibilityObservation>{
                        {&root, true },
                        {child, false},
                        {child, true }
    });
}

TEST_CASE("Visibility delivery skips a pending recipient destroyed by a callback", "[tui][widget][visibility]")
{
    std::vector<VisibilityObservation> events;
    TuiApplication                     app{0, nullptr};
    RecordingWidget                    root;
    auto*                              doomed    = new RecordingWidget{&root};
    auto*                              surviving = new RecordingWidget{&root};
    REQUIRE(doomed->show());
    REQUIRE(surviving->show());

    observe_effective_visibility(root, events);
    observe_effective_visibility(*surviving, events);
    root.visible_changed.connect(doomed, [doomed](bool visible) -> void {
        if (visible) {
            delete doomed;
        }
    });

    REQUIRE(root.show());

    CHECK(root.child_widgets() == std::vector<Widget*>{surviving});
    CHECK(events == std::vector<VisibilityObservation>{
                        {&root,     true},
                        {surviving, true}
    });
}

TEST_CASE("Widget rejects invalid ownership changes during teardown", "[tui][widget][visibility]")
{
    OperationalLogGuard logs;
    logs.capture->set_level(LogLevel::Fatal);

    std::vector<VisibilityObservation> events;
    TuiApplication                     app{0, nullptr};
    RecordingWidget                    root;
    auto*                              owner      = new Widget{&root};
    Object*                            core_owner = owner;
    REQUIRE(root.show());
    REQUIRE(owner->show());
    observe_effective_visibility(root, events);

    root.visible_changed.connect(owner, [owner](bool visible) -> void {
        if (!visible) {
            delete owner;
        }
    });

    SECTION("Core ownership prevents cycles across the widget teardown boundary")
    {
        auto* child = new RecordingWidget{owner};
        REQUIRE(child->show());
        REQUIRE(app.set_main_widget(nullptr));
        observe_effective_visibility(*child, events);

        core_owner->destroyed.connect(child, [&root, &events, core_owner, child]() -> void {
            CHECK(child->parent_widget() == nullptr);
            CHECK(child->parent() == core_owner);

            CHECK_FALSE(root.set_parent(child));

            CHECK(root.parent() == nullptr);
            CHECK(root.children() == std::vector<Object*>{core_owner});
            CHECK(child->parent() == core_owner);
            CHECK_FALSE(child->is_visible_to_root());
            CHECK(events.empty());
        });

        root.hide();

        CHECK(app.main_widget() == nullptr);
        CHECK(logs.capture->records().size() == 1);
    }

    SECTION("Core rejects detach and reparent after recursive deletion has been committed")
    {
        Widget destination;
        auto*  first = new TeardownWidget{owner};
        auto*  later = new RecordingWidget{owner};
        REQUIRE(first->show());
        REQUIRE(later->show());
        observe_effective_visibility(*later, events);

        first->tearing_down.connect(later, [first, later, core_owner, &destination, &events]() -> void {
            CHECK(first->parent() == nullptr);
            CHECK(later->parent() == core_owner);
            CHECK(later->parent_widget() == nullptr);

            CHECK_FALSE(first->set_parent(nullptr));
            CHECK_FALSE(later->set_parent(nullptr));
            CHECK_FALSE(later->set_parent(&destination));

            CHECK(first->parent() == nullptr);
            CHECK(later->parent() == core_owner);
            CHECK(destination.children().empty());
            CHECK_FALSE(later->is_visible_to_root());
            CHECK(events.empty());
        });

        root.hide();

        CHECK(app.main_widget() == &root);
        CHECK(logs.capture->records().size() == 3);
    }

    CHECK(root.children().empty());
    CHECK(events == std::vector<VisibilityObservation>{
                        {&root, false}
    });
}

TEST_CASE("Detaching during parent teardown supersedes a pending Hide event", "[tui][widget][visibility]")
{
    enum class TeardownPath : std::uint8_t {
        DirectDestroyed,
        RecursiveDestroyed,
        RecursiveSubclassDestructor,
    };
    auto const path = GENERATE(TeardownPath::DirectDestroyed,
                               TeardownPath::RecursiveDestroyed,
                               TeardownPath::RecursiveSubclassDestructor);
    CAPTURE(path);

    std::vector<VisibilityObservation> events;
    TuiApplication                     app{0, nullptr};
    RecordingWidget                    root;
    REQUIRE(root.show());

    auto const recursive_teardown = path != TeardownPath::DirectDestroyed;
    auto*      outer_parent       = recursive_teardown ? new Widget{&root} : &root;
    if (recursive_teardown) {
        REQUIRE(outer_parent->show());
    }
    auto* parent = new TeardownWidget{outer_parent};
    REQUIRE(parent->show());
    auto* child = new RecordingWidget{parent};
    REQUIRE(child->show());
    REQUIRE(child->is_visible_to_root());

    observe_effective_visibility(root, events);
    observe_effective_visibility(*child, events);

    std::unique_ptr<Widget> detached;
    auto const              detach = [child, &detached, &events]() -> void {
        CHECK(child->parent() != nullptr);
        CHECK_FALSE(child->is_visible_to_root());
        CHECK(events.empty());

        CHECK(child->set_parent(nullptr));
        if (!child->parent()) {
            detached.reset(child);
        }
    };
    if (path == TeardownPath::RecursiveSubclassDestructor) {
        parent->tearing_down.connect(child, [parent, child, detach]() -> void {
            CHECK(parent->parent() == nullptr);
            CHECK(child->parent_widget() == parent);
            detach();
        });
    }
    else {
        parent->destroyed.connect(child, [child, detach]() -> void {
            CHECK(child->parent_widget() == nullptr);
            detach();
        });
    }

    auto* doomed = recursive_teardown ? outer_parent : parent;
    root.visible_changed.connect(doomed, [doomed](bool visible) -> void {
        if (!visible) {
            delete doomed;
        }
    });

    root.hide();

    REQUIRE(detached);
    CHECK(detached->is_visible_to_root());
    CHECK(events == std::vector<VisibilityObservation>{
                        {detached.get(), true },
                        {&root,          false}
    });
}

TEST_CASE("Widget recognizes typed visibility events independently of acceptance", "[tui][widget][event]")
{
    Widget    widget;
    ShowEvent show;
    HideEvent hide;
    hide.accept();

    CHECK(widget.event(show));
    CHECK_FALSE(show.accepted());
    CHECK(widget.event(hide));
    CHECK(hide.accepted());

    Event untyped_show{EventTypes::Show};
    CHECK_FALSE(widget.event(untyped_show));
}
