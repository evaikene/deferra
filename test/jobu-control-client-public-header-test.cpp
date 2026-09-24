#include "control_client.hpp"

#include <type_traits>

static_assert(std::is_base_of_v<jb::core::Object, jb::jobu::ControlClient>);
static_assert(!std::is_copy_constructible_v<jb::jobu::ControlClient>);

auto main() -> int
{
    return 0;
}
