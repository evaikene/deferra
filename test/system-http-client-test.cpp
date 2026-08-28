#include "http/system_http_client.hpp"

#include "application.hpp"
#include "event_loop_types.hpp"
#include "http/curl_multi_priv.hpp"
#include "http/curl_multi_test_priv.hpp"
#include "http/curl_runtime_priv.hpp"
#include "support/fake_event_loop_backend.hpp"
#include "support/http_test_server.hpp"
#include "support/temporary_directory.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
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

auto bytes(std::string_view value) -> jb::core::ByteBuffer
{
    auto const view = jb::core::as_bytes(value);
    return {view.begin(), view.end()};
}

auto byte_text(jb::core::ByteBuffer const& value) -> std::string
{
    auto const view = jb::core::as_string_view(value);
    return {view.data(), view.size()};
}

constexpr auto hex_nibble(char character) -> std::uint8_t
{
    if (character >= '0' && character <= '9') {
        return static_cast<std::uint8_t>(character - '0');
    }
    if (character >= 'a' && character <= 'f') {
        return static_cast<std::uint8_t>(character - 'a' + 10);
    }
    throw std::invalid_argument{"invalid hexadecimal fixture"};
}

auto hex_bytes(std::string_view value) -> jb::core::ByteBuffer
{
    if (value.size() % 2U != 0U) {
        throw std::invalid_argument{"invalid hexadecimal fixture length"};
    }
    auto result = jb::core::ByteBuffer{};
    result.reserve(value.size() / 2U);
    for (std::size_t index = 0; index < value.size(); index += 2U) {
        auto const byte = static_cast<std::uint8_t>((hex_nibble(value[index]) << 4U) | hex_nibble(value[index + 1U]));
        result.push_back(static_cast<std::byte>(byte));
    }
    return result;
}

