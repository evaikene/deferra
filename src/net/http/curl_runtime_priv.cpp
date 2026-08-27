#include "curl_runtime_priv.hpp"

#include <curl/curl.h>

#include <string>
#include <string_view>
#include <utility>

namespace jb::net::http::detail {

namespace {

using RuntimeResult = jb::core::Result<CurlRuntimeCapabilities, jb::core::Error>;
using VoidResult    = jb::core::Result<void, jb::core::Error>;

static_assert(kMinimumCurlRuntimeVersion == CURL_VERSION_BITS(7, 85, 0));

auto runtime_unavailable(std::string_view reason) -> jb::core::Error
{
    return {
        .category = jb::core::ErrorCategory::Unavailable,
        .code     = "net.http.runtime_unavailable",
        .message  = "The system HTTP runtime does not satisfy the required capabilities",
        .detail   = std::string{reason},
    };
}

class CurlRuntimeGuard {
public:
    CurlRuntimeGuard()
        : _initialization_result{curl_global_init(CURL_GLOBAL_DEFAULT)}
    {}

    ~CurlRuntimeGuard()
    {
        if (_initialization_result == CURLE_OK) {
            curl_global_cleanup();
        }
    }

    CurlRuntimeGuard(CurlRuntimeGuard const&)                    = delete;
    CurlRuntimeGuard(CurlRuntimeGuard&&)                         = delete;
    auto operator=(CurlRuntimeGuard const&) -> CurlRuntimeGuard& = delete;
    auto operator=(CurlRuntimeGuard&&) -> CurlRuntimeGuard&      = delete;

    [[nodiscard]] auto initialization_result() const noexcept -> CURLcode { return _initialization_result; }

private:
    CURLcode _initialization_result;
};

auto process_runtime() -> CurlRuntimeGuard&
{
    // Function-local initialization is thread-safe and registers cleanup before any client returned by the factory.
    static CurlRuntimeGuard runtime;
    return runtime;
}

auto supports_protocol(char const* const* protocols, std::string_view required) noexcept -> bool
{
    if (!protocols) {
        return false;
    }

    for (auto const* current = protocols; *current; ++current) {
        if (std::string_view{*current} == required) {
            return true;
        }
    }
    return false;
}

} // anonymous namespace

auto validate_curl_runtime(CurlRuntimeCapabilities const& capabilities) -> VoidResult
{
    if (capabilities.version_number < kMinimumCurlRuntimeVersion) {
        return VoidResult::failure(runtime_unavailable("runtime.version_too_old"));
    }
    if (!capabilities.supports_http) {
        return VoidResult::failure(runtime_unavailable("runtime.http_unavailable"));
    }
    if (!capabilities.supports_https) {
        return VoidResult::failure(runtime_unavailable("runtime.https_unavailable"));
    }
    if (!capabilities.supports_ssl) {
        return VoidResult::failure(runtime_unavailable("runtime.ssl_unavailable"));
    }
    if (!capabilities.supports_zlib) {
        return VoidResult::failure(runtime_unavailable("runtime.zlib_unavailable"));
    }
    if (!capabilities.supports_asynchronous_dns) {
        return VoidResult::failure(runtime_unavailable("runtime.asynchronous_dns_unavailable"));
    }
    return VoidResult::success();
}

auto preflight_curl_runtime() -> RuntimeResult
{
    auto& runtime = process_runtime();
    if (runtime.initialization_result() != CURLE_OK) {
        return RuntimeResult::failure(runtime_unavailable("runtime.global_initialization_failed"));
    }

    auto const* information = curl_version_info(CURLVERSION_NOW);
    if (!information) {
        return RuntimeResult::failure(runtime_unavailable("runtime.version_information_unavailable"));
    }

    auto const capabilities = CurlRuntimeCapabilities{
        .version_number            = information->version_num,
        .supports_http             = supports_protocol(information->protocols, "http"),
        .supports_https            = supports_protocol(information->protocols, "https"),
        .supports_ssl              = (information->features & CURL_VERSION_SSL) != 0,
        .supports_zlib             = (information->features & CURL_VERSION_LIBZ) != 0,
        .supports_asynchronous_dns = (information->features & CURL_VERSION_ASYNCHDNS) != 0,
        .supports_https_proxy      = (information->features & CURL_VERSION_HTTPS_PROXY) != 0,
    };
    auto validation = validate_curl_runtime(capabilities);
    if (!validation) {
        return RuntimeResult::failure(std::move(validation).error());
    }
    return RuntimeResult::success(capabilities);
}

} // namespace jb::net::http::detail
