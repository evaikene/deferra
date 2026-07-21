#pragma once

#include <string_view>

namespace jb::jobu::detail {

[[nodiscard]] auto is_valid_queue_name(std::string_view name) noexcept -> bool;
[[nodiscard]] auto is_valid_idempotency_key(std::string_view key) noexcept -> bool;

} // namespace jb::jobu::detail
