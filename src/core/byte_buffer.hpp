/** @file byte_buffer.hpp
 * @brief Defines project-owned containers and views for arbitrary binary data.
 */
#pragma once

#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

namespace jb::core {

/// Owning buffer for arbitrary bytes such as database blobs and captured output.
using ByteBuffer = std::vector<std::byte>;
/// Read-only non-owning view of arbitrary bytes.
using ByteView   = std::span<std::byte const>;

/// Returns a byte-preserving view without allocating or validating encoding.
/// @return A view valid only while `value` remains valid.
[[nodiscard]] inline auto as_bytes(std::string_view value) noexcept -> ByteView
{
    return std::as_bytes(std::span{value.data(), value.size()});
}

/// Returns a byte-preserving text view without allocating or validating UTF-8.
/// @return A view valid only while `value` remains valid.
[[nodiscard]] inline auto as_string_view(ByteView value) noexcept -> std::string_view
{
    return {reinterpret_cast<char const*>(value.data()), value.size()};
}

} // namespace jb::core
