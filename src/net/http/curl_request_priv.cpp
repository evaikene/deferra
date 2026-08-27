#include "curl_request_priv.hpp"

#include <limits>
#include <string_view>
#include <utility>

namespace jb::net::http::detail {

namespace {

using RequestResult = jb::core::Result<std::unique_ptr<CurlRequest>, jb::core::Error>;

auto request_backend_unavailable(std::string_view reason) -> jb::core::Error
{
    return {
        .category = jb::core::ErrorCategory::Unavailable,
        .code     = "net.http.unavailable",
        .message  = "The system HTTP client could not prepare the request",
        .detail   = std::string{reason},
    };
}

auto request_internal_error() -> HttpError
{
    return {
        .kind = HttpErrorKind::Internal,
        .error =
            {
                    .category = jb::core::ErrorCategory::Internal,
                    .code     = "net.http.internal",
                    .message  = "The HTTP request failed before detailed transport mapping was available",
                    },
    };
}

auto discard_response_bytes(char* /*data*/, std::size_t size, std::size_t count, void* /*context*/) noexcept
    -> std::size_t
{
    if (count != 0U && size > std::numeric_limits<std::size_t>::max() / count) {
        return 0U;
    }
    return size * count;
}

template <typename Value>
auto set_option(CURL* easy, CURLoption option, Value value) noexcept -> bool
{
    return curl_easy_setopt(easy, option, value) == CURLE_OK;
}

} // anonymous namespace

void CurlEasyDeleter::operator()(CURL* easy) const noexcept
{
    curl_easy_cleanup(easy);
}

auto CurlRequest::create(HttpRequestId id, HttpRequest request, HttpCompletionHandler completion) -> RequestResult
{
    auto easy = CurlEasy{curl_easy_init()};
    if (!easy) {
        return RequestResult::failure(request_backend_unavailable("request.easy_initialization_failed"));
    }

    auto result = std::unique_ptr<CurlRequest>{
        new CurlRequest{id, std::move(request), std::move(completion), std::move(easy)}
    };
    auto* handle = result->easy();

    // Stage 5.3 admits only a minimal GET, but every network effect still receives the security-critical backend
    // baseline. The write sink drains bytes without exposing libcurl's default output destination.
    auto const configured =
        set_option(handle, CURLOPT_URL, result->_request.url.c_str()) && set_option(handle, CURLOPT_HTTPGET, 1L) &&
        set_option(handle, CURLOPT_NOSIGNAL, 1L) && set_option(handle, CURLOPT_PROTOCOLS_STR, "http,https") &&
        set_option(handle, CURLOPT_FOLLOWLOCATION, 0L) && set_option(handle, CURLOPT_ACCEPT_ENCODING, "") &&
        set_option(handle, CURLOPT_PROXY, "") && set_option(handle, CURLOPT_NOPROXY, "") &&
        set_option(handle, CURLOPT_SSL_VERIFYPEER, 1L) && set_option(handle, CURLOPT_SSL_VERIFYHOST, 2L) &&
        set_option(handle, CURLOPT_WRITEFUNCTION, &discard_response_bytes) &&
        set_option(handle, CURLOPT_WRITEDATA, result.get()) && set_option(handle, CURLOPT_PRIVATE, result.get());
    if (!configured) {
        return RequestResult::failure(request_backend_unavailable("request.easy_configuration_failed"));
    }

    return RequestResult::success(std::move(result));
}

CurlRequest::CurlRequest(HttpRequestId id, HttpRequest request, HttpCompletionHandler completion, CurlEasy easy)
    : _id{id}
    , _request{std::move(request)}
    , _completion{std::move(completion)}
    , _easy{std::move(easy)}
{}

void CurlRequest::prepare_completion(HttpCompletionResult result)
{
    _state  = State::PendingCompletion;
    _result = std::move(result);
}

auto CurlRequest::take_handler() -> HttpCompletionHandler
{
    return std::move(_completion);
}

auto CurlRequest::take_result() -> HttpCompletionResult
{
    return std::move(_result).value();
}

auto CurlRequest::minimal_transfer_result(CURLcode result) const -> HttpCompletionResult
{
    if (result != CURLE_OK) {
        return HttpCompletionResult::failure(request_internal_error());
    }

    long status_code{0};
    if (curl_easy_getinfo(_easy.get(), CURLINFO_RESPONSE_CODE, &status_code) != CURLE_OK || status_code < 0L ||
        status_code > static_cast<long>(std::numeric_limits<std::uint16_t>::max())) {
        return HttpCompletionResult::failure(request_internal_error());
    }

    return HttpCompletionResult::success(HttpResponse{.status_code = static_cast<std::uint16_t>(status_code)});
}

} // namespace jb::net::http::detail
