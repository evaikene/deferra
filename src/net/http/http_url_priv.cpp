#include "http_url_priv.hpp"

#include "http_validation_priv.hpp"

#include <curl/urlapi.h>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <memory>
#include <string>
#include <system_error>

namespace jb::net::http::detail {

namespace {

struct CurlUrlDeleter {
    void operator()(CURLU* url) const noexcept { curl_url_cleanup(url); }
};

struct CurlStringDeleter {
    void operator()(char* value) const noexcept { curl_free(value); }
};

using CurlUrl    = std::unique_ptr<CURLU, CurlUrlDeleter>;
using CurlString = std::unique_ptr<char, CurlStringDeleter>;

struct HttpOrigin {
    std::string   scheme;
    std::string   host;
    std::uint16_t port{0};

    auto operator==(HttpOrigin const&) const -> bool = default;
};

auto redirect_failed(std::string_view reason) -> jb::core::Error
{
    return {
        .category = jb::core::ErrorCategory::Io,
        .code     = "net.http.redirect_failed",
        .message  = "The HTTP redirect could not be followed safely",
        .detail   = std::string{reason},
    };
}

auto get_url_part(CURLU* url, CURLUPart part, unsigned int flags = 0U) -> CurlString
{
    char*      raw_value{nullptr};
    auto const result = curl_url_get(url, part, &raw_value, flags);
    auto       value  = CurlString{raw_value};
    if (result != CURLUE_OK) {
        value.reset();
    }
    return value;
}

auto parse_origin(CURLU* url) -> jb::core::Result<HttpOrigin, jb::core::Error>
{
    using Result = jb::core::Result<HttpOrigin, jb::core::Error>;

    auto scheme = get_url_part(url, CURLUPART_SCHEME);
    auto host   = get_url_part(url, CURLUPART_HOST);
    auto port   = get_url_part(url, CURLUPART_PORT, CURLU_DEFAULT_PORT);
    if (!scheme || !host || !port) {
        return Result::failure(redirect_failed("redirect.invalid_target"));
    }

    auto const port_text   = std::string_view{port.get()};
    auto       port_number = std::uint32_t{0};
    auto       parsed      = std::from_chars(port_text.begin(), port_text.end(), port_number);
    if (parsed.ec != std::errc{} || parsed.ptr != port_text.end() || port_number > 65535U) {
        return Result::failure(redirect_failed("redirect.invalid_target"));
    }

    auto origin = HttpOrigin{
        .scheme = scheme.get(),
        .host   = host.get(),
        .port   = static_cast<std::uint16_t>(port_number),
    };
    std::ranges::transform(origin.scheme, origin.scheme.begin(), [](unsigned char value) -> char {
        return static_cast<char>(value >= static_cast<unsigned char>('A') && value <= static_cast<unsigned char>('Z')
                                     ? value + ('a' - 'A')
                                     : value);
    });
    std::ranges::transform(origin.host, origin.host.begin(), [](unsigned char value) -> char {
        return static_cast<char>(value >= static_cast<unsigned char>('A') && value <= static_cast<unsigned char>('Z')
                                     ? value + ('a' - 'A')
                                     : value);
    });
    return Result::success(std::move(origin));
}

} // anonymous namespace

auto resolve_redirect_target(std::string_view current_url, std::string_view location)
    -> jb::core::Result<RedirectTarget, jb::core::Error>
{
    using Result = jb::core::Result<RedirectTarget, jb::core::Error>;

    if (location.empty()) {
        return Result::failure(redirect_failed("redirect.invalid_target"));
    }

    auto url = CurlUrl{curl_url()};
    if (!url) {
        return Result::failure(redirect_failed("redirect.url_parser_unavailable"));
    }

    auto current = std::string{current_url};
    auto next    = std::string{location};
    if (curl_url_set(url.get(), CURLUPART_URL, current.c_str(), 0U) != CURLUE_OK) {
        return Result::failure(redirect_failed("redirect.invalid_current_url"));
    }
    auto current_origin = parse_origin(url.get());
    if (!current_origin) {
        return Result::failure(std::move(current_origin).error());
    }

    if (curl_url_set(url.get(), CURLUPART_URL, next.c_str(), 0U) != CURLUE_OK) {
        return Result::failure(redirect_failed("redirect.invalid_target"));
    }
    auto resolved = get_url_part(url.get(), CURLUPART_URL);
    if (!resolved) {
        return Result::failure(redirect_failed("redirect.invalid_target"));
    }
    auto validation = jb::net::detail::validate_http_url(resolved.get());
    if (!validation) {
        return Result::failure(redirect_failed("redirect.invalid_target"));
    }

    auto target_origin = parse_origin(url.get());
    if (!target_origin) {
        return Result::failure(std::move(target_origin).error());
    }
    if (current_origin->scheme == "https" && target_origin->scheme == "http") {
        return Result::failure(redirect_failed("redirect.https_downgrade"));
    }

    return Result::success({
        .url          = resolved.get(),
        .cross_origin = *current_origin != *target_origin,
    });
}

} // namespace jb::net::http::detail
