#include "http/system_http_client.hpp"

#include "application.hpp"
#include "event_loop_types.hpp"
#include "http/curl_multi_test_priv.hpp"
#include "http/curl_runtime_priv.hpp"
#include "support/fake_event_loop_backend.hpp"
#include "support/http_test_server.hpp"
#include "support/temporary_directory.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using jb::net::http::SystemHttpClient;
using jb::net::http::SystemHttpClientOptions;
using namespace std::chrono_literals;

auto minimal_request(std::string url) -> jb::net::HttpRequest
{
    return {.url = std::move(url)};
}

template <typename Predicate>
auto process_until(jb::core::Application& app, Predicate&& predicate, std::chrono::milliseconds timeout = 5s) -> bool
{
    auto const deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        if (app.process_events(jb::core::EventFlag::All, 25) == jb::core::ProcessEventsResult::Failed) {
            return false;
        }
    }
    return predicate();
}

void drain_fake_loop(jb::core::EventLoop& loop)
{
    for (auto index = 0; index < 8; ++index) {
        REQUIRE(loop.process_events(jb::core::EventFlag::All, 0) != jb::core::ProcessEventsResult::Failed);
    }
}

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

TEST_CASE("system HTTP runtime preflight creates a ready minimal client", "[net][http]")
{
    auto app    = jb::core::Application{0, nullptr};
    auto result = SystemHttpClient::create(*app.event_loop());
    REQUIRE(result);

    auto client = std::move(result).value();
    REQUIRE(client);
    CHECK(client->parent() == nullptr);
    CHECK(client->event_loop() == app.event_loop());
    CHECK(client->is_available());
    CHECK(client->active_request_count() == 0U);
    CHECK_FALSE(client->failure());

    auto callback_called = false;
    auto start =
        client->start({}, [&callback_called](jb::net::HttpRequestId, jb::net::HttpCompletionResult const&) -> void {
            callback_called = true;
        });
    REQUIRE_FALSE(start);
    CHECK(start.error().code == "net.http.invalid_request");
    CHECK_FALSE(callback_called);

    auto cancellation = client->cancel(1U);
    REQUIRE_FALSE(cancellation);
    CHECK(cancellation.error().code == "net.http.request_not_found");
    CHECK_FALSE(callback_called);
}

TEST_CASE("system HTTP client rejects semantics outside the Stage 5.3 subset", "[net][http]")
{
    auto app    = jb::core::Application{0, nullptr};
    auto result = SystemHttpClient::create(*app.event_loop());
    REQUIRE(result);
    auto client = std::move(result).value();

    auto callback = [](jb::net::HttpRequestId, jb::net::HttpCompletionResult const&) -> void {};

    auto request   = minimal_request("http://127.0.0.1:1/");
    request.method = "POST";
    auto start     = client->start(std::move(request), callback);
    REQUIRE_FALSE(start);
    CHECK(start.error().detail == "stage_5_3.method_not_supported");

    request = minimal_request("http://127.0.0.1:1/");
    request.headers.push_back({.name = "X-Test", .value = "value"});
    start = client->start(std::move(request), callback);
    REQUIRE_FALSE(start);
    CHECK(start.error().detail == "stage_5_3.headers_not_supported");

    request      = minimal_request("http://127.0.0.1:1/");
    request.body = jb::core::ByteBuffer{};
    start        = client->start(std::move(request), callback);
    REQUIRE_FALSE(start);
    CHECK(start.error().detail == "stage_5_3.body_not_supported");

    request                  = minimal_request("http://127.0.0.1:1/");
    request.follow_redirects = true;
    start                    = client->start(std::move(request), callback);
    REQUIRE_FALSE(start);
    CHECK(start.error().detail == "stage_5_3.redirects_not_supported");

    request = minimal_request("https://127.0.0.1:1/");
    start   = client->start(std::move(request), callback);
    REQUIRE_FALSE(start);
    CHECK(start.error().detail == "stage_5_3.https_not_supported");

    request            = minimal_request("http://127.0.0.1:1/");
    request.verify_tls = false;
    start              = client->start(std::move(request), callback);
    REQUIRE_FALSE(start);
    CHECK(start.error().detail == "stage_5_3.unsafe_tls_not_supported");

    request = minimal_request("http://127.0.0.1:1/");
    start   = client->start(std::move(request), {});
    REQUIRE_FALSE(start);
    CHECK(start.error().detail == "completion.empty");

    CHECK(client->is_available());
    CHECK(client->active_request_count() == 0U);

    auto proxied = SystemHttpClient::create(*app.event_loop(), SystemHttpClientOptions{.proxy = "http://127.0.0.1:1"});
    REQUIRE(proxied);
    start = (*proxied)->start(minimal_request("http://127.0.0.1:1/"), callback);
    REQUIRE_FALSE(start);
    CHECK(start.error().detail == "stage_5_3.proxy_not_supported");
}

