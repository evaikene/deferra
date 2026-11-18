#include <jb/tui/geometry.hpp>

int main()
{
    auto const rect = jb::tui::Rect{.x = -2, .y = 3, .width = 5, .height = 4};
    return rect.right() == 3 && rect.bottom() == 7 ? 0 : 1;
}
