#pragma once

#include "error.hpp"
#include "http_client.hpp"
#include "result.hpp"

#include <string_view>

namespace jb::net::detail {

/// Validates one HTTP(S) final-wire-form URL without exposing its value in errors.
[[nodiscard]] auto validate_http_url(std::string_view url) -> jb::core::Result<void, jb::core::Error>;

/// Validates the backend-independent request contract without exposing request data in errors.
[[nodiscard]] auto validate_http_request(HttpRequest const& request) -> jb::core::Result<void, jb::core::Error>;

} // namespace jb::net::detail
