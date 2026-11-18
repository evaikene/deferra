#pragma once

namespace jb::tui {

/// A position in zero-based terminal-cell coordinates.
struct Point {
    int x = 0;
    int y = 0;

    auto operator==(Point const&) const -> bool = default;
};

/// Signed terminal-cell dimensions.
struct Size {
    int width  = 0;
    int height = 0;

    auto operator==(Size const&) const -> bool = default;
};

/// A terminal-cell rectangle that preserves its signed position and dimensions as supplied.
struct Rect {
    int x      = 0;
    int y      = 0;
    int width  = 0;
    int height = 0;

    /// Returns the exclusive right edge, clamped to the range of int.
    [[nodiscard]] auto right() const -> int;

    /// Returns the exclusive bottom edge, clamped to the range of int.
    [[nodiscard]] auto bottom() const -> int;

    /// Returns whether either dimension is zero or negative.
    [[nodiscard]] auto empty() const -> bool { return width <= 0 || height <= 0; }

    auto operator==(Rect const&) const -> bool = default;
};

/// Signed offsets for the four sides, measured in terminal cells.
struct Margins {
    int left   = 0;
    int top    = 0;
    int right  = 0;
    int bottom = 0;

    auto operator==(Margins const&) const -> bool = default;
};

} // namespace jb::tui
