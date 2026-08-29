#include "http_validation_priv.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

namespace jb::net::detail {

namespace {

constexpr std::size_t   kMaximumMethodBytes{32};
constexpr std::size_t   kMaximumUrlBytes{std::size_t{16} * 1024U};
constexpr std::size_t   kMaximumHeaderCount{128};
constexpr std::size_t   kMaximumHeaderBytes{std::size_t{64} * 1024U};
constexpr std::uint32_t kMaximumRedirects{20};

constexpr std::array<std::string_view, 8> kReservedHeaders{
    "Host",
    "Content-Length",
    "Transfer-Encoding",
    "Connection",
    "Proxy-Connection",
    "TE",
    "Trailer",
    "Upgrade",
};

auto invalid_request(std::string_view reason) -> jb::core::Result<void, jb::core::Error>
{
    return jb::core::Result<void, jb::core::Error>::failure({
        .category = jb::core::ErrorCategory::InvalidArgument,
        .code     = "net.http.invalid_request",
        .message  = "HTTP request is invalid",
        .detail   = std::string{reason},
    });
}

constexpr auto ascii_lower(unsigned char value) noexcept -> unsigned char
{
    if (value >= static_cast<unsigned char>('A') && value <= static_cast<unsigned char>('Z')) {
        return static_cast<unsigned char>(value + ('a' - 'A'));
    }
    return value;
}

auto ascii_equal(std::string_view lhs, std::string_view rhs) noexcept -> bool
{
    return lhs.size() == rhs.size() && std::ranges::equal(lhs, rhs, [](char left, char right) {
               return ascii_lower(static_cast<unsigned char>(left)) == ascii_lower(static_cast<unsigned char>(right));
           });
}

constexpr auto is_token_character(unsigned char value) noexcept -> bool
{
    if ((value >= static_cast<unsigned char>('a') && value <= static_cast<unsigned char>('z')) ||
        (value >= static_cast<unsigned char>('A') && value <= static_cast<unsigned char>('Z')) ||
        (value >= static_cast<unsigned char>('0') && value <= static_cast<unsigned char>('9'))) {
        return true;
    }

    switch (value) {
        case '!':
        case '#':
        case '$':
        case '%':
        case '&':
        case '\'':
        case '*':
        case '+':
        case '-':
        case '.':
        case '^':
        case '_':
        case '`':
        case '|':
        case '~':
            return true;
        default:
            return false;
    }
}

auto is_token(std::string_view value) noexcept -> bool
{
    return !value.empty() && std::ranges::all_of(value, [](char character) {
        return is_token_character(static_cast<unsigned char>(character));
    });
}

constexpr auto is_hex_digit(unsigned char value) noexcept -> bool
{
    return (value >= static_cast<unsigned char>('0') && value <= static_cast<unsigned char>('9')) ||
           (value >= static_cast<unsigned char>('a') && value <= static_cast<unsigned char>('f')) ||
           (value >= static_cast<unsigned char>('A') && value <= static_cast<unsigned char>('F'));
}

auto validate_url_characters(std::string_view url) -> std::string_view
{
    for (std::size_t index = 0; index < url.size(); ++index) {
        auto const character = static_cast<unsigned char>(url[index]);
        if (character < 0x21U || character > 0x7eU) {
            return "url.not_printable_ascii";
        }
        if (character == static_cast<unsigned char>('#')) {
            return "url.fragment_forbidden";
        }
        if (character == static_cast<unsigned char>('%')) {
            if (index + 2U >= url.size() || !is_hex_digit(static_cast<unsigned char>(url[index + 1U])) ||
                !is_hex_digit(static_cast<unsigned char>(url[index + 2U]))) {
                return "url.invalid_percent_escape";
            }
            index += 2U;
        }
    }
    return {};
}

auto valid_port_suffix(std::string_view suffix) -> bool
{
    return suffix.size() > 1U && suffix.front() == ':' && std::ranges::all_of(suffix.substr(1), [](char character) {
               auto const value = static_cast<unsigned char>(character);
               return value >= static_cast<unsigned char>('0') && value <= static_cast<unsigned char>('9');
           });
}

auto validate_authority(std::string_view authority) -> std::string_view
{
    if (authority.empty()) {
        return "url.missing_host";
    }
    if (authority.find('@') != std::string_view::npos) {
        return "url.userinfo_forbidden";
    }

    if (authority.front() == '[') {
        auto const closing = authority.find(']');
        if (closing == std::string_view::npos || closing == 1U) {
            return "url.missing_host";
        }
        auto const suffix = authority.substr(closing + 1U);
        if ((!suffix.empty() && !valid_port_suffix(suffix)) || authority.find('[', 1U) != std::string_view::npos ||
            authority.find(']', closing + 1U) != std::string_view::npos) {
            return "url.invalid_authority";
        }
        return {};
    }

    if (authority.find_first_of("[]") != std::string_view::npos) {
        return "url.invalid_authority";
    }

    auto const colon = authority.find(':');
    if (colon == std::string_view::npos) {
        return {};
    }
    if (colon == 0U) {
        return "url.missing_host";
    }
    if (authority.find(':', colon + 1U) != std::string_view::npos || !valid_port_suffix(authority.substr(colon))) {
        return "url.invalid_authority";
    }
    return {};
}

auto validate_url(std::string_view url) -> std::string_view
{
    if (url.empty()) {
        return "url.empty";
    }
    if (url.size() > kMaximumUrlBytes) {
        return "url.too_long";
    }
    if (auto const reason = validate_url_characters(url); !reason.empty()) {
        return reason;
    }

    auto const scheme_end = url.find(':');
    if (scheme_end == std::string_view::npos ||
        (!ascii_equal(url.substr(0, scheme_end), "http") && !ascii_equal(url.substr(0, scheme_end), "https"))) {
        return "url.unsupported_scheme";
    }
    auto const authority_begin = scheme_end + 3U;
    if (scheme_end + 2U >= url.size() || url[scheme_end + 1U] != '/' || url[scheme_end + 2U] != '/') {
        return "url.invalid_absolute_form";
    }
    auto const authority_end = url.find_first_of("/?", authority_begin);
    auto const authority     = url.substr(authority_begin, authority_end - authority_begin);
    return validate_authority(authority);
}

auto validate_method(std::string_view method) -> std::string_view
{
    if (method.empty()) {
        return "method.empty";
    }
    if (method.size() > kMaximumMethodBytes) {
        return "method.too_long";
    }
    if (!is_token(method)) {
        return "method.invalid_token";
    }
    return {};
}

auto validate_headers(std::vector<HttpHeader> const& headers) -> std::string_view
{
    if (headers.size() > kMaximumHeaderCount) {
        return "headers.too_many";
    }

    std::size_t total_bytes{0};
    for (std::size_t index = 0; index < headers.size(); ++index) {
        auto const& header = headers[index];
        if (!is_token(header.name)) {
            return "headers.invalid_name";
        }
        if (header.value.find_first_of("\r\n\0", 0U, 3U) != std::string::npos) {
            return "headers.invalid_value";
        }
        if (std::ranges::any_of(kReservedHeaders,
                                [&header](std::string_view reserved) { return ascii_equal(header.name, reserved); })) {
            return "headers.reserved_name";
        }
        for (std::size_t previous = 0; previous < index; ++previous) {
            if (ascii_equal(header.name, headers[previous].name)) {
                return "headers.duplicate_name";
            }
        }
        if (header.name.size() > kMaximumHeaderBytes - total_bytes) {
            return "headers.too_large";
        }
        total_bytes += header.name.size();
        if (header.value.size() > kMaximumHeaderBytes - total_bytes) {
            return "headers.too_large";
        }
        total_bytes += header.value.size();
    }
    return {};
}

auto representable_backend_size(std::size_t value) noexcept -> bool
{
    return static_cast<std::uintmax_t>(value) <= static_cast<std::uintmax_t>(std::numeric_limits<std::int64_t>::max());
}

} // anonymous namespace

auto validate_http_url(std::string_view url) -> jb::core::Result<void, jb::core::Error>
{
    if (auto const reason = validate_url(url); !reason.empty()) {
        return invalid_request(reason);
    }
    return jb::core::Result<void, jb::core::Error>::success();
}

auto validate_http_request(HttpRequest const& request) -> jb::core::Result<void, jb::core::Error>
{
    if (auto const reason = validate_method(request.method); !reason.empty()) {
        return invalid_request(reason);
    }
    auto url = validate_http_url(request.url);
    if (!url) {
        return url;
    }
    if (auto const reason = validate_headers(request.headers); !reason.empty()) {
        return invalid_request(reason);
    }
    if (request.method == "HEAD" && request.body.has_value()) {
        return invalid_request("body.head_forbidden");
    }
    if (request.timeout < std::chrono::milliseconds{1} || request.timeout > std::chrono::days{30}) {
        return invalid_request("timeout.out_of_range");
    }
    if (request.max_redirects > kMaximumRedirects || (request.follow_redirects && request.max_redirects == 0U)) {
        return invalid_request("redirects.out_of_range");
    }
    if ((request.body && !representable_backend_size(request.body->size())) ||
        !representable_backend_size(request.response_body_limit) ||
        !representable_backend_size(request.response_header_limit)) {
        return invalid_request("size.not_representable");
    }
    return jb::core::Result<void, jb::core::Error>::success();
}

} // namespace jb::net::detail
