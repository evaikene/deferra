#pragma once

#include "error.hpp"
#include "result.hpp"

#include <string>
#include <string_view>

namespace jb::net::http::detail {

struct RedirectTarget {
    std::string url;
    bool        cross_origin{false};
    bool        uses_tls{false};
};

/// Resolves and validates one redirect target while applying origin and downgrade policy.
[[nodiscard]] auto resolve_redirect_target(std::string_view current_url, std::string_view location)
    -> jb::core::Result<RedirectTarget, jb::core::Error>;

} // namespace jb::net::http::detail
