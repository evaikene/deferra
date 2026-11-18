#pragma once

#include <jb/core/object.hpp>
#include <jb/core/signal.hpp>
#include <jb/tui/geometry.hpp>

#include <vector>

namespace jb::tui {

class HideEvent;
class MoveEvent;
class ResizeEvent;
class ShowEvent;

/// A TUI object whose widget parent also owns it in the core object tree.
/// Parent-owned widgets must be heap-allocated. Use Widget::set_parent() for
/// reparenting so the core and typed parent links stay in sync.
class Widget : public jb::core::Object {
public:

    /// Constructs a root widget or transfers ownership to a parent on the same thread.
    explicit Widget(Widget* parent = nullptr);
    ~Widget() override;

    /// Returns the owning widget parent, or nullptr for a root widget or a parent in teardown.
    [[nodiscard]] auto parent_widget() const -> Widget*;

    /// Returns borrowed pointers to live direct widget children in insertion order.
    [[nodiscard]] auto child_widgets() const -> std::vector<Widget*>;

    /// Transfers ownership to a same-thread parent, or detaches when passed nullptr.
    /// Returns true for a successful transfer or a valid same-parent request.
    /// Core validates ownership and rejects requests for widgets committed to deletion.
    /// Rejections log a fatal error and return false if execution continues,
    /// preserving parentage and visibility.
    /// Clear main-widget registration before attaching the main widget to a parent.
    /// A successful reparent sends notifications for changes in effective visibility.
    auto set_parent(Widget* parent) -> bool;

    /// Returns whether @p widget is a strict descendant of this widget.
    [[nodiscard]] auto is_ancestor_of(Widget const* widget) const -> bool;

    /// Returns whether @p widget is a strict ancestor of this widget.
    [[nodiscard]] auto is_descendant_of(Widget const* widget) const -> bool;

    /// Returns whether @p widget is this widget or one of its ancestors.
    [[nodiscard]] auto is_descendant_of_or_same(Widget const* widget) const -> bool;

    /// Returns the rectangle in parent coordinates, or screen coordinates for a root.
    [[nodiscard]] auto geometry() const -> Rect;
    [[nodiscard]] auto pos() const -> Point;
    [[nodiscard]] auto size() const -> Size;
    [[nodiscard]] auto width() const -> int;
    [[nodiscard]] auto height() const -> int;

    /// Returns the local rectangle with origin (0, 0).
    [[nodiscard]] auto rect() const -> Rect;

    /// Returns the rectangle in screen coordinates with the widget's own dimensions.
    /// The sum of ancestor offsets and the widget position saturates at the limits of int.
    [[nodiscard]] auto screen_rect() const -> Rect;

    /// Stores the new geometry and emits geometry_changed, then sends Move and Resize
    /// notifications in that order for the components that changed. An unchanged
    /// rectangle emits nothing. Negative dimensions log a fatal error and clamp to zero.
    ///
    /// Direct callbacks can initiate another update. Each notification carries the
    /// geometry change that produced it. Keep the widget alive through synchronous
    /// delivery. Use delete_later() to request destruction from a callback.
    auto set_geometry(Rect geometry) -> void;

    /// Changes the position while preserving the dimensions.
    auto move(Point pos) -> void;

    /// Changes the dimensions while preserving the position.
    auto resize(Size size) -> void;

    /// Returns the widget's own visibility flag, initially false.
    [[nodiscard]] auto visible() const -> bool;

    /// Returns the same local visibility as visible().
    [[nodiscard]] auto is_visible() const -> bool;

    /// Returns true when this widget and each widget ancestor are locally visible.
    /// Saved parent visibility applies through recursive teardown and is cleared
    /// when an explicit detach or reparent succeeds.
    [[nodiscard]] auto is_visible_to_root() const -> bool;

    /// Sets local visibility to true, with the registration rules of set_visible().
    auto show() -> bool;

    /// Sets local visibility to false.
    auto hide() -> void;

    /// Sets local visibility and returns true on success. Showing a root requires
    /// the active TUI application and registers it as the main widget, even when
    /// the root is already locally visible. A missing application or a different
    /// registered main widget logs a fatal error and returns false before mutation.
    ///
    /// The local signal is emitted before effective visibility notifications. For
    /// each transition, Show visits pending ancestors before descendants and Hide
    /// visits pending descendants before ancestors, using ancestry at delivery.
    /// A nested transition supersedes any pending effective event for that widget.
    auto set_visible(bool visible) -> bool;

    /// Dispatches recognized notifications and returns whether the event was handled.
    /// Recognition is independent of the event's accepted state.
    auto event(jb::core::Event& event) -> bool override;

    /// Emitted with the new parent-relative geometry before the move and resize events.
    jb::core::Signal<Rect> geometry_changed;

    /// Emitted when this widget's local visibility flag changes.
    jb::core::Signal<bool> visible_changed;

protected:

    virtual auto move_event(MoveEvent& event) -> void;
    virtual auto resize_event(ResizeEvent& event) -> void;
    virtual auto show_event(ShowEvent& event) -> void;
    virtual auto hide_event(HideEvent& event) -> void;

private:

    class VisibilityNotifications;
};

} // namespace jb::tui