auto header_value(std::vector<jb::test::HttpTestHeader> const& headers, std::string_view name)
    -> std::optional<std::string_view>
{
    auto const match = std::ranges::find_if(headers, [name](jb::test::HttpTestHeader const& header) {
        return std::ranges::equal(header.name, name, [](char left, char right) {
            auto lower = [](char value) -> char {
                return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
            };
            return lower(left) == lower(right);
        });
    });
    if (match == headers.end()) {
        return std::nullopt;
    }
    return match->value;
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

auto complete_request(jb::core::Application& app, SystemHttpClient& client, jb::net::HttpRequest request)
    -> jb::net::HttpCompletionResult
{
    auto completion = std::optional<jb::net::HttpCompletionResult>{};
    auto started    = client.start(std::move(request),
                                   [&completion](jb::net::HttpRequestId, jb::net::HttpCompletionResult result) -> void {
                                    completion = std::move(result);
                                   });
    if (!started) {
        throw std::runtime_error{started.error().code};
    }
    if (!process_until(app, [&completion]() -> bool { return completion.has_value(); })) {
        throw std::runtime_error{"HTTP completion timed out"};
    }
    return std::move(*completion);
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

TEST_CASE("system HTTP client keeps later transport policy outside the Stage 5.4 scope", "[net][http]")
{
    auto app    = jb::core::Application{0, nullptr};
    auto result = SystemHttpClient::create(*app.event_loop());
    REQUIRE(result);
    auto client = std::move(result).value();

    auto callback = [](jb::net::HttpRequestId, jb::net::HttpCompletionResult const&) -> void {};

    auto request             = minimal_request("http://127.0.0.1:1/");
    request.follow_redirects = true;
    auto start               = client->start(std::move(request), callback);
    REQUIRE_FALSE(start);
    CHECK(start.error().detail == "stage_5_4.redirects_not_supported");

    request = minimal_request("https://127.0.0.1:1/");
    start   = client->start(std::move(request), callback);
    REQUIRE_FALSE(start);
    CHECK(start.error().detail == "stage_5_4.https_not_supported");

    request            = minimal_request("http://127.0.0.1:1/");
    request.verify_tls = false;
    start              = client->start(std::move(request), callback);
    REQUIRE_FALSE(start);
    CHECK(start.error().detail == "stage_5_4.unsafe_tls_not_supported");

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
    CHECK(start.error().detail == "stage_5_4.proxy_not_supported");
}

TEST_CASE("system HTTP client preserves methods headers and optional binary bodies", "[net][http]")
{
    auto app     = jb::core::Application{0, nullptr};
    auto server  = jb::test::HttpTestServer{};
    auto created = SystemHttpClient::create(*app.event_loop());
    REQUIRE(created);
    auto client = std::move(created).value();
    server.release_responses();

    auto const methods = std::vector<std::string>{"GET", "HEAD", "POST", "PUT", "PATCH", "DELETE", "PURGE"};
    for (auto const& method : methods) {
        auto request   = minimal_request(server.url("/method-" + method));
        request.method = method;
        auto result    = complete_request(app, *client, std::move(request));
        REQUIRE(result);
        CHECK(result->status_code == 200U);
        if (method == "HEAD") {
            CHECK(result->body.bytes.empty());
            CHECK(result->body.total_bytes == 0U);
        }
        else {
            CHECK(byte_text(result->body.bytes) == "test-response");
        }
    }

    auto absent_body   = minimal_request(server.url("/absent"));
    absent_body.method = "POST";
    REQUIRE(complete_request(app, *client, std::move(absent_body)));

    auto empty_body   = minimal_request(server.url("/empty"));
    empty_body.method = "POST";
    empty_body.body   = jb::core::ByteBuffer{};
    REQUIRE(complete_request(app, *client, std::move(empty_body)));

    auto const binary_body = jb::core::ByteBuffer{
        std::byte{0x00},
        std::byte{0x41},
        std::byte{0xff},
        std::byte{0x7f},
    };
    auto binary_request   = minimal_request(server.url("/binary"));
    binary_request.method = "PATCH";
    binary_request.body   = binary_body;
    REQUIRE(complete_request(app, *client, std::move(binary_request)));

    auto header_request    = minimal_request(server.url("/headers"));
    header_request.method  = "PUT";
    header_request.headers = {
        {.name = "X-First", .value = "one"},
        {.name = "x-Second", .value = "two", .sensitive = true},
    };
    REQUIRE(complete_request(app, *client, std::move(header_request)));

    auto large_request   = minimal_request(server.url("/large-body"));
    large_request.method = "POST";
    large_request.body   = jb::core::ByteBuffer((std::size_t{1024} * 1024U) + 1U, std::byte{0x5a});
    REQUIRE(complete_request(app, *client, std::move(large_request)));

    auto get_body   = minimal_request(server.url("/get-body"));
    get_body.body   = binary_body;
    auto get_result = complete_request(app, *client, std::move(get_body));
    REQUIRE(get_result);

    auto const recorded = server.requests();
    REQUIRE(recorded.size() == methods.size() + 6U);
    for (std::size_t index = 0; index < methods.size(); ++index) {
        CHECK(recorded[index].method == methods[index]);
    }

    auto const& absent = recorded[methods.size()];
    CHECK(absent.body.empty());
    CHECK_FALSE(header_value(absent.headers, "Content-Length"));

    auto const& empty = recorded[methods.size() + 1U];
    CHECK(empty.body.empty());
    REQUIRE(header_value(empty.headers, "Content-Length"));
    CHECK(*header_value(empty.headers, "Content-Length") == "0");

    auto const& binary = recorded[methods.size() + 2U];
    CHECK(binary.body == binary_body);
    REQUIRE(header_value(binary.headers, "Content-Length"));
    CHECK(*header_value(binary.headers, "Content-Length") == "4");

    auto const& headers = recorded[methods.size() + 3U].headers;
    auto const  first =
        std::ranges::find_if(headers, [](jb::test::HttpTestHeader const& header) { return header.name == "X-First"; });
    auto const second =
        std::ranges::find_if(headers, [](jb::test::HttpTestHeader const& header) { return header.name == "x-Second"; });
    REQUIRE(first != headers.end());
    REQUIRE(second != headers.end());
    CHECK(first < second);
    CHECK(first->value == "one");
    CHECK(second->value == "two");
    CHECK_FALSE(header_value(headers, "Expect"));

    auto const& large = recorded[methods.size() + 4U];
    CHECK(large.body.size() == (std::size_t{1024} * 1024U) + 1U);
    CHECK_FALSE(header_value(large.headers, "Expect"));

    auto const& recorded_get_body = recorded[methods.size() + 5U];
    CHECK(recorded_get_body.method == "GET");
    CHECK(recorded_get_body.body == binary_body);
}

TEST_CASE("system HTTP client retains only the parsed and raw final response block", "[net][http]")
{
    auto app      = jb::core::Application{0, nullptr};
    auto server   = jb::test::HttpTestServer{};
    auto response = jb::test::HttpTestResponse{
        .informational =
            {
                            {
                    .status_code = 103,
                    .reason      = "Early Hints",
                    .headers     = {{.name = "Link", .value = "</style.css>; rel=preload"}},
                }, },
        .status_code = 201,
        .reason      = "Created",
        .headers =
            {
                            {.name = "X-Trim", .value = "\tvalue \t"},
                            {.name = "Set-Cookie", .value = "a=1"},
                            {.name = "Set-Cookie", .value = "b=2"},
                            },
        .body = bytes("response-data"),
    };
    server.enqueue_response(response);
    server.release_responses();

    auto created = SystemHttpClient::create(*app.event_loop());
    REQUIRE(created);
    auto client = std::move(created).value();

    auto const expected_raw       = std::string{"HTTP/1.1 201 Created\r\n"
                                                "X-Trim: \tvalue \t\r\n"
                                                "Set-Cookie: a=1\r\n"
                                                "Set-Cookie: b=2\r\n"
                                                "Content-Length: 13\r\n"
                                                "Connection: close\r\n\r\n"};
    auto       request            = minimal_request(server.url());
    request.response_body_limit   = 13U;
    request.response_header_limit = expected_raw.size();
    auto completed                = complete_request(app, *client, std::move(request));
    REQUIRE(completed);
    auto const& value = completed.value();
    CHECK(value.status_code == 201U);
    REQUIRE(value.headers.size() == 5U);
    CHECK(value.headers[0].name == "X-Trim");
    CHECK(value.headers[0].value == "value");
    CHECK(value.headers[1].name == "Set-Cookie");
    CHECK(value.headers[1].value == "a=1");
    CHECK(value.headers[2].name == "Set-Cookie");
    CHECK(value.headers[2].value == "b=2");
    CHECK(byte_text(value.body.bytes) == "response-data");
    CHECK(value.body.total_bytes == 13U);
    CHECK_FALSE(value.body.truncated);

    CHECK(byte_text(value.raw_headers.bytes) == expected_raw);
    CHECK(value.raw_headers.total_bytes == expected_raw.size());
    CHECK_FALSE(value.raw_headers.truncated);
    CHECK(value.redirect_count == 0U);
    CHECK(value.elapsed >= jb::core::Duration::zero());
    CHECK_FALSE(value.tls_verified);
}

TEST_CASE("system HTTP client captures first and last bytes while draining complete streams", "[net][http]")
{
    auto app    = jb::core::Application{0, nullptr};
    auto server = jb::test::HttpTestServer{};
    auto body   = std::string(std::size_t{128} * 1024U, 'm');
    body.replace(0U, 4U, "abcd");
    body.replace(body.size() - 3U, 3U, "nop");
    auto response = jb::test::HttpTestResponse{
        .headers = {{.name = "X-Test", .value = "value"}},
        .body    = bytes(body),
    };
    server.enqueue_response(response);
    server.release_responses();

    auto created = SystemHttpClient::create(*app.event_loop());
    REQUIRE(created);
    auto client = std::move(created).value();

    auto request                  = minimal_request(server.url());
    request.response_body_limit   = 7U;
    request.response_header_limit = 13U;
    auto completed                = complete_request(app, *client, std::move(request));
    REQUIRE(completed);

    auto const& value = completed.value();
    CHECK(byte_text(value.body.bytes) == "abcdnop");
    CHECK(value.body.total_bytes == body.size());
    CHECK(value.body.truncated);

    auto const full_headers     = std::string{"HTTP/1.1 200 OK\r\n"
                                              "X-Test: value\r\n"
                                              "Content-Length: 131072\r\n"
                                              "Connection: close\r\n\r\n"};
    auto const expected_headers = full_headers.substr(0U, 7U) + full_headers.substr(full_headers.size() - 6U);
    CHECK(byte_text(value.raw_headers.bytes) == expected_headers);
    CHECK(value.raw_headers.total_bytes == full_headers.size());
    CHECK(value.raw_headers.truncated);
}

TEST_CASE("system HTTP client dechunks and decompresses response bodies", "[net][http]")
{
    auto app    = jb::core::Application{0, nullptr};
    auto server = jb::test::HttpTestServer{};

    server.enqueue_response({
        .body       = bytes("decompressed response body"),
        .framing    = jb::test::HttpTestBodyFraming::Chunked,
        .chunk_size = 4U,
    });
    server.enqueue_response({
        .headers = {{.name = "Content-Encoding", .value = "gzip"}},
        .body = hex_bytes("1f8b08000000000002ff4b494dcecf2d284a2d2e4e4d51005205f979c5a90a49f9299500fa57a4f31a000000"),
    });
    server.enqueue_response({
        .headers = {{.name = "Content-Encoding", .value = "deflate"}},
        .body    = hex_bytes("789c4b494dcecf2d284a2d2e4e4d51005205f979c5a90a49f92995008d610a5c"),
    });
    server.release_responses();

    auto created = SystemHttpClient::create(*app.event_loop());
    REQUIRE(created);
    auto client = std::move(created).value();

    for (auto const& path : {"/chunked", "/gzip", "/deflate"}) {
        auto completed = complete_request(app, *client, minimal_request(server.url(path)));
        REQUIRE(completed);
        CHECK(byte_text(completed->body.bytes) == "decompressed response body");
        CHECK(completed->body.total_bytes == 26U);
        CHECK_FALSE(completed->body.truncated);
    }
}

TEST_CASE("system HTTP client enforces the independent parsed header limit safely", "[net][http]")
{
    auto app      = jb::core::Application{0, nullptr};
    auto server   = jb::test::HttpTestServer{};
    auto response = jb::test::HttpTestResponse{
        .headers = {{.name = "X-Secret", .value = "server-secret-sentinel-that-exceeds-the-hard-limit"}},
        .body    = bytes("ignored"),
    };
    server.enqueue_response(std::move(response));
    server.release_responses();

    auto options                                 = SystemHttpClientOptions{};
    options.maximum_parsed_response_header_bytes = 48U;
    auto created                                 = SystemHttpClient::create(*app.event_loop(), options);
    REQUIRE(created);
    auto client = std::move(created).value();

    auto completed = complete_request(app, *client, minimal_request(server.url()));
    REQUIRE_FALSE(completed);
    CHECK(completed.error().kind == jb::net::HttpErrorKind::Protocol);
    CHECK(completed.error().error.code == "net.http.protocol_error");
    CHECK(completed.error().error.detail == "response.headers_too_large");
    CHECK(completed.error().error.message.find("server-secret-sentinel") == std::string::npos);
    CHECK(completed.error().error.detail.find("server-secret-sentinel") == std::string::npos);
    CHECK(client->is_available());
    CHECK(client->active_request_count() == 0U);
}

TEST_CASE("system HTTP client reuses a keep-alive connection", "[net][http]")
{
    auto app    = jb::core::Application{0, nullptr};
    auto server = jb::test::HttpTestServer{};
    server.enqueue_response({.body = bytes("first"), .keep_alive = true});
    server.enqueue_response({.body = bytes("second")});
    server.release_responses();

    auto created = SystemHttpClient::create(*app.event_loop());
    REQUIRE(created);
    auto client = std::move(created).value();

    auto first = complete_request(app, *client, minimal_request(server.url("/first")));
    REQUIRE(first);
    CHECK(byte_text(first->body.bytes) == "first");
    auto second = complete_request(app, *client, minimal_request(server.url("/second")));
    REQUIRE(second);
    CHECK(byte_text(second->body.bytes) == "second");
    CHECK(server.accepted_connection_count() == 1U);
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

TEST_CASE("curl multi adapter defers and coalesces socket watch changes", "[net][http]")
{
    using jb::net::http::detail::CurlMultiAdapterTestAccess;
    using jb::net::http::detail::CurlMultiSocketInterest;

    auto fake         = jb::core::priv::make_fake_event_loop();
    auto current_loop = jb::core::priv::ScopedCurrentEventLoop{fake.loop.get()};
    REQUIRE(jb::net::http::detail::preflight_curl_runtime());

    auto completion_count = 0;
    auto failure_count    = 0;
    auto created          = jb::net::http::detail::CurlMultiAdapter::create(
        *fake.loop,
        [&](auto*, auto) -> void { ++completion_count; },
        [&](jb::core::Error const&) -> void { ++failure_count; });
    REQUIRE(created);
    auto adapter = std::move(created).value();

    auto const initial_add_calls    = fake.backend->add_fd_calls;
    auto const initial_remove_calls = fake.backend->remove_fd_calls;
    auto const initial_wakeup_calls = fake.backend->wakeup_calls;

    REQUIRE(CurlMultiAdapterTestAccess::record_socket_update(*adapter, 42, CurlMultiSocketInterest::Read));
    REQUIRE(CurlMultiAdapterTestAccess::schedule_reconcile(*adapter));
    REQUIRE(CurlMultiAdapterTestAccess::record_socket_update(*adapter, 42, CurlMultiSocketInterest::Write));
    REQUIRE(CurlMultiAdapterTestAccess::schedule_reconcile(*adapter));

    auto state = CurlMultiAdapterTestAccess::state(*adapter);
    CHECK(state.watch_count == 0U);
    CHECK(state.pending_watch_update_count == 1U);
    CHECK(state.reconcile_queued);
    CHECK(fake.backend->add_fd_calls == initial_add_calls);
    CHECK(fake.backend->wakeup_calls == initial_wakeup_calls + 1);

    REQUIRE(fake.loop->process_events(jb::core::EventFlag::Tasks, 0) != jb::core::ProcessEventsResult::Failed);
    state = CurlMultiAdapterTestAccess::state(*adapter);
    CHECK(state.watch_count == 1U);
    CHECK(state.pending_watch_update_count == 0U);
    CHECK_FALSE(state.reconcile_queued);
    CHECK(fake.backend->add_fd_calls == initial_add_calls + 1);
    CHECK(fake.backend->last_added_fd == 42);
    CHECK_FALSE(fake.backend->last_added_events.test(jb::core::FdEvent::Read));
    CHECK(fake.backend->last_added_events.test(jb::core::FdEvent::Write));
    auto superseded_callback = jb::core::priv::EventLoopTestAccess::fd_callback(*fake.loop, 42);
    REQUIRE(superseded_callback);

    REQUIRE(CurlMultiAdapterTestAccess::record_socket_update(*adapter, 42, CurlMultiSocketInterest::ReadWrite));
    REQUIRE(CurlMultiAdapterTestAccess::schedule_reconcile(*adapter));
    CHECK(fake.backend->add_fd_calls == initial_add_calls + 1);
    REQUIRE(fake.loop->process_events(jb::core::EventFlag::Tasks, 0) != jb::core::ProcessEventsResult::Failed);
    CHECK(fake.backend->add_fd_calls == initial_add_calls + 2);
    CHECK(fake.backend->remove_fd_calls == initial_remove_calls);
    CHECK(fake.backend->last_added_events.test(jb::core::FdEvent::Read));
    CHECK(fake.backend->last_added_events.test(jb::core::FdEvent::Write));

    auto const refresh_wakeup_calls = fake.backend->wakeup_calls;
    REQUIRE(CurlMultiAdapterTestAccess::refresh_socket_watch_after_readiness(*adapter, 42));
    REQUIRE(CurlMultiAdapterTestAccess::refresh_socket_watch_after_readiness(*adapter, 42));
    state = CurlMultiAdapterTestAccess::state(*adapter);
    CHECK(state.pending_watch_update_count == 1U);
    CHECK(state.reconcile_queued);
    CHECK(fake.backend->add_fd_calls == initial_add_calls + 2);
    CHECK(fake.backend->wakeup_calls == refresh_wakeup_calls + 1);
    REQUIRE(fake.loop->process_events(jb::core::EventFlag::Tasks, 0) != jb::core::ProcessEventsResult::Failed);
    state = CurlMultiAdapterTestAccess::state(*adapter);
    CHECK(state.pending_watch_update_count == 0U);
    CHECK_FALSE(state.reconcile_queued);
    CHECK(fake.backend->add_fd_calls == initial_add_calls + 3);
    CHECK(fake.backend->last_added_events.test(jb::core::FdEvent::Read));
    CHECK(fake.backend->last_added_events.test(jb::core::FdEvent::Write));

    jb::net::http::detail::fail_next_curl_multi_operation_for_testing(
        jb::net::http::detail::CurlMultiFailurePoint::SocketAction);
    superseded_callback(42, jb::core::FdEvent::Read);
    jb::net::http::detail::fail_next_curl_multi_operation_for_testing(
        jb::net::http::detail::CurlMultiFailurePoint::None);
    CHECK(adapter->is_available());
    CHECK(failure_count == 0);

    REQUIRE(CurlMultiAdapterTestAccess::record_socket_update(*adapter, 42, CurlMultiSocketInterest::Remove));
    REQUIRE(CurlMultiAdapterTestAccess::schedule_reconcile(*adapter));
    REQUIRE(fake.loop->process_events(jb::core::EventFlag::Tasks, 0) != jb::core::ProcessEventsResult::Failed);
    state = CurlMultiAdapterTestAccess::state(*adapter);
    CHECK(state.watch_count == 0U);
    CHECK(fake.backend->remove_fd_calls == initial_remove_calls + 1);
    CHECK(fake.backend->last_removed_fd == 42);
    CHECK(completion_count == 0);
    CHECK(failure_count == 0);
}

TEST_CASE("curl multi adapter cancels timers and unwatches final socket handles", "[net][http]")
{
    using jb::net::http::detail::CurlMultiAdapterTestAccess;
    using jb::net::http::detail::CurlMultiSocketInterest;

    auto fake         = jb::core::priv::make_fake_event_loop();
    auto current_loop = jb::core::priv::ScopedCurrentEventLoop{fake.loop.get()};
    REQUIRE(jb::net::http::detail::preflight_curl_runtime());

    auto completion_count = 0;
    auto failure_count    = 0;
    auto created          = jb::net::http::detail::CurlMultiAdapter::create(
        *fake.loop,
        [&](auto*, auto) -> void { ++completion_count; },
        [&](jb::core::Error const&) -> void { ++failure_count; });
    REQUIRE(created);
    auto adapter = std::move(created).value();

    CurlMultiAdapterTestAccess::record_timer_update(*adapter, 0L);
    REQUIRE(CurlMultiAdapterTestAccess::schedule_reconcile(*adapter));
    CHECK(CurlMultiAdapterTestAccess::state(*adapter).timer_update_pending);
    REQUIRE(fake.loop->process_events(jb::core::EventFlag::Tasks, 0) != jb::core::ProcessEventsResult::Failed);
    auto state = CurlMultiAdapterTestAccess::state(*adapter);
    CHECK(state.timer_armed);
    CHECK_FALSE(state.timer_update_pending);

    CurlMultiAdapterTestAccess::record_timer_update(*adapter, 60'000L);
    REQUIRE(CurlMultiAdapterTestAccess::schedule_reconcile(*adapter));
    state = CurlMultiAdapterTestAccess::state(*adapter);
    CHECK_FALSE(state.timer_armed);
    CHECK(state.timer_update_pending);
    REQUIRE(fake.loop->process_events(jb::core::EventFlag::Watchers, 37) != jb::core::ProcessEventsResult::Failed);
    CHECK(fake.backend->last_timeout_ms == 37);

    REQUIRE(fake.loop->process_events(jb::core::EventFlag::Tasks, 0) != jb::core::ProcessEventsResult::Failed);
    CHECK(CurlMultiAdapterTestAccess::state(*adapter).timer_armed);
    CurlMultiAdapterTestAccess::record_timer_update(*adapter, -1L);
    REQUIRE(CurlMultiAdapterTestAccess::schedule_reconcile(*adapter));
    REQUIRE(fake.loop->process_events(jb::core::EventFlag::Watchers, 37) != jb::core::ProcessEventsResult::Failed);
    CHECK(fake.backend->last_timeout_ms == 37);
    REQUIRE(fake.loop->process_events(jb::core::EventFlag::Tasks, 0) != jb::core::ProcessEventsResult::Failed);
    CHECK_FALSE(CurlMultiAdapterTestAccess::state(*adapter).timer_armed);

    CurlMultiAdapterTestAccess::record_timer_update(*adapter, 0L);
    REQUIRE(CurlMultiAdapterTestAccess::record_socket_update(*adapter, 43, CurlMultiSocketInterest::Read));
    REQUIRE(CurlMultiAdapterTestAccess::schedule_reconcile(*adapter));
    REQUIRE(fake.loop->process_events(jb::core::EventFlag::Tasks, 0) != jb::core::ProcessEventsResult::Failed);
    state = CurlMultiAdapterTestAccess::state(*adapter);
    REQUIRE(state.timer_armed);
    REQUIRE(state.watch_count == 1U);
    auto const remove_calls = fake.backend->remove_fd_calls;

    CurlMultiAdapterTestAccess::shutdown(*adapter);
    state = CurlMultiAdapterTestAccess::state(*adapter);
    CHECK_FALSE(state.timer_armed);
    CHECK(state.watch_count == 0U);
    CHECK(fake.backend->remove_fd_calls == remove_calls + 1);
    CHECK(fake.backend->last_removed_fd == 43);

    fake.backend->ready_events.push_back({.fd = 43, .events = jb::core::FdEvent::Read});
    REQUIRE(fake.loop->process_events(jb::core::EventFlag::Watchers, 37) != jb::core::ProcessEventsResult::Failed);
    CHECK(fake.backend->last_timeout_ms == 37);
    CHECK(completion_count == 0);
    CHECK(failure_count == 0);
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
