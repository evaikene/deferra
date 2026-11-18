#include "widget.hpp"

#include "application.hpp"
#include "widget_priv.hpp"

#include <jb/core/application.hpp>
#include <jb/core/event.hpp>
#include <jb/core/logging.hpp>
#include <jb/core/object.hpp>
#include <jb/tui/events.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>

namespace jb::tui {
namespace {

auto clamp_screen_coordinate(std::int64_t position) -> int
{
    constexpr std::int64_t minimum = std::numeric_limits<int>::min();
    constexpr std::int64_t maximum = std::numeric_limits<int>::max();
    return static_cast<int>(std::clamp(position, minimum, maximum));
}

auto live_widget_child(Widget const& parent, jb::core::Object* owned_child) -> Widget*
{
    auto* child = dynamic_cast<Widget*>(owned_child);
    if (child && child->parent_widget() == &parent) {
        return child;
    }
    return nullptr;
}

} // namespace

class Widget::VisibilityNotifications {
public:

    VisibilityNotifications(Widget& widget, bool was_visible)
        : _visible(widget.is_visible_to_root())
    {
        if (_visible == was_visible) {
            return;
        }

        collect(widget);

        // The snapshot has stable entries from this point on. Destruction or a
        // newer effective transition can invalidate an entry during a callback.
        for (auto& recipient : _recipients) {
            recipient.snapshot_begin = _recipients.data();
            auto* data               = recipient.widget->d_ptr<priv::WidgetPrivate>();
            if (data->pending_visibility_recipient) {
                data->pending_visibility_recipient->widget = nullptr;
            }
            data->pending_visibility_recipient = &recipient;
        }
    }

    ~VisibilityNotifications()
    {
        for (auto const& recipient : _recipients) {
            if (recipient.widget) {
                recipient.widget->d_ptr<priv::WidgetPrivate>()->pending_visibility_recipient = nullptr;
            }
        }
    }

    VisibilityNotifications(VisibilityNotifications const&)                    = delete;
    auto operator=(VisibilityNotifications const&) -> VisibilityNotifications& = delete;

    void send()
    {
        for (auto& recipient : _recipients) {
            while (recipient.widget) {
                // A callback can reparent pending recipients. Recheck ancestry
                // before each delivery and keep the original order otherwise.
                auto* next =
                    _visible ? pending_show_ancestor(*recipient.widget) : first_pending_hide(*recipient.widget);
                dispatch(next ? *next : recipient);
            }
        }
    }

private:

    auto pending_recipient(Widget const& widget) const -> priv::VisibilityRecipient*
    {
        auto* recipient = widget.d_ptr<priv::WidgetPrivate const>()->pending_visibility_recipient;
        if (recipient && recipient->snapshot_begin == _recipients.data()) {
            return recipient;
        }
        return nullptr;
    }

    auto pending_show_ancestor(Widget const& widget) const -> priv::VisibilityRecipient*
    {
        priv::VisibilityRecipient* first = nullptr;
        for (auto const* parent = widget.parent_widget(); parent; parent = parent->parent_widget()) {
            if (auto* recipient = pending_recipient(*parent)) {
                first = recipient;
            }
        }
        return first;
    }

    auto first_pending_hide(Widget const& widget) const -> priv::VisibilityRecipient*
    {
        for (auto* child : widget.children()) {
            if (auto const* widget_child = live_widget_child(widget, child)) {
                if (auto* recipient = first_pending_hide(*widget_child)) {
                    return recipient;
                }
            }
        }
        return pending_recipient(widget);
    }

    void dispatch(priv::VisibilityRecipient& recipient) const
    {
        // Drop the pending entry before invoking user code, which may destroy
        // the recipient or start a new visibility change.
        auto* widget                                                       = std::exchange(recipient.widget, nullptr);
        widget->d_ptr<priv::WidgetPrivate>()->pending_visibility_recipient = nullptr;
        if (_visible) {
            ShowEvent event;
            jb::core::Application::send_event(widget, event);
        }
        else {
            HideEvent event;
            jb::core::Application::send_event(widget, event);
        }
    }

