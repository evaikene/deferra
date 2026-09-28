#include "http/url_validation.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string_view>

using jb::net::http::UrlScheme;
using jb::net::http::UrlValidationIssue;
using jb::net::http::validate_url;

TEST_CASE("HTTP URL validation accepts complete HTTP and HTTPS authorities", "[net][http][url]")
{
    auto http = validate_url("http://example.test:8080/path");
    REQUIRE(http);
    CHECK(*http == UrlScheme::Http);

    auto https = validate_url("https://[::1]:443/path");
    REQUIRE(https);
    CHECK(*https == UrlScheme::Https);
}

TEST_CASE("HTTP URL validation rejects malformed and credential-bearing URLs", "[net][http][url]")
{
    for (auto const* url : {"", "relative-proxy", "http://proxy.test:99999", "http://[invalid]:8080"}) {
        CAPTURE(url);
        auto validated = validate_url(url);
        REQUIRE_FALSE(validated);
        CHECK(validated.error() == UrlValidationIssue::InvalidUrl);
    }

    auto unsupported = validate_url("ftp://example.test");
    REQUIRE_FALSE(unsupported);
    CHECK(unsupported.error() == UrlValidationIssue::UnsupportedScheme);

    auto credentials = validate_url("http://user:password@proxy.test");
    REQUIRE_FALSE(credentials);
    CHECK(credentials.error() == UrlValidationIssue::UserinfoForbidden);

    auto const embedded_nul = std::string_view{"http://proxy.test\0:80", 21U};
    auto       nul_result   = validate_url(embedded_nul);
    REQUIRE_FALSE(nul_result);
    CHECK(nul_result.error() == UrlValidationIssue::InvalidUrl);
}
