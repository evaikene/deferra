#include "http_client.hpp"

#include "http_validation_priv.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace jb::core;
using namespace jb::net;
using namespace jb::net::detail;
using namespace std::chrono_literals;

namespace {

constexpr std::size_t kMaximumUrlBytes{std::size_t{16} * 1024U};
constexpr std::size_t kMaximumHeaderBytes{std::size_t{64} * 1024U};

auto request_for(std::string url = "https://example.test/path?key=value") -> HttpRequest
{
    return {.url = std::move(url)};
}

void check_valid(HttpRequest const& request)
{
    auto result = validate_http_request(request);
    if (!result) {
        INFO(result.error().detail);
    }
    REQUIRE(result);
}

void check_invalid(HttpRequest const& request, std::string_view reason)
{
    auto result = validate_http_request(request);
    REQUIRE_FALSE(result);
    CHECK(result.error().category == ErrorCategory::InvalidArgument);
    CHECK(result.error().code == "net.http.invalid_request");
    CHECK(result.error().message == "HTTP request is invalid");
    CHECK(result.error().detail == reason);
    CHECK(result.error().code.find("example.test") == std::string::npos);
    CHECK(result.error().message.find("example.test") == std::string::npos);
    CHECK(result.error().detail.find("example.test") == std::string::npos);
}

} // anonymous namespace

TEST_CASE("HTTP request defaults implement the generic contract", "[net][http][contract]")
{
    auto request = HttpRequest{};
    CHECK(request.method == "GET");
    CHECK(request.url.empty());
    CHECK(request.headers.empty());
    CHECK_FALSE(request.body);
    CHECK(request.timeout == 120s);
    CHECK(request.verify_tls);
    CHECK_FALSE(request.follow_redirects);
    CHECK(request.max_redirects == 5U);
    CHECK(request.response_body_limit == std::size_t{1024} * 1024U);
    CHECK(request.response_header_limit == std::size_t{64} * 1024U);

    auto response = HttpResponse{};
    CHECK(response.status_code == 0U);
    CHECK(response.redirect_count == 0U);
    CHECK(response.elapsed == Duration{});
    CHECK_FALSE(response.tls_verified);

    auto error = HttpError{};
    CHECK(error.kind == HttpErrorKind::Internal);
    CHECK(error.redirect_count == 0U);
    CHECK_FALSE(error.status_code);
}

TEST_CASE("HTTP validation accepts method and URL contract boundaries", "[net][http][validation]")
{
    auto request   = request_for("HTTPS://[2001:db8::1]:8443/a%20b?q=%2F");
    request.method = "!#$%&'*+-.^_`|~0123456789ABCDEFG";
    REQUIRE(request.method.size() == 32U);
    check_valid(request);

    auto maximum_url = std::string{"http://a/"};
    maximum_url.append(kMaximumUrlBytes - maximum_url.size(), 'x');
    check_valid(request_for(std::move(maximum_url)));
    check_valid(request_for("http://example.test:8080?empty"));
}

TEST_CASE("HTTP validation rejects invalid methods", "[net][http][validation]")
{
    auto empty   = request_for();
    empty.method = "";
    check_invalid(empty, "method.empty");

    auto long_method   = request_for();
    long_method.method = std::string(33U, 'A');
    check_invalid(long_method, "method.too_long");

    for (auto method : {std::string{"HAS SPACE"}, std::string{"GET/"}, std::string{"M\x7f"}}) {
        auto invalid   = request_for();
        invalid.method = std::move(method);
        check_invalid(invalid, "method.invalid_token");
    }
}

TEST_CASE("HTTP validation rejects invalid URL forms", "[net][http][validation]")
{
    auto too_long = std::string{"http://a/"};
    too_long.append((kMaximumUrlBytes + 1U) - too_long.size(), 'x');

    auto nul_url = std::string{"http://example.test/a"};
    nul_url.push_back('\0');

    auto non_ascii = std::string{"http://example.test/"};
    non_ascii.push_back(static_cast<char>(0x80));

    auto const cases = std::vector<std::pair<std::string, std::string_view>>{
        {"",                              "url.empty"                 },
        {std::move(too_long),             "url.too_long"              },
        {"http://example.test/has space", "url.not_printable_ascii"   },
        {std::move(nul_url),              "url.not_printable_ascii"   },
        {std::move(non_ascii),            "url.not_printable_ascii"   },
        {"http://example.test/%",         "url.invalid_percent_escape"},
        {"http://example.test/%0G",       "url.invalid_percent_escape"},
        {"http://example.test/#part",     "url.fragment_forbidden"    },
        {"ftp://example.test/",           "url.unsupported_scheme"    },
        {"example.test/path",             "url.unsupported_scheme"    },
        {"http:/example.test",            "url.invalid_absolute_form" },
        {"http:///path",                  "url.missing_host"          },
        {"http://user@example.test/",     "url.userinfo_forbidden"    },
        {"http://:80/",                   "url.missing_host"          },
        {"http://example.test:/",         "url.invalid_authority"     },
        {"http://example.test:abc/",      "url.invalid_authority"     },
        {"http://2001:db8::1/",           "url.invalid_authority"     },
        {"http://[]/",                    "url.missing_host"          },
        {"http://[2001:db8::1/path",      "url.missing_host"          },
        {"http://[2001:db8::1]extra/",    "url.invalid_authority"     },
    };

    for (auto const& [url, reason] : cases) {
        CAPTURE(url, reason);
        check_invalid(request_for(url), reason);
    }
}