TEST_CASE("system HTTP client completes a loopback GET strictly after start", "[net][http]")
{
    auto app    = jb::core::Application{0, nullptr};
    auto server = jb::test::HttpTestServer{};
    server.release_responses();
    auto created = SystemHttpClient::create(*app.event_loop());
    REQUIRE(created);
    auto client = std::move(created).value();

    auto callback_count = 0;
    auto inside_start   = true;
    auto completion     = std::optional<jb::net::HttpCompletionResult>{};
    auto started        = client->start(minimal_request(server.url()),
                                        [&](jb::net::HttpRequestId id, jb::net::HttpCompletionResult result) -> void {
                                     CHECK_FALSE(inside_start);
                                     CHECK(id == 1U);
                                     ++callback_count;
                                     completion = std::move(result);
                                        });
    inside_start        = false;

    REQUIRE(started);
    CHECK(*started == 1U);
    CHECK(callback_count == 0);
    CHECK(client->active_request_count() == 1U);
    REQUIRE(process_until(app, [&]() -> bool { return callback_count == 1; }));
    REQUIRE(completion);
    REQUIRE(*completion);
    CHECK(completion->value().status_code == 200U);
    CHECK(client->active_request_count() == 0U);
    CHECK(client->is_available());
}

TEST_CASE("system HTTP client drives two held GETs concurrently", "[net][http]")
{
    auto app     = jb::core::Application{0, nullptr};
    auto server  = jb::test::HttpTestServer{};
    auto created = SystemHttpClient::create(*app.event_loop());
    REQUIRE(created);
    auto client = std::move(created).value();

    auto callback_count = 0;
    auto completion     = [&](jb::net::HttpRequestId, jb::net::HttpCompletionResult result) -> void {
        REQUIRE(result);
        ++callback_count;
    };
    auto first = client->start(minimal_request(server.url("/first")), completion);
    REQUIRE(first);
    auto second = client->start(minimal_request(server.url("/second")), completion);
    REQUIRE(second);
    CHECK(*second > *first);
    CHECK(client->active_request_count() == 2U);

    REQUIRE(process_until(app, [&]() -> bool { return server.wait_for_requests(2U, 0ms); }));
    CHECK(callback_count == 0);
    CHECK(client->active_request_count() == 2U);

    server.release_responses();
    REQUIRE(process_until(app, [&]() -> bool { return callback_count == 2; }));
    CHECK(client->active_request_count() == 0U);
}

TEST_CASE("system HTTP cancellation is asynchronous and exact once", "[net][http]")
{
    auto app     = jb::core::Application{0, nullptr};
    auto server  = jb::test::HttpTestServer{};
    auto created = SystemHttpClient::create(*app.event_loop());
    REQUIRE(created);
    auto client = std::move(created).value();

    auto callback_count = 0;
    auto completion     = std::optional<jb::net::HttpCompletionResult>{};
    auto started        = client->start(minimal_request(server.url()),
                                        [&](jb::net::HttpRequestId, jb::net::HttpCompletionResult result) -> void {
                                     ++callback_count;
                                     completion = std::move(result);
                                        });
    REQUIRE(started);
    REQUIRE(process_until(app, [&]() -> bool { return server.wait_for_requests(1U, 0ms); }));

    auto cancelled_result = client->cancel(*started);
    REQUIRE(cancelled_result);
    CHECK(callback_count == 0);
    auto repeated = client->cancel(*started);
    REQUIRE_FALSE(repeated);
    CHECK(repeated.error().code == "net.http.request_not_found");

    REQUIRE(process_until(app, [&]() -> bool { return callback_count == 1; }));
    REQUIRE(completion);
    REQUIRE_FALSE(*completion);
    CHECK(completion->error().kind == jb::net::HttpErrorKind::Cancelled);
    CHECK(completion->error().error.code == "net.http.cancelled");
    CHECK(client->active_request_count() == 0U);
    server.release_responses();
}

