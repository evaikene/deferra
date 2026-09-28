#include "url_validation.hpp"

#include <curl/curl.h>
#include <curl/urlapi.h>

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

namespace jb::net::http {

namespace {

struct CurlUrlDeleter {
    void operator()(CURLU* url) const noexcept { curl_url_cleanup(url); }
};

struct CurlStringDeleter {
    void operator()(char* value) const noexcept { curl_free(value); }
};

using CurlUrl    = std::unique_ptr<CURLU, CurlUrlDeleter>;
using CurlString = std::unique_ptr<char, CurlStringDeleter>;

constexpr auto ascii_lower(unsigned char value) noexcept -> unsigned char
{
    if (value >= static_cast<unsigned char>('A') && value <= static_cast<unsigned char>('Z')) {
        return static_cast<unsigned char>(value + ('a' - 'A'));
    }
    return value;
}

auto ascii_equal(std::string_view lhs, std::string_view rhs) noexcept -> bool
{
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (std::size_t index = 0; index < lhs.size(); ++index) {
        if (ascii_lower(static_cast<unsigned char>(lhs[index])) !=
            ascii_lower(static_cast<unsigned char>(rhs[index]))) {
            return false;
        }
    }
    return true;
}

auto get_url_part(CURLU* url, CURLUPart part, CurlString& value) -> bool
{
    char*      raw_value{nullptr};
    auto const result = curl_url_get(url, part, &raw_value, 0);
    value.reset(raw_value);
    return result == CURLUE_OK;
}

auto has_userinfo_part(CURLU* url, CURLUPart part) -> jb::core::Result<bool, UrlValidationIssue>
{
    using UserinfoResult = jb::core::Result<bool, UrlValidationIssue>;

    char*      raw_value{nullptr};
    auto const result = curl_url_get(url, part, &raw_value, 0);
    auto       value  = CurlString{raw_value};
    if (result == CURLUE_OK) {
        return UserinfoResult::success(true);
    }
    auto const missing_part = part == CURLUPART_USER ? CURLUE_NO_USER : CURLUE_NO_PASSWORD;
    if (result == missing_part) {
        return UserinfoResult::success(false);
    }
    return UserinfoResult::failure(UrlValidationIssue::InvalidUrl);
}

} // anonymous namespace

auto validate_url(std::string_view url) -> jb::core::Result<UrlScheme, UrlValidationIssue>
{
    using UrlResult = jb::core::Result<UrlScheme, UrlValidationIssue>;

    if (url.empty() || url.find('\0') != std::string_view::npos) {
        return UrlResult::failure(UrlValidationIssue::InvalidUrl);
    }

    auto parsed_url = CurlUrl{curl_url()};
    if (!parsed_url) {
        return UrlResult::failure(UrlValidationIssue::Unavailable);
    }
    auto const owned_url = std::string{url};
    if (curl_url_set(parsed_url.get(), CURLUPART_URL, owned_url.c_str(), 0) != CURLUE_OK) {
        return UrlResult::failure(UrlValidationIssue::InvalidUrl);
    }

    auto scheme = CurlString{};
    auto host   = CurlString{};
    if (!get_url_part(parsed_url.get(), CURLUPART_SCHEME, scheme) ||
        !get_url_part(parsed_url.get(), CURLUPART_HOST, host) || !host || std::string_view{host.get()}.empty()) {
        return UrlResult::failure(UrlValidationIssue::InvalidAbsoluteUrl);
    }

    auto const is_http  = scheme && ascii_equal(scheme.get(), "http");
    auto const is_https = scheme && ascii_equal(scheme.get(), "https");
    if (!is_http && !is_https) {
        return UrlResult::failure(UrlValidationIssue::UnsupportedScheme);
    }

    auto user_present = has_userinfo_part(parsed_url.get(), CURLUPART_USER);
    if (!user_present) {
        return UrlResult::failure(user_present.error());
    }
    auto password_present = has_userinfo_part(parsed_url.get(), CURLUPART_PASSWORD);
    if (!password_present) {
        return UrlResult::failure(password_present.error());
    }
    if (*user_present || *password_present) {
        return UrlResult::failure(UrlValidationIssue::UserinfoForbidden);
    }

    return UrlResult::success(is_https ? UrlScheme::Https : UrlScheme::Http);
}

} // namespace jb::net::http
