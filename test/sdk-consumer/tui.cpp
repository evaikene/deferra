#include <jb/tui/geometry.hpp>

int main()
{
    return jb::tui::priv::library_linked() ? 0 : 1;
}
