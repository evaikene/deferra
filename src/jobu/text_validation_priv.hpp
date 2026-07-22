#pragma once

#include <string_view>

namespace jb::jobu::detail {

[[nodiscard]] auto is_valid_utf8(std::string_view text) noexcept -> bool;
[[nodiscard]] auto has_ascii_control(std::string_view text) noexcept -> bool;

} // namespace jb::jobu::detail
