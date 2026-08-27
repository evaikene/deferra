#include "system_http_client.hpp"

#include "curl_runtime_priv.hpp"

#include <curl/curl.h>
#include <curl/urlapi.h>

#include <fstream>
#include <ios>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace jb::net::http {

namespace {

using ClientResult = jb::core::Result<std::unique_ptr<SystemHttpClient>, jb::core::Error>;
using VoidResult   = jb::core::Result<void, jb::core::Error>;

constexpr std::size_t kMaximumParsedResponseHeaderBytes{std::size_t{64} * 1024U * 1024U};

auto invalid_options(std::string_view reason) -> jb::core::Error
{
    return {
        .category = jb::core::ErrorCategory::InvalidArgument,
        .code     = "net.http.invalid_options",
        .message  = "The system HTTP client options are invalid",
        .detail   = std::string{reason},
    };
}

auto event_loop_unavailable() -> jb::core::Error
{
    return {
        .category = jb::core::ErrorCategory::Unavailable,
        .code     = "net.http.event_loop_unavailable",
        .message  = "The system HTTP client requires a valid current EventLoop",
    };
}

auto runtime_unavailable(std::string_view reason) -> jb::core::Error
{
    return {
        .category = jb::core::ErrorCategory::Unavailable,
        .code     = "net.http.runtime_unavailable",
        .message  = "The system HTTP runtime does not satisfy the requested configuration",
        .detail   = std::string{reason},
    };
}

auto validate_ca_bundle(std::filesystem::path const& path) -> VoidResult
{
    std::error_code error;
    auto const      status = std::filesystem::status(path, error);
    if (error) {
        return VoidResult::failure(invalid_options("ca_bundle.unavailable"));
    }
    if (!std::filesystem::is_regular_file(status)) {
        return VoidResult::failure(invalid_options("ca_bundle.not_regular_file"));
    }

    auto input = std::ifstream{path, std::ios::binary};
    if (!input.is_open()) {
        return VoidResult::failure(invalid_options("ca_bundle.not_readable"));
    }
    return VoidResult::success();
}

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

auto has_userinfo_part(CURLU* url, CURLUPart part) -> jb::core::Result<bool, jb::core::Error>
{
    char*      raw_value{nullptr};
    auto const result = curl_url_get(url, part, &raw_value, 0);
    auto       value  = CurlString{raw_value};
    if (result == CURLUE_OK) {
        return jb::core::Result<bool, jb::core::Error>::success(true);
    }
    auto const missing_part = part == CURLUPART_USER ? CURLUE_NO_USER : CURLUE_NO_PASSWORD;
    if (result == missing_part) {
        return jb::core::Result<bool, jb::core::Error>::success(false);
    }
    return jb::core::Result<bool, jb::core::Error>::failure(invalid_options("proxy.invalid_url"));
}

auto validate_proxy(std::string const& proxy, detail::CurlRuntimeCapabilities const& capabilities) -> VoidResult
{
    if (proxy.empty() || proxy.find('\0') != std::string::npos) {
        return VoidResult::failure(invalid_options("proxy.invalid_url"));
    }

    auto url = CurlUrl{curl_url()};
    if (!url) {
        return VoidResult::failure(runtime_unavailable("runtime.url_parser_unavailable"));
    }
    if (curl_url_set(url.get(), CURLUPART_URL, proxy.c_str(), 0) != CURLUE_OK) {
        return VoidResult::failure(invalid_options("proxy.invalid_url"));
    }

    auto scheme = CurlString{};
    auto host   = CurlString{};
    if (!get_url_part(url.get(), CURLUPART_SCHEME, scheme) || !get_url_part(url.get(), CURLUPART_HOST, host) || !host ||
        std::string_view{host.get()}.empty()) {
        return VoidResult::failure(invalid_options("proxy.invalid_absolute_url"));
    }

    auto const is_http  = scheme && ascii_equal(scheme.get(), "http");
    auto const is_https = scheme && ascii_equal(scheme.get(), "https");
    if (!is_http && !is_https) {
        return VoidResult::failure(invalid_options("proxy.unsupported_scheme"));
    }

    auto user_present = has_userinfo_part(url.get(), CURLUPART_USER);
    if (!user_present) {
        return VoidResult::failure(std::move(user_present).error());
    }
    auto password_present = has_userinfo_part(url.get(), CURLUPART_PASSWORD);
    if (!password_present) {
        return VoidResult::failure(std::move(password_present).error());
    }
    if (*user_present || *password_present) {
        return VoidResult::failure(invalid_options("proxy.userinfo_forbidden"));
    }

    if (is_https && !capabilities.supports_https_proxy) {
        return VoidResult::failure(runtime_unavailable("runtime.https_proxy_unavailable"));
    }
    return VoidResult::success();
}

} // anonymous namespace

struct SystemHttpClient::Private {
    explicit Private(SystemHttpClientOptions value)
        : options{std::move(value)}
    {}

    // The validated snapshot is retained so future transfer admission never borrows caller-owned configuration.
    [[maybe_unused]] SystemHttpClientOptions options;
};

auto SystemHttpClient::create(jb::core::EventLoop& loop, SystemHttpClientOptions options) -> ClientResult
{
    if (&loop != jb::core::EventLoop::current() || !loop.is_valid()) {
        return ClientResult::failure(event_loop_unavailable());
    }
    if (options.maximum_parsed_response_header_bytes == 0U ||
        options.maximum_parsed_response_header_bytes > kMaximumParsedResponseHeaderBytes) {
        return ClientResult::failure(invalid_options("maximum_parsed_response_header_bytes.out_of_range"));
    }
    if (options.ca_bundle) {
        auto ca_bundle = validate_ca_bundle(*options.ca_bundle);
        if (!ca_bundle) {
            return ClientResult::failure(std::move(ca_bundle).error());
        }
    }

    auto capabilities = detail::preflight_curl_runtime();
    if (!capabilities) {
        return ClientResult::failure(std::move(capabilities).error());
    }
    if (options.proxy) {
        auto proxy = validate_proxy(*options.proxy, *capabilities);
        if (!proxy) {
            return ClientResult::failure(std::move(proxy).error());
        }
    }

    return ClientResult::success(std::unique_ptr<SystemHttpClient>{new SystemHttpClient{std::move(options)}});
}

SystemHttpClient::SystemHttpClient(SystemHttpClientOptions options)
    : _data{std::make_unique<Private>(std::move(options))}
{}

SystemHttpClient::~SystemHttpClient() = default;

auto SystemHttpClient::is_available() const noexcept -> bool
{
    return false;
}

auto SystemHttpClient::start(jb::net::HttpRequest /*request*/, jb::net::HttpCompletionHandler /*completion*/)
    -> jb::core::Result<jb::net::HttpRequestId, jb::core::Error>
{
    return jb::core::Result<jb::net::HttpRequestId, jb::core::Error>::failure({
        .category = jb::core::ErrorCategory::Unavailable,
        .code     = "net.http.unavailable",
        .message  = "The system HTTP transfer engine is unavailable",
    });
}

auto SystemHttpClient::cancel(jb::net::HttpRequestId /*request_id*/) -> jb::core::Result<void, jb::core::Error>
{
    return jb::core::Result<void, jb::core::Error>::failure({
        .category = jb::core::ErrorCategory::NotFound,
        .code     = "net.http.request_not_found",
        .message  = "The HTTP request is not active",
    });
}

auto SystemHttpClient::active_request_count() const noexcept -> std::size_t
{
    return 0;
}

auto SystemHttpClient::failure() const -> std::optional<jb::core::Error>
{
    return std::nullopt;
}

} // namespace jb::net::http
