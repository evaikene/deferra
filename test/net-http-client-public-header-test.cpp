#include "http_client.hpp"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>

namespace {

class FakeHttpClient final : public jb::net::HttpClient {
public:
    [[nodiscard]] auto is_available() const noexcept -> bool override { return false; }

    [[nodiscard]] auto start(jb::net::HttpRequest request, jb::net::HttpCompletionHandler completion)
        -> jb::core::Result<jb::net::HttpRequestId, jb::core::Error> override
    {
        (void)request;
        (void)completion;
        return jb::core::Result<jb::net::HttpRequestId, jb::core::Error>::failure({
            .category = jb::core::ErrorCategory::Unavailable,
            .code     = "net.http.unavailable",
            .message  = "HTTP client is unavailable",
        });
    }

    [[nodiscard]] auto cancel(jb::net::HttpRequestId request_id) -> jb::core::Result<void, jb::core::Error> override
    {
        (void)request_id;
        return jb::core::Result<void, jb::core::Error>::failure({
            .category = jb::core::ErrorCategory::NotFound,
            .code     = "net.http.request_not_found",
            .message  = "HTTP request is not active",
        });
    }

    [[nodiscard]] auto active_request_count() const noexcept -> std::size_t override { return 0; }

    [[nodiscard]] auto failure() const -> std::optional<jb::core::Error> override { return std::nullopt; }
};

} // anonymous namespace

using StartResult  = jb::core::Result<jb::net::HttpRequestId, jb::core::Error>;
using CancelResult = jb::core::Result<void, jb::core::Error>;

static_assert(std::is_same_v<std::underlying_type_t<jb::net::HttpErrorKind>, std::uint8_t>);
static_assert(std::is_move_constructible_v<jb::net::HttpRequest>);
static_assert(std::is_move_constructible_v<jb::net::HttpResponse>);
static_assert(std::is_move_constructible_v<jb::net::HttpError>);
static_assert(
    std::is_invocable_r_v<void, jb::net::HttpCompletionHandler, jb::net::HttpRequestId, jb::net::HttpCompletionResult>);

static_assert(std::is_base_of_v<jb::core::Object, jb::net::HttpClient>);
static_assert(std::is_abstract_v<jb::net::HttpClient>);
static_assert(std::has_virtual_destructor_v<jb::net::HttpClient>);
static_assert(!std::is_copy_constructible_v<jb::net::HttpClient>);
static_assert(!std::is_move_constructible_v<jb::net::HttpClient>);
static_assert(std::is_final_v<FakeHttpClient>);
static_assert(std::same_as<decltype(jb::net::HttpClient::failed), jb::core::Signal<jb::core::Error>>);
static_assert(
    std::is_same_v<decltype(&jb::net::HttpClient::is_available), bool (jb::net::HttpClient::*)() const noexcept>);
static_assert(
    std::is_same_v<decltype(&jb::net::HttpClient::start),
                   StartResult (jb::net::HttpClient::*)(jb::net::HttpRequest, jb::net::HttpCompletionHandler)>);
static_assert(std::is_same_v<decltype(&jb::net::HttpClient::cancel),
                             CancelResult (jb::net::HttpClient::*)(jb::net::HttpRequestId)>);
static_assert(std::is_same_v<decltype(&jb::net::HttpClient::active_request_count),
                             std::size_t (jb::net::HttpClient::*)() const noexcept>);
static_assert(std::is_same_v<decltype(&jb::net::HttpClient::failure),
                             std::optional<jb::core::Error> (jb::net::HttpClient::*)() const>);

auto main() -> int
{
    auto client = FakeHttpClient{};
    return client.parent() == nullptr && !client.is_available() && client.active_request_count() == 0U &&
                   !client.failure()
             ? 0
             : 1;
}
