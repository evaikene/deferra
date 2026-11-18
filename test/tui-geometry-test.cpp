#include <jb/tui/geometry.hpp>

#include <catch2/catch_test_macros.hpp>

#include <limits>

using jb::tui::Margins;
using jb::tui::Point;
using jb::tui::Rect;
using jb::tui::Size;

TEST_CASE("TUI geometry defaults to zero", "[tui][geometry]")
{
    Point const   point;
    Size const    size;
    Rect const    rect;
    Margins const margins;

    CHECK(point == Point{.x = 0, .y = 0});
    CHECK(size == Size{.width = 0, .height = 0});
    CHECK(rect == Rect{.x = 0, .y = 0, .width = 0, .height = 0});
    CHECK(margins == Margins{.left = 0, .top = 0, .right = 0, .bottom = 0});
}

TEST_CASE("TUI geometry equality compares signed values", "[tui][geometry]")
{
    auto const point = Point{.x = -3, .y = 5};
    CHECK(point == Point{.x = -3, .y = 5});
    CHECK(point != Point{.x = 3, .y = 5});

    auto const size = Size{.width = 4, .height = -2};
    CHECK(size == Size{.width = 4, .height = -2});
    CHECK(size != Size{.width = 4, .height = 2});

    auto const rect = Rect{.x = -3, .y = -5, .width = 4, .height = -2};
    CHECK(rect == Rect{.x = -3, .y = -5, .width = 4, .height = -2});
    CHECK(rect != Rect{.x = -3, .y = -5, .width = 4, .height = 2});

    auto const margins = Margins{.left = 1, .top = -2, .right = 3, .bottom = 4};
    CHECK(margins == Margins{.left = 1, .top = -2, .right = 3, .bottom = 4});
    CHECK(margins != Margins{.left = 1, .top = 2, .right = 3, .bottom = 4});
}

TEST_CASE("TUI rectangle edges are exclusive", "[tui][geometry]")
{
    auto const rect = Rect{.x = -5, .y = -7, .width = 3, .height = 4};
    CHECK(rect.right() == -2);
    CHECK(rect.bottom() == -3);

    auto const negative_dimensions = Rect{.x = 5, .y = 7, .width = -3, .height = -4};
    CHECK(negative_dimensions.right() == 2);
    CHECK(negative_dimensions.bottom() == 3);
}

TEST_CASE("TUI rectangles are empty if either dimension is nonpositive", "[tui][geometry]")
{
    CHECK(Rect{}.empty());
    CHECK(Rect{.width = 0, .height = 2}.empty());
    CHECK(Rect{.width = 2, .height = 0}.empty());
    CHECK(Rect{.width = -1, .height = 2}.empty());
    CHECK(Rect{.width = 2, .height = -1}.empty());
    CHECK_FALSE(Rect{.width = 2, .height = 3}.empty());
}

TEST_CASE("TUI rectangle edges saturate at the integer limits", "[tui][geometry]")
{
    constexpr auto maximum = std::numeric_limits<int>::max();
    constexpr auto minimum = std::numeric_limits<int>::min();

    auto const at_limits = Rect{.x = maximum - 1, .y = minimum + 1, .width = 1, .height = -1};
    CHECK(at_limits.right() == maximum);
    CHECK(at_limits.bottom() == minimum);

    auto const past_limits = Rect{.x = maximum, .y = minimum, .width = 1, .height = -1};
    CHECK(past_limits.right() == maximum);
    CHECK(past_limits.bottom() == minimum);

    auto const extreme_dimensions = Rect{.x = -1, .y = 1, .width = minimum, .height = maximum};
    CHECK(extreme_dimensions.right() == minimum);
    CHECK(extreme_dimensions.bottom() == maximum);
}
