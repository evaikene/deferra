/// @file url_validation.hpp
/// @brief Validates absolute HTTP and HTTPS URLs without opening a connection.
#pragma once

#include "result.hpp"

#include <cstdint>
#include <string_view>

namespace jb::net::http {

/// Scheme of a validated HTTP or HTTPS URL.
enum class UrlScheme : std::uint8_t {
    Http,
    Https
};

/// Value-free reason why a URL could not be validated.
enum class UrlValidationIssue : std::uint8_t {
    InvalidUrl,
    InvalidAbsoluteUrl,
    UnsupportedScheme,
    UserinfoForbidden,
    Unavailable
};

/// Validates one absolute HTTP or HTTPS URL with a nonempty host, valid authority and port, and no user information.
/// @param url Complete URL text. The function does not retain it or open a connection.
/// @return The URL scheme, or a value-free issue. Unavailable means validation could not be completed.
/// This syntax check does not test whether a configured proxy transport is available at runtime.
[[nodiscard]] auto validate_url(std::string_view url) -> jb::core::Result<UrlScheme, UrlValidationIssue>;

} // namespace jb::net::http
