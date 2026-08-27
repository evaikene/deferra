#include "scheduler.hpp"

#include <type_traits>

static_assert(std::is_same_v<std::underlying_type_t<jb::jobu::CancelDisposition>, std::uint8_t>);
static_assert(std::is_copy_constructible_v<jb::jobu::CancelRunResult>);
static_assert(std::is_move_constructible_v<jb::jobu::CancelRunResult>);

auto main() -> int
{
    return 0;
}