TEST_CASE("system HTTP client suppresses callbacks during destruction", "[net][http]")
{
    auto app     = jb::core::Application{0, nullptr};
    auto server  = jb::test::HttpTestServer{};
    auto created = SystemHttpClient::create(*app.event_loop());
    REQUIRE(created);
    auto client = std::move(created).value();

    auto callback_count = 0;
    auto started =
        client->start(minimal_request(server.url()),
                      [&](jb::net::HttpRequestId, jb::net::HttpCompletionResult const&) -> void { ++callback_count; });
    REQUIRE(started);
    REQUIRE(process_until(app, [&]() -> bool { return server.wait_for_requests(1U, 0ms); }));

    client.reset();
    server.release_responses();
    for (auto index = 0; index < 4; ++index) {
        REQUIRE(app.process_events(jb::core::EventFlag::All, 0) != jb::core::ProcessEventsResult::Failed);
    }
    CHECK(callback_count == 0);
}

TEST_CASE("system HTTP client rejects the triggering start when initial drive posting fails", "[net][http]")
{
    auto fake         = jb::core::priv::make_fake_event_loop();
    auto current_loop = jb::core::priv::ScopedCurrentEventLoop{fake.loop.get()};
    auto created      = SystemHttpClient::create(*fake.loop);
    REQUIRE(created);
    auto client = std::move(created).value();

    auto first_callbacks  = 0;
    auto second_callbacks = 0;
    auto failed_count     = 0;
    client->failed.connect([&](jb::core::Error const&) -> void {
        CHECK_FALSE(client->is_available());
        REQUIRE(client->failure());
        ++failed_count;
    });
    auto first = client->start(minimal_request("http://127.0.0.1:1/first"),
                               [&](jb::net::HttpRequestId, jb::net::HttpCompletionResult result) -> void {
                                   REQUIRE_FALSE(result);
                                   CHECK(result.error().kind == jb::net::HttpErrorKind::Internal);
                                   ++first_callbacks;
                               });
    REQUIRE(first);

    fake.backend->wakeup_result = false;
    auto second                 = client->start(
        minimal_request("http://127.0.0.1:1/second"),
        [&](jb::net::HttpRequestId, jb::net::HttpCompletionResult const&) -> void { ++second_callbacks; });
    REQUIRE_FALSE(second);
    CHECK(second.error().code == "net.http.backend_failed");
    CHECK_FALSE(client->is_available());
    REQUIRE(client->failure());
    CHECK(client->active_request_count() == 1U);

    fake.backend->wakeup_result = true;
    drain_fake_loop(*fake.loop);
    CHECK(first_callbacks == 1);
    CHECK(second_callbacks == 0);
    CHECK(failed_count == 1);
    CHECK(client->active_request_count() == 0U);
}