    void collect(Widget& widget)
    {
        if (_visible) {
            _recipients.push_back({.widget = &widget});
        }

        // A locally hidden child keeps its entire subtree effectively hidden.
        for (auto* child : widget.children()) {
            if (auto* widget_child = live_widget_child(widget, child); widget_child && widget_child->visible()) {
                collect(*widget_child);
            }
        }

        if (!_visible) {
            _recipients.push_back({.widget = &widget});
        }
    }

    bool const                             _visible;
    std::vector<priv::VisibilityRecipient> _recipients;
};

Widget::Widget(Widget* parent)
    : Object(*new priv::WidgetPrivate, parent)
{}

Widget::~Widget()
{
    auto const* data           = d_ptr<priv::WidgetPrivate const>();
    auto const& owned_children = children();
    if (!owned_children.empty()) {
        auto const visible_during_teardown = is_visible_to_root();

        // Core keeps children linked through its destroyed signal. Preserve
        // their visibility while stopping typed walks at the dying parent.
        for (auto* child : owned_children) {
            if (auto* widget = dynamic_cast<Widget*>(child)) {
                widget->d_ptr<priv::WidgetPrivate>()->parent_visibility_during_teardown = visible_during_teardown;
            }
        }
    }

    if (auto* recipient = data->pending_visibility_recipient) {
        recipient->widget = nullptr;
    }

    // Clear registration before Object emits destroyed from its base destructor.
    Application::unregister_destroyed_widget(this);
}

auto Widget::parent_widget() const -> Widget*
{
    if (d_ptr<priv::WidgetPrivate const>()->parent_visibility_during_teardown) {
        return nullptr;
    }

    return dynamic_cast<Widget*>(Object::parent());
}

auto Widget::child_widgets() const -> std::vector<Widget*>
{
    std::vector<Widget*> widgets;
    for (auto* child : children()) {
        if (auto* widget = live_widget_child(*this, child)) {
            widgets.push_back(widget);
        }
    }
    return widgets;
}

auto Widget::set_parent(Widget* parent) -> bool
{
    if (Object::parent() == parent) {
        return Object::set_parent(parent);
    }

    if (parent && Application::is_registered_main_widget(this)) {
        jb::core::log_fatal("Widget::set_parent: clear main-widget registration before reparenting");
        return false;
    }

    auto const was_visible = is_visible_to_root();

    if (!Object::set_parent(parent)) {
        return false;
    }

    d_ptr<priv::WidgetPrivate>()->parent_visibility_during_teardown.reset();
    VisibilityNotifications{*this, was_visible}.send();
    return true;
}

auto Widget::is_ancestor_of(Widget const* widget) const -> bool
{
    return widget != nullptr && widget->is_descendant_of(this);
}

auto Widget::is_descendant_of(Widget const* widget) const -> bool
{
    auto const* parent = parent_widget();
    return parent != nullptr && parent->is_descendant_of_or_same(widget);
}

auto Widget::is_descendant_of_or_same(Widget const* widget) const -> bool
{
    for (auto const* current = this; current; current = current->parent_widget()) {
        if (current == widget) {
            return true;
        }
    }
    return false;
}

auto Widget::geometry() const -> Rect
{
    return d_ptr<priv::WidgetPrivate const>()->geometry;
}

auto Widget::pos() const -> Point
{
    auto const bounds = geometry();
    return {.x = bounds.x, .y = bounds.y};
}

auto Widget::size() const -> Size
{
    auto const bounds = geometry();
    return {.width = bounds.width, .height = bounds.height};
}

auto Widget::width() const -> int
{
    return geometry().width;
}

auto Widget::height() const -> int
{
    return geometry().height;
}

auto Widget::rect() const -> Rect
{
    auto const dimensions = size();
    return {.width = dimensions.width, .height = dimensions.height};
}

auto Widget::screen_rect() const -> Rect
{
    auto         result = geometry();
    std::int64_t x      = result.x;
    std::int64_t y      = result.y;

    // Opposite offsets can cancel, so clamp only after summing the entire chain.
    for (auto const* ancestor = parent_widget(); ancestor; ancestor = ancestor->parent_widget()) {
        auto const offset  = ancestor->pos();
        x                 += offset.x;
        y                 += offset.y;
    }

    result.x = clamp_screen_coordinate(x);
    result.y = clamp_screen_coordinate(y);
    return result;
}

auto Widget::set_geometry(Rect geometry) -> void
{
    if (geometry.width < 0 || geometry.height < 0) {
        jb::core::log_fatal("Widget::set_geometry: width and height must be nonnegative");
        geometry.width  = std::max(geometry.width, 0);
        geometry.height = std::max(geometry.height, 0);
    }

    auto*      data         = d_ptr<priv::WidgetPrivate>();
    auto const old_geometry = data->geometry;
    if (old_geometry == geometry) {
        return;
    }

    // Commit the new bounds before notifying. Keep both snapshots on the stack
    // because a direct slot may change the stored geometry again.
    data->geometry = geometry;
    emit(geometry_changed, geometry);

    Point const old_pos{.x = old_geometry.x, .y = old_geometry.y};
    Point const new_pos{.x = geometry.x, .y = geometry.y};
    if (old_pos != new_pos) {
        MoveEvent notification{old_pos, new_pos};
        jb::core::Application::send_event(this, notification);
    }

    Size const old_size{.width = old_geometry.width, .height = old_geometry.height};
    Size const new_size{.width = geometry.width, .height = geometry.height};
    if (old_size != new_size) {
        ResizeEvent notification{old_size, new_size};
        jb::core::Application::send_event(this, notification);
    }
}

auto Widget::move(Point pos) -> void
{
    auto bounds = geometry();
    bounds.x    = pos.x;
    bounds.y    = pos.y;
    set_geometry(bounds);
}

auto Widget::resize(Size size) -> void
{
    auto bounds   = geometry();
    bounds.width  = size.width;
    bounds.height = size.height;
    set_geometry(bounds);
}

auto Widget::visible() const -> bool
{
    return d_ptr<priv::WidgetPrivate const>()->visible;
}

auto Widget::is_visible() const -> bool
{
    return visible();
}

auto Widget::is_visible_to_root() const -> bool
{
    for (auto const* widget = this; widget; widget = widget->parent_widget()) {
        auto const* data = widget->d_ptr<priv::WidgetPrivate const>();
        if (!data->visible) {
            return false;
        }

        // Core can clear the parent link before the subclass destructor runs.
        // A successful typed parent change clears the saved visibility.
        if (data->parent_visibility_during_teardown) {
            return *data->parent_visibility_during_teardown;
        }
    }
    return true;
}

auto Widget::show() -> bool
{
    return set_visible(true);
}

auto Widget::hide() -> void
{
    set_visible(false);
}

auto Widget::set_visible(bool visible) -> bool
{
    // Explicit unregistration can leave a root locally visible, so validate
    // registration before handling an unchanged visibility flag.
    if (visible && !Object::parent()) {
        auto* app = Application::instance();
        if (!app) {
            jb::core::log_fatal("Widget::set_visible: showing a root widget requires a TUI application");
            return false;
        }
        if (!app->register_shown_top_level(this)) {
            return false;
        }
    }

    auto* data = d_ptr<priv::WidgetPrivate>();
    if (data->visible == visible) {
        return true;
    }

    auto const was_visible = is_visible_to_root();
    data->visible          = visible;

    VisibilityNotifications notifications{*this, was_visible};
    emit(visible_changed, visible);
    notifications.send();
    return true;
}

auto Widget::event(jb::core::Event& event) -> bool
{
    // A numeric event ID alone does not establish the event's payload type.
    switch (event.type()) {
        case EventTypes::Move:
            if (auto* notification = dynamic_cast<MoveEvent*>(&event)) {
                move_event(*notification);
                return true;
            }
            break;
        case EventTypes::Resize:
            if (auto* notification = dynamic_cast<ResizeEvent*>(&event)) {
                resize_event(*notification);
                return true;
            }
            break;
        case EventTypes::Show:
            if (auto* notification = dynamic_cast<ShowEvent*>(&event)) {
                show_event(*notification);
                return true;
            }
            break;
        case EventTypes::Hide:
            if (auto* notification = dynamic_cast<HideEvent*>(&event)) {
                hide_event(*notification);
                return true;
            }
            break;
        default:
            break;
    }

    return Object::event(event);
}

auto Widget::move_event(MoveEvent& /* event */) -> void
{}

auto Widget::resize_event(ResizeEvent& /* event */) -> void
{}

auto Widget::show_event(ShowEvent& /* event */) -> void
{}

auto Widget::hide_event(HideEvent& /* event */) -> void
{}

} // namespace jb::tui
