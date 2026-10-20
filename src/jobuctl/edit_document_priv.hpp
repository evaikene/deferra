#pragma once

#include "edit_resource_priv.hpp"
#include "error.hpp"
#include "json.hpp"
#include "result.hpp"

#include <cstddef>
#include <string>
#include <string_view>

namespace jb::jobuctl::detail {

/// Maximum complete human draft size, including instructions; RPC and persisted payload limits are unchanged.
inline constexpr std::size_t max_edit_draft_bytes = std::size_t{16} * 1024 * 1024;

/// Fixed, input-safe size error, also used by bounded file creation and reads.
[[nodiscard]] auto edit_draft_too_large(EditResource resource) -> jb::core::Error;

/// Pretty-prints JSON with whitespace changes only, preserving strings and scalar representations.
/// Enforces the normal JSON depth limit and a 16 MiB output limit. Errors never contain supplied values.
[[nodiscard]] auto format_edit_json(EditResource resource, jb::core::JsonValue const& value)
    -> jb::core::Result<std::string, jb::core::Error>;

/// Prepends trusted, complete JSONC instruction comments supplied by domain code to pretty JSON.
/// Bounds the complete draft, including instructions, to 16 MiB. Ordinary request files remain strict JSON.
[[nodiscard]] auto format_edit_draft(EditResource               resource,
                                     jb::core::JsonValue const& value,
                                     std::string_view instructions) -> jb::core::Result<std::string, jb::core::Error>;

/// Parses one JSONC object using the existing JSON codec, accepting line/block comments outside strings.
/// Bounds the original bytes to 16 MiB before parsing; normal depth, UTF-8, member and numeric rules apply.
/// Rejects trailing commas and leaves caller/saved bytes unchanged. Does not enable comments in request files.
[[nodiscard]] auto parse_edit_draft(EditResource resource, std::string_view draft)
    -> jb::core::Result<jb::core::JsonValue, jb::core::Error>;

/// Serializes validated minimal params as compact strict JSON under the 1 MiB request-file/framing limit.
/// Preserves the normal depth limit, performs no I/O, and does not replace domain decoding or RPC admission.
[[nodiscard]] auto serialize_edit_request(EditResource resource, jb::core::JsonValue const& params)
    -> jb::core::Result<std::string, jb::core::Error>;

} // namespace jb::jobuctl::detail
