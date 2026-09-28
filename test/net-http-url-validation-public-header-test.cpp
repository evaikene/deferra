#include "http/url_validation.hpp"

#include <string_view>
#include <type_traits>

using UrlResult = jb::core::Result<jb::net::http::UrlScheme, jb::net::http::UrlValidationIssue>;

static_assert(std::is_same_v<decltype(&jb::net::http::validate_url), UrlResult (*)(std::string_view)>);

auto main() -> int
{
    auto validated = jb::net::http::validate_url("http://example.test:8080");
    return validated && *validated == jb::net::http::UrlScheme::Http ? 0 : 1;
}
