#pragma once

#include "error.hpp"
#include "http_client.hpp"
#include "result.hpp"

namespace jb::net::detail {

/// Validates the backend-independent request contract without exposing request data in errors.
[[nodiscard]] auto validate_http_request(HttpRequest const& request) -> jb::core::Result<void, jb::core::Error>;

} // namespace jb::net::detail
