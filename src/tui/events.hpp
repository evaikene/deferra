/// @file events.hpp
/// @brief Synchronous notifications for changes to widget geometry and effective visibility.
///
/// Delivery and the reported change are independent of the inherited acceptance flag.
#pragma once

#include <jb/core/event.hpp>
#include <jb/tui/geometry.hpp>

namespace jb::tui {

/// Event type IDs reserved for TUI notifications in the core user range.
struct EventTypes {
    static constexpr jb::core::Event::Type TuiBase = jb::core::Event::User + 10'000;
    static constexpr auto                  Move    = TuiBase + 1;
    static constexpr auto                  Resize  = TuiBase + 2;
    static constexpr auto                  Show    = TuiBase + 3;
    static constexpr auto                  Hide    = TuiBase + 4;
};

/// Reports a change in widget position and owns copies of both positions.
class MoveEvent : public jb::core::Event {
public:

    MoveEvent(Point old_pos, Point new_pos)
        : Event(EventTypes::Move)
        , _old_pos(old_pos)
        , _new_pos(new_pos)
    {}

    [[nodiscard]] auto old_pos() const -> Point { return _old_pos; }

    [[nodiscard]] auto new_pos() const -> Point { return _new_pos; }

private:

    Point _old_pos;
    Point _new_pos;
};

/// Reports a change in widget size and owns copies of both sizes.
class ResizeEvent : public jb::core::Event {
public:

    ResizeEvent(Size old_size, Size new_size)
        : Event(EventTypes::Resize)
        , _old_size(old_size)
        , _new_size(new_size)
    {}

    [[nodiscard]] auto old_size() const -> Size { return _old_size; }

    [[nodiscard]] auto new_size() const -> Size { return _new_size; }

private:

    Size _old_size;
    Size _new_size;
};

/// Reports that a widget became effectively visible.
/// A widget is effectively visible when it and all of its ancestors are visible.
class ShowEvent : public jb::core::Event {
public:

    ShowEvent()
        : Event(EventTypes::Show)
    {}
};

/// Reports that a widget lost effective visibility.
class HideEvent : public jb::core::Event {
public:

    HideEvent()
        : Event(EventTypes::Hide)
    {}
};

} // namespace jb::tui