TEST_CASE("system HTTP adapter failures complete accepted requests and fail once", "[net][http]")
{
    auto const failure_point = GENERATE(jb::net::http::detail::CurlMultiFailurePoint::TimerRegistration,
                                        jb::net::http::detail::CurlMultiFailurePoint::SocketAction);

    auto fake         = jb::core::priv::make_fake_event_loop();
    auto current_loop = jb::core::priv::ScopedCurrentEventLoop{fake.loop.get()};
    auto created      = SystemHttpClient::create(*fake.loop);
    REQUIRE(created);
    auto client = std::move(created).value();

    auto callback_count = 0;
    auto failed_count   = 0;
    client->failed.connect([&](jb::core::Error const& error) -> void {
        CHECK(error.code == "net.http.backend_failed");
        CHECK_FALSE(client->is_available());
        REQUIRE(client->failure());
        ++failed_count;
    });
    auto completion = [&](jb::net::HttpRequestId, jb::net::HttpCompletionResult result) -> void {
        REQUIRE_FALSE(result);
        CHECK(result.error().kind == jb::net::HttpErrorKind::Internal);
        CHECK(result.error().error.code == "net.http.backend_failed");
        ++callback_count;
    };
    REQUIRE(client->start(minimal_request("http://127.0.0.1:1/first"), completion));
    REQUIRE(client->start(minimal_request("http://127.0.0.1:1/second"), completion));
    CHECK(client->active_request_count() == 2U);

    jb::net::http::detail::fail_next_curl_multi_operation_for_testing(failure_point);
    drain_fake_loop(*fake.loop);

    CHECK(callback_count == 2);
    CHECK(failed_count == 1);
    CHECK(client->active_request_count() == 0U);
    CHECK_FALSE(client->is_available());
    REQUIRE(client->failure());

    auto later = client->start(minimal_request("http://127.0.0.1:1/later"), completion);
    REQUIRE_FALSE(later);
    CHECK(later.error().code == "net.http.unavailable");
    drain_fake_loop(*fake.loop);
    CHECK(callback_count == 2);
    CHECK(failed_count == 1);
}

TEST_CASE("system HTTP client fails closed when EventLoop watch registration fails", "[net][http]")
{
    auto fake         = jb::core::priv::make_fake_event_loop();
    auto current_loop = jb::core::priv::ScopedCurrentEventLoop{fake.loop.get()};
    auto created      = SystemHttpClient::create(*fake.loop);
    REQUIRE(created);
    auto client = std::move(created).value();

    auto callback_count = 0;
    auto failed_count   = 0;
    client->failed.connect([&](jb::core::Error const&) -> void { ++failed_count; });
    auto started =
        client->start(minimal_request("http://127.0.0.1:1/"),
                      [&](jb::net::HttpRequestId, jb::net::HttpCompletionResult const&) -> void { ++callback_count; });
    REQUIRE(started);

    fake.backend->add_fd_result = false;
    drain_fake_loop(*fake.loop);

    CHECK_FALSE(client->is_available());
    REQUIRE(client->failure());
    CHECK(client->failure()->detail == "event_loop.watch_registration_failed");
    CHECK(callback_count == 1);
    CHECK(failed_count == 1);
}

TEST_CASE("system HTTP client leaves persistent failed-watch callbacks inert", "[net][http]")
{
    auto fake         = jb::core::priv::make_fake_event_loop();
    auto current_loop = jb::core::priv::ScopedCurrentEventLoop{fake.loop.get()};
    auto created      = SystemHttpClient::create(*fake.loop);
    REQUIRE(created);
    auto client = std::move(created).value();

    auto callback_count = 0;
    REQUIRE(
        client->start(minimal_request("http://127.0.0.1:1/"),
                      [&](jb::net::HttpRequestId, jb::net::HttpCompletionResult const&) -> void { ++callback_count; }));
    drain_fake_loop(*fake.loop);
    REQUIRE(fake.backend->add_fd_calls > 0);
    auto const watched_fd = fake.backend->last_added_fd;

    fake.backend->remove_fd_result = false;
    client.reset();
    fake.backend->ready_events.push_back({.fd = watched_fd, .events = jb::core::FdEvent::Read});
    REQUIRE(fake.loop->process_events(jb::core::EventFlag::Watchers, 0) != jb::core::ProcessEventsResult::Failed);
    CHECK(callback_count == 0);
}

TEST_CASE("system HTTP client factory reports multi construction failure without a client", "[net][http]")
{
    auto app = jb::core::Application{0, nullptr};
    jb::net::http::detail::fail_next_curl_multi_operation_for_testing(
        jb::net::http::detail::CurlMultiFailurePoint::Initialization);

    auto created = SystemHttpClient::create(*app.event_loop());
    REQUIRE_FALSE(created);
    CHECK(created.error().code == "net.http.backend_failed");
    CHECK(created.error().detail == "multi.initialization_failed");
}
