/** @file value.hpp
 * @brief Defines backend-independent database values and byte-preserving construction helpers.
 */
#pragma once

#include "byte_buffer.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>

namespace jb::db {

/** Represents SQL null independently of empty text and blob values.
 *
 * Store this alternative in a Value when a parameter or field must represent SQL NULL.
 */
struct Null {
    /// Compares two SQL null markers.
    /// @param other Null marker to compare.
    /// @return Always true because Null has no additional state.
    auto operator==(Null const& other) const -> bool = default;
};

/** Owning value accepted and returned by database drivers.
 *
 * Repositories explicitly select the required alternative; the database layer performs no implicit conversions.
 */
using Value = std::variant<Null, std::int64_t, double, std::string, jb::core::ByteBuffer>;

/// Creates an owning text database value without changing its bytes.
/// @param value Text bytes to copy.
/// @return A Value containing an owned std::string.
[[nodiscard]] auto make_text(std::string_view value) -> Value;

/// Creates an owning blob database value without interpreting its bytes.
/// @param value Binary bytes to copy.
/// @return A Value containing an owned jb::core::ByteBuffer.
[[nodiscard]] auto make_blob(jb::core::ByteView value) -> Value;

} // namespace jb::db
