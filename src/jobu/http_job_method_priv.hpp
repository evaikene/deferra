#pragma once

#include "error.hpp"
#include "result.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>

namespace jb::jobu::detail {

/// Methods admitted for newly supplied HTTP job payloads; the generic network API remains unrestricted.
enum class HttpJobMethod : std::uint8_t {
    Get,
    Post,
    Put,
    Patch,
    Delete,
    Head,
};

inline constexpr std::string_view supported_http_job_methods = "GET, POST, PUT, PATCH, DELETE, HEAD";

/// Parses only the six supported methods, using ASCII case folding without trimming or aliases.
/// Invalid input produces a bounded, escaped jobu.job.invalid_payload message.
[[nodiscard]] auto parse_submitted_http_method(std::string_view value)
    -> jb::core::Result<HttpJobMethod, jb::core::Error>;

/// Returns the canonical spelling of a valid method, borrowing static storage.
[[nodiscard]] auto http_method_text(HttpJobMethod method) noexcept -> std::string_view;

/// Exact previously valid spelling retained only by the stored-payload compatibility decoder.
struct LegacyHttpMethod {
    std::string wire_value;
};

using DecodedHttpMethod = std::variant<HttpJobMethod, LegacyHttpMethod>;

/// Classifies an already structurally validated stored method without case folding or admission checks.
/// New input must use parse_submitted_http_method instead; this path preserves legacy execution semantics.
[[nodiscard]] auto decode_stored_http_method(std::string value) -> DecodedHttpMethod;

/// Returns canonical enum text or exact legacy text; a legacy view borrows from method.
[[nodiscard]] auto http_method_wire_text(DecodedHttpMethod const& method) noexcept -> std::string_view;

} // namespace jb::jobu::detail
