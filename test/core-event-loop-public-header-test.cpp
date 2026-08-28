#include "event_loop.hpp"

#include <cstdint>
#include <type_traits>

using WatchFdMethod = jb::core::FdWatch (jb::core::EventLoop::*)(int,
                                                                 jb::core::FdEvents,
                                                                 jb::core::FdTriggerMode,
                                                                 jb::core::FdCallback);

static_assert(std::is_same_v<std::underlying_type_t<jb::core::FdTriggerMode>, std::uint8_t>);
static_assert(std::is_same_v<decltype(&jb::core::EventLoop::watch_fd), WatchFdMethod>);

auto main() -> int
{
    auto const edge  = jb::core::FdTriggerMode::Edge;
    auto const level = jb::core::FdTriggerMode::Level;
    return edge != level ? 0 : 1;
}
