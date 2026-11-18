#include "geometry.hpp"

#include <limits>

namespace jb::tui {
namespace {

auto exclusive_bound(int origin, int extent) -> int
{
    constexpr auto maximum = std::numeric_limits<int>::max();
    constexpr auto minimum = std::numeric_limits<int>::min();

    // Check the available range before adding so the sum cannot overflow.
    if (extent > 0 && origin > maximum - extent) {
        return maximum;
    }
    if (extent < 0 && origin < minimum - extent) {
        return minimum;
    }

    return origin + extent;
}

} // anonymous namespace

auto Rect::right() const -> int
{
    return exclusive_bound(x, width);
}

auto Rect::bottom() const -> int
{
    return exclusive_bound(y, height);
}

} // namespace jb::tui
