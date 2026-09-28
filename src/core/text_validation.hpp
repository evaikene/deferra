/// @file text_validation.hpp
/// @brief Common validation of UTF-8 text and ASCII control bytes.
#pragma once

#include <string_view>

namespace jb::core {

/// Reports whether the complete byte sequence is well-formed UTF-8.
/// Empty text and embedded NUL bytes are valid UTF-8; callers apply any additional text policy separately.
[[nodiscard]] auto is_valid_utf8(std::string_view text) noexcept -> bool;

/// Reports whether any byte is an ASCII control character (U+0000–U+001F or U+007F).
/// This byte-level check does not require valid UTF-8 input.
[[nodiscard]] auto has_ascii_control(std::string_view text) noexcept -> bool;

} // namespace jb::core
