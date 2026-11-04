#include "scoped_event_loop.hpp"

int main()
{
    auto scope = jb::core::ScopedEventLoop::create();
    return scope ? 0 : 1;
}