TEST_CASE("HTTP validation enforces header syntax ownership and bounds", "[net][http][validation]")
{
    auto valid    = request_for();
    valid.headers = {
        {.name = "X-Extension", .value = "value\twith\tobs-fold-free-tabs"},
        {.name = "Proxy-Authorization", .value = "literal", .sensitive = true},
    };
    check_valid(valid);

    for (auto const* const reserved : std::array{"Host",
                                                 "content-length",
                                                 "TRANSFER-ENCODING",
                                                 "Connection",
                                                 "Proxy-Connection",
                                                 "te",
                                                 "Trailer",
                                                 "Upgrade"}) {
        auto request    = request_for();
        request.headers = {
            {.name = reserved, .value = "value"}
        };
        check_invalid(request, "headers.reserved_name");
    }

    auto empty_name    = request_for();
    empty_name.headers = {
        {.name = "", .value = "value"}
    };
    check_invalid(empty_name, "headers.invalid_name");

    auto bad_name    = request_for();
    bad_name.headers = {
        {.name = "Bad Name", .value = "value"}
    };
    check_invalid(bad_name, "headers.invalid_name");

    for (auto value : {
             std::string{"line\rbreak"},
             std::string{"line\nbreak"},
             std::string{"nul\0value", 9U}
    }) {
        auto request    = request_for();
        request.headers = {
            {.name = "X-Value", .value = std::move(value)}
        };
        check_invalid(request, "headers.invalid_value");
    }

    auto duplicate    = request_for();
    duplicate.headers = {
        {.name = "X-Name", .value = "one"},
        {.name = "x-name", .value = "two"}
    };
    check_invalid(duplicate, "headers.duplicate_name");

    auto maximum_count = request_for();
    for (std::size_t index = 0; index < 128U; ++index) {
        maximum_count.headers.push_back({.name = "X-" + std::to_string(index), .value = ""});
    }
    check_valid(maximum_count);

    auto too_many = maximum_count;
    too_many.headers.push_back({.name = "X-128", .value = ""});
    check_invalid(too_many, "headers.too_many");

    auto exact_bytes    = request_for();
    exact_bytes.headers = {
        {.name = "X", .value = std::string(kMaximumHeaderBytes - 1U, 'a')}
    };
    check_valid(exact_bytes);

    auto too_large    = request_for();
    too_large.headers = {
        {.name = "X", .value = std::string(kMaximumHeaderBytes, 'a')}
    };
    check_invalid(too_large, "headers.too_large");
}

TEST_CASE("HTTP validation errors do not expose request data", "[net][http][validation]")
{
    auto request    = request_for("https://example.test/path?token=url-secret-sentinel");
    request.headers = {
        {.name = "X-Secret", .value = "header-secret-sentinel\r"}
    };

    auto result = validate_http_request(request);
    REQUIRE_FALSE(result);
    for (auto const& field : {result.error().code, result.error().message, result.error().detail}) {
        CHECK(field.find("url-secret-sentinel") == std::string::npos);
        CHECK(field.find("header-secret-sentinel") == std::string::npos);
    }
}

TEST_CASE("HTTP validation enforces body timeout redirect and backend-size boundaries", "[net][http][validation]")
{
    auto absent_body = request_for();
    check_valid(absent_body);

    auto empty_body = request_for();
    empty_body.body = ByteBuffer{};
    check_valid(empty_body);

    auto head_body   = request_for();
    head_body.method = "HEAD";
    head_body.body   = ByteBuffer{};
    check_invalid(head_body, "body.head_forbidden");

    auto lower_head   = request_for();
    lower_head.method = "head";
    lower_head.body   = ByteBuffer{};
    check_valid(lower_head);

    for (auto timeout : std::array<Duration, 2>{1ms, std::chrono::days{30}}) {
        auto request    = request_for();
        request.timeout = timeout;
        check_valid(request);
    }
    for (auto timeout : std::array<Duration, 2>{1ms - 1ns, std::chrono::days{30} + 1ns}) {
        auto request    = request_for();
        request.timeout = timeout;
        check_invalid(request, "timeout.out_of_range");
    }

    auto redirects_disabled          = request_for();
    redirects_disabled.max_redirects = 0;
    check_valid(redirects_disabled);

    auto redirects_enabled             = request_for();
    redirects_enabled.follow_redirects = true;
    redirects_enabled.max_redirects    = 20;
    check_valid(redirects_enabled);

    auto no_redirect_capacity             = request_for();
    no_redirect_capacity.follow_redirects = true;
    no_redirect_capacity.max_redirects    = 0;
    check_invalid(no_redirect_capacity, "redirects.out_of_range");

    auto excessive_redirects          = request_for();
    excessive_redirects.max_redirects = 21;
    check_invalid(excessive_redirects, "redirects.out_of_range");

    if constexpr (std::numeric_limits<std::size_t>::digits > std::numeric_limits<std::int64_t>::digits) {
        auto excessive_capture                = request_for();
        excessive_capture.response_body_limit = std::numeric_limits<std::size_t>::max();
        check_invalid(excessive_capture, "size.not_representable");
    }
}
