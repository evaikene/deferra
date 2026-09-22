#pragma once

#include "error.hpp"
#include "http_client.hpp"
#include "result.hpp"

#include <optional>
#include <span>
#include <string_view>

namespace jb::net::detail {

/// Borrowed header fields. An absent value denotes unresolved bytes; name/count/known-byte checks still apply.
struct HttpHeaderFields {
    std::string_view                name;
    std::optional<std::string_view> value;
};

/// Validates shared literal request fields and structural bounds, including HEAD body presence.
/// Unknown header bytes must be checked again by validate_http_request after preparation.
[[nodiscard]] auto validate_http_request_fields(std::string_view                  method,
                                                std::string_view                  url,
                                                std::span<HttpHeaderFields const> headers,
                                                bool has_body) -> jb::core::Result<void, jb::core::Error>;

/// Validates one HTTP(S) final-wire-form URL without exposing its value in errors.
[[nodiscard]] auto validate_http_url(std::string_view url) -> jb::core::Result<void, jb::core::Error>;

/// Validates the backend-independent request contract without exposing request data in errors.
[[nodiscard]] auto validate_http_request(HttpRequest const& request) -> jb::core::Result<void, jb::core::Error>;

} // namespace jb::net::detail
