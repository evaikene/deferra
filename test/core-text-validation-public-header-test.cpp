#include "text_validation.hpp"

#include <string_view>
#include <type_traits>

using TextCheck = bool (*)(std::string_view) noexcept;

static_assert(std::is_same_v<decltype(&jb::core::is_valid_utf8), TextCheck>);
static_assert(std::is_same_v<decltype(&jb::core::has_ascii_control), TextCheck>);

auto main() -> int
{
    return jb::core::is_valid_utf8("text") && !jb::core::has_ascii_control("text") ? 0 : 1;
}
