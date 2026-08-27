#include "http/system_http_client.hpp"

#include "application.hpp"
#include "http/curl_runtime_priv.hpp"
#include "support/temporary_directory.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>

namespace {

using jb::net::http::SystemHttpClient;
using jb::net::http::SystemHttpClientOptions;

auto complete_runtime_capabilities() -> jb::net::http::detail::CurlRuntimeCapabilities
{
    return {
        .version_number            = jb::net::http::detail::kMinimumCurlRuntimeVersion,
        .supports_http             = true,
        .supports_https            = true,
        .supports_ssl              = true,
        .supports_zlib             = true,
        .supports_asynchronous_dns = true,
        .supports_https_proxy      = false,
    };
}

void check_runtime_rejection(jb::net::http::detail::CurlRuntimeCapabilities capabilities, std::string_view reason)
{
    auto result = jb::net::http::detail::validate_curl_runtime(capabilities);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == "net.http.runtime_unavailable");
    CHECK(result.error().detail == reason);
}

void check_safe_option_error(jb::core::Result<std::unique_ptr<SystemHttpClient>, jb::core::Error> const& result,
                             std::string_view                                                            secret)
{
    REQUIRE_FALSE(result);
    CHECK(result.error().code == "net.http.invalid_options");
    CHECK(result.error().message.find(secret) == std::string::npos);
    CHECK(result.error().detail.find(secret) == std::string::npos);
}

} // anonymous namespace

TEST_CASE("curl runtime validation requires every mandatory capability", "[net][http]")
{
    auto capabilities = complete_runtime_capabilities();
    CHECK(jb::net::http::detail::validate_curl_runtime(capabilities));

    capabilities.version_number = jb::net::http::detail::kMinimumCurlRuntimeVersion - 1U;
    check_runtime_rejection(capabilities, "runtime.version_too_old");

    capabilities               = complete_runtime_capabilities();
    capabilities.supports_http = false;
    check_runtime_rejection(capabilities, "runtime.http_unavailable");

    capabilities                = complete_runtime_capabilities();
    capabilities.supports_https = false;
    check_runtime_rejection(capabilities, "runtime.https_unavailable");

    capabilities              = complete_runtime_capabilities();
    capabilities.supports_ssl = false;
    check_runtime_rejection(capabilities, "runtime.ssl_unavailable");

    capabilities               = complete_runtime_capabilities();
    capabilities.supports_zlib = false;
    check_runtime_rejection(capabilities, "runtime.zlib_unavailable");

    capabilities                           = complete_runtime_capabilities();
    capabilities.supports_asynchronous_dns = false;
    check_runtime_rejection(capabilities, "runtime.asynchronous_dns_unavailable");
}

TEST_CASE("system HTTP client factory requires the current EventLoop", "[net][http]")
{
    auto loop   = jb::core::EventLoop{};
    auto client = SystemHttpClient::create(loop);
    REQUIRE_FALSE(client);
    CHECK(client.error().code == "net.http.event_loop_unavailable");

    auto app        = jb::core::Application{0, nullptr};
    auto other_loop = jb::core::EventLoop{};
    client          = SystemHttpClient::create(other_loop);
    REQUIRE_FALSE(client);
    CHECK(client.error().code == "net.http.event_loop_unavailable");
}

TEST_CASE("system HTTP client validates owning options without leaking their values", "[net][http]")
{
    auto app       = jb::core::Application{0, nullptr};
    auto directory = jb::test::TemporaryDirectory{};

    auto options                                 = SystemHttpClientOptions{};
    options.maximum_parsed_response_header_bytes = 0U;
    auto client                                  = SystemHttpClient::create(*app.event_loop(), options);
    check_safe_option_error(client, "0");

    options.maximum_parsed_response_header_bytes = (std::size_t{64} * 1024U * 1024U) + 1U;
    client                                       = SystemHttpClient::create(*app.event_loop(), options);
    check_safe_option_error(client, "67108865");

    options.maximum_parsed_response_header_bytes = std::size_t{64} * 1024U * 1024U;
    client                                       = SystemHttpClient::create(*app.event_loop(), options);
    REQUIRE(client);

    auto const secret_ca_path = directory.path() / "secret-ca-bundle.pem";
    options                   = SystemHttpClientOptions{.ca_bundle = secret_ca_path};
    client                    = SystemHttpClient::create(*app.event_loop(), options);
    check_safe_option_error(client, "secret-ca-bundle.pem");

    options = SystemHttpClientOptions{.ca_bundle = directory.path()};
    client  = SystemHttpClient::create(*app.event_loop(), options);
    check_safe_option_error(client, directory.path().filename().string());

    auto ca_bundle = std::ofstream{secret_ca_path, std::ios::binary};
    REQUIRE(ca_bundle.is_open());
    ca_bundle << "test CA data";
    ca_bundle.close();

    options = SystemHttpClientOptions{.ca_bundle = secret_ca_path};
    client  = SystemHttpClient::create(*app.event_loop(), options);
    REQUIRE(client);

    auto const secret_proxy = std::string{"http://secret-user:secret-password@example.test"};
    options                 = SystemHttpClientOptions{.proxy = secret_proxy};
    client                  = SystemHttpClient::create(*app.event_loop(), options);
    check_safe_option_error(client, "secret-user");
    CHECK(client.error().message.find("secret-password") == std::string::npos);
    CHECK(client.error().detail.find("secret-password") == std::string::npos);

    options = SystemHttpClientOptions{.proxy = "relative-proxy"};
    client  = SystemHttpClient::create(*app.event_loop(), options);
    check_safe_option_error(client, "relative-proxy");

    options = SystemHttpClientOptions{.proxy = "ftp://example.test"};
    client  = SystemHttpClient::create(*app.event_loop(), options);
    check_safe_option_error(client, "ftp://example.test");

    options = SystemHttpClientOptions{.proxy = "http://127.0.0.1:1"};
    client  = SystemHttpClient::create(*app.event_loop(), options);
    REQUIRE(client);
}

TEST_CASE("system HTTP runtime preflight creates a construction-only client", "[net][http]")
{
    auto app    = jb::core::Application{0, nullptr};
    auto result = SystemHttpClient::create(*app.event_loop());
    REQUIRE(result);

    auto client = std::move(result).value();
    REQUIRE(client);
    CHECK(client->parent() == nullptr);
    CHECK(client->event_loop() == app.event_loop());
    CHECK_FALSE(client->is_available());
    CHECK(client->active_request_count() == 0U);
    CHECK_FALSE(client->failure());

    auto callback_called = false;
    auto start =
        client->start({}, [&callback_called](jb::net::HttpRequestId, jb::net::HttpCompletionResult const&) -> void {
            callback_called = true;
        });
    REQUIRE_FALSE(start);
    CHECK(start.error().code == "net.http.unavailable");
    CHECK_FALSE(callback_called);

    auto cancellation = client->cancel(1U);
    REQUIRE_FALSE(cancellation);
    CHECK(cancellation.error().code == "net.http.request_not_found");
    CHECK_FALSE(callback_called);
}
