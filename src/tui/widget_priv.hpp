#pragma once

#include <jb/core/object_priv.hpp>
#include <jb/tui/geometry.hpp>

#include <optional>

namespace jb::tui {

class Widget;

namespace priv {

struct VisibilityRecipient {
    Widget* widget;

    // The first entry identifies the snapshot even after it has been consumed.
    VisibilityRecipient const* snapshot_begin = nullptr;
};

struct WidgetPrivate : jb::core::priv::ObjectPrivate {
    Rect geometry{};
    bool visible = false;

    // The dying parent's last effective visibility. Ends typed ancestry at this link.
    std::optional<bool> parent_visibility_during_teardown;

    // Borrows an entry in a synchronous visibility notification snapshot.
    VisibilityRecipient* pending_visibility_recipient = nullptr;
};

} // namespace priv
} // namespace jb::tui
