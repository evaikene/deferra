#include "retry_mode.hpp"

#include <cstdint>
#include <type_traits>

static_assert(std::is_same_v<std::underlying_type_t<jb::jobu::RetryMode>, std::uint8_t>);
static_assert(static_cast<std::uint8_t>(jb::jobu::RetryMode::Blocking) == 0);
static_assert(static_cast<std::uint8_t>(jb::jobu::RetryMode::Reschedule) == 1);

auto main() -> int
{
    return 0;
}
