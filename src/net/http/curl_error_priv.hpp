#pragma once

#include "http_client.hpp"

#include <curl/curl.h>

namespace jb::net::http::detail {

/// Maps one non-OK libcurl transfer result to a stable request-local project error.
[[nodiscard]] auto map_curl_error(CURLcode result) -> HttpError;

} // namespace jb::net::http::detail
