#include "http/system_http_client.hpp"

#include "application.hpp"
#include "event_loop_types.hpp"
#include "http/curl_error_priv.hpp"
#include "http/curl_multi_priv.hpp"
#include "http/curl_multi_test_priv.hpp"
#include "http/curl_request_priv.hpp"
#include "http/curl_runtime_priv.hpp"
#include "http/http_url_priv.hpp"
#include "support/fake_event_loop_backend.hpp"
#include "support/http_test_certificates.hpp"
#include "support/http_test_server.hpp"
#include "support/temporary_directory.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
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

constexpr std::size_t kSustainedTransferSize{(std::size_t{1024} * 1024U) + 1U};

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

auto server_origin(jb::test::HttpTestServer const& server) -> std::string
{
    auto result = server.url();
    if (result.ends_with('/')) {
        result.pop_back();
    }
    return result;
}

auto write_test_ca(jb::test::TemporaryDirectory const& directory, std::string_view filename) -> std::filesystem::path
{
    auto path   = directory.path() / filename;
    auto output = std::ofstream{path, std::ios::binary};
    if (!output.is_open()) {
        throw std::runtime_error{"could not create test CA bundle"};
    }
    output << jb::test::kHttpTestCaCertificate;
    output.close();
    return path;
}

class ScopedEnvironmentVariable final {
public:
    ScopedEnvironmentVariable(std::string name, std::optional<std::string> value)
        : _name{std::move(name)}
    {
        if (auto const* previous = std::getenv(_name.c_str())) {
            _previous = previous;
        }
        if (!set(std::move(value))) {
            throw std::runtime_error{"could not set test environment variable"};
        }
    }

    ~ScopedEnvironmentVariable() { static_cast<void>(set(std::move(_previous))); }

    ScopedEnvironmentVariable(ScopedEnvironmentVariable const&)                    = delete;
    ScopedEnvironmentVariable(ScopedEnvironmentVariable&&)                         = delete;
    auto operator=(ScopedEnvironmentVariable const&) -> ScopedEnvironmentVariable& = delete;
    auto operator=(ScopedEnvironmentVariable&&) -> ScopedEnvironmentVariable&      = delete;

private:
    [[nodiscard]] auto set(std::optional<std::string> value) noexcept -> bool
    {
        if (value) {
            return ::setenv(_name.c_str(), value->c_str(), 1) == 0;
        }
        return ::unsetenv(_name.c_str()) == 0;
    }

    std::string                _name;
    std::optional<std::string> _previous;
};

auto redirect_response(std::uint16_t status, std::string location, std::string_view body = {})
    -> jb::test::HttpTestResponse
{
    return {
        .status_code = status,
        .reason      = "Redirect",
        .headers     = {{.name = "Location", .value = std::move(location)}},
        .body        = bytes(body),
    };
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

void check_safe_project_error(jb::net::HttpError const& error, std::initializer_list<std::string_view> sentinels)
{
    CHECK_FALSE(error.error.code.empty());
    CHECK_FALSE(error.error.message.empty());
    for (auto sentinel : sentinels) {
        CHECK(error.error.code.find(sentinel) == std::string::npos);
        CHECK(error.error.message.find(sentinel) == std::string::npos);
        CHECK(error.error.detail.find(sentinel) == std::string::npos);
    }
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

TEST_CASE("curl timeout conversion preserves every positive remaining deadline", "[net][http]")
{
    using jb::net::http::detail::curl_timeout_milliseconds;

    auto sub_millisecond = curl_timeout_milliseconds(std::chrono::nanoseconds{1});
    REQUIRE(sub_millisecond);
    CHECK(*sub_millisecond == 1L);

    auto exact = curl_timeout_milliseconds(1ms);
    REQUIRE(exact);
    CHECK(*exact == 1L);

    auto rounded_up = curl_timeout_milliseconds(std::chrono::microseconds{1001});
    REQUIRE(rounded_up);
    CHECK(*rounded_up == 2L);

    auto expired = curl_timeout_milliseconds(jb::core::Duration::zero());
    REQUIRE(expired);
    CHECK(*expired == 1L);

    auto maximum = curl_timeout_milliseconds(std::chrono::days{30});
    REQUIRE(maximum);
    CHECK(*maximum == std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::days{30}).count());
}

TEST_CASE("curl request expires an accepted deadline before its deferred drive", "[net][http]")
{
    auto request    = minimal_request("http://127.0.0.1/deferred-deadline");
    request.timeout = 5ms;
    auto created    = jb::net::http::detail::CurlRequest::create(
        1U,
        std::move(request),
        [](jb::net::HttpRequestId, jb::net::HttpCompletionResult const&) -> void {},
        {},
        std::size_t{64} * 1024U);
    REQUIRE(created);
    auto state = std::move(created).value();

    auto const now         = jb::core::Clock::now();
    auto const accepted_at = now - 10ms;
    REQUIRE(state->prepare_admission(accepted_at));
    state->mark_accepted();

    CHECK(state->deadline() == accepted_at + 5ms);
    REQUIRE(state->deadline_expired(now));
    auto result = state->timeout_result();
    REQUIRE_FALSE(result);
    CHECK(result.error().kind == jb::net::HttpErrorKind::Timeout);
    CHECK(result.error().error.category == jb::core::ErrorCategory::Timeout);
    CHECK(result.error().error.code == "net.http.timeout");
    CHECK(result.error().elapsed >= 10ms);
}

TEST_CASE("curl errors map to stable safe request-local categories", "[net][http]")
{
    using jb::core::ErrorCategory;
    using jb::net::HttpErrorKind;
    using jb::net::http::detail::map_curl_error;

    struct Case {
        CURLcode         curl_code;
        HttpErrorKind    kind;
        ErrorCategory    category;
        std::string_view code;
    };

    auto const cases = std::array{
        Case{.curl_code = CURLE_COULDNT_RESOLVE_HOST,
             .kind      = HttpErrorKind::Resolve,
             .category  = ErrorCategory::Unavailable,
             .code      = "net.http.resolve_failed"         },
        Case{.curl_code = CURLE_COULDNT_CONNECT,
             .kind      = HttpErrorKind::Connect,
             .category  = ErrorCategory::Unavailable,
             .code      = "net.http.connect_failed"         },
        Case{.curl_code = CURLE_PEER_FAILED_VERIFICATION,
             .kind      = HttpErrorKind::TlsVerification,
             .category  = ErrorCategory::PermissionDenied,
             .code      = "net.http.tls_verification_failed"},
        Case{.curl_code = CURLE_SSL_CONNECT_ERROR,
             .kind      = HttpErrorKind::TlsHandshake,
             .category  = ErrorCategory::Io,
             .code      = "net.http.tls_handshake_failed"   },
        Case{.curl_code = CURLE_OPERATION_TIMEDOUT,
             .kind      = HttpErrorKind::Timeout,
             .category  = ErrorCategory::Timeout,
             .code      = "net.http.timeout"                },
        Case{.curl_code = CURLE_SEND_ERROR,
             .kind      = HttpErrorKind::Send,
             .category  = ErrorCategory::Io,
             .code      = "net.http.send_failed"            },
        Case{.curl_code = CURLE_RECV_ERROR,
             .kind      = HttpErrorKind::Receive,
             .category  = ErrorCategory::Io,
             .code      = "net.http.receive_failed"         },
        Case{.curl_code = CURLE_SSL_SHUTDOWN_FAILED,
             .kind      = HttpErrorKind::Receive,
             .category  = ErrorCategory::Io,
             .code      = "net.http.receive_failed"         },
        Case{.curl_code = CURLE_TOO_MANY_REDIRECTS,
             .kind      = HttpErrorKind::Redirect,
             .category  = ErrorCategory::Io,
             .code      = "net.http.redirect_failed"        },
        Case{.curl_code = CURLE_WEIRD_SERVER_REPLY,
             .kind      = HttpErrorKind::Protocol,
             .category  = ErrorCategory::Io,
             .code      = "net.http.protocol_error"         },
        Case{.curl_code = CURLE_OUT_OF_MEMORY,
             .kind      = HttpErrorKind::Internal,
             .category  = ErrorCategory::Internal,
             .code      = "net.http.internal"               },
    };

    for (auto const& test : cases) {
        auto mapped = map_curl_error(test.curl_code);
        CHECK(mapped.kind == test.kind);
        CHECK(mapped.error.category == test.category);
        CHECK(mapped.error.code == test.code);
        CHECK(mapped.error.detail.empty());
        check_safe_project_error(mapped, {"secret-url", "secret-header", "secret-body", "curl diagnostic"});
    }

    auto unmapped = map_curl_error(CURLE_FAILED_INIT);
    CHECK(unmapped.kind == HttpErrorKind::Internal);
    CHECK(unmapped.error.category == ErrorCategory::Internal);
    CHECK(unmapped.error.code == "net.http.internal");
    CHECK(unmapped.error.detail.empty());
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

    start = client->start(minimal_request("https://127.0.0.1:1/"), {});
    REQUIRE_FALSE(start);
    CHECK(start.error().detail == "completion.empty");

    auto cancellation = client->cancel(1U);
    REQUIRE_FALSE(cancellation);
    CHECK(cancellation.error().code == "net.http.request_not_found");
    CHECK_FALSE(callback_called);
}

TEST_CASE("system HTTP client shares one absolute deadline timer across accepted requests", "[net][http]")
{
    auto       app      = jb::core::Application{0, nullptr};
    auto const baseline = jb::core::priv::EventLoopTestAccess::active_timer_count(*app.event_loop());
    auto       created  = SystemHttpClient::create(*app.event_loop());
    REQUIRE(created);
    auto client = std::move(created).value();

    auto callback_count = 0;
    auto request_ids    = std::vector<jb::net::HttpRequestId>{};
    for (auto index = 0; index < 3; ++index) {
        auto request    = minimal_request("http://127.0.0.1:1/shared-deadline");
        request.timeout = 1h;
        auto started    = client->start(
            std::move(request),
            [&](jb::net::HttpRequestId, jb::net::HttpCompletionResult const&) -> void { ++callback_count; });
        REQUIRE(started);
        request_ids.push_back(*started);
        CHECK(jb::core::priv::EventLoopTestAccess::active_timer_count(*app.event_loop()) == baseline + 1U);
    }

    REQUIRE(client->cancel(request_ids[0]));
    CHECK(jb::core::priv::EventLoopTestAccess::active_timer_count(*app.event_loop()) == baseline + 1U);
    REQUIRE(client->cancel(request_ids[1]));
    CHECK(jb::core::priv::EventLoopTestAccess::active_timer_count(*app.event_loop()) == baseline + 1U);
    REQUIRE(client->cancel(request_ids[2]));
    CHECK(jb::core::priv::EventLoopTestAccess::active_timer_count(*app.event_loop()) == baseline);
    CHECK(callback_count == 0);

    client.reset();
    CHECK(jb::core::priv::EventLoopTestAccess::active_timer_count(*app.event_loop()) == baseline);
    drain_fake_loop(*app.event_loop());
    CHECK(callback_count == 0);
}

TEST_CASE("system HTTP client applies verified and unsafe origin TLS policy", "[net][http]")
{
    auto app       = jb::core::Application{0, nullptr};
    auto directory = jb::test::TemporaryDirectory{};
    auto ca_path   = write_test_ca(directory, "stage-5.7-secret-ca.pem");

    SECTION("default trust rejects the test CA")
    {
        auto server  = jb::test::HttpTestServer{jb::test::HttpTestTransport::Tls};
        auto created = SystemHttpClient::create(*app.event_loop());
        REQUIRE(created);
        auto completed = complete_request(app, **created, minimal_request(server.url("/untrusted")));
        REQUIRE_FALSE(completed);
        CHECK(completed.error().kind == jb::net::HttpErrorKind::TlsVerification);
        CHECK_FALSE(completed.error().tls_verified);
        check_safe_project_error(completed.error(), {"stage-5.7-secret-ca", "untrusted"});
    }

    SECTION("explicit CA verifies trust and identity")
    {
        auto server = jb::test::HttpTestServer{jb::test::HttpTestTransport::Tls};
        server.enqueue_response({.body = bytes("verified")});
        server.release_responses();
        auto created = SystemHttpClient::create(*app.event_loop(), {.ca_bundle = ca_path});
        REQUIRE(created);
        auto completed = complete_request(app, **created, minimal_request(server.url("/verified")));
        REQUIRE(completed);
        CHECK(byte_text(completed->body.bytes) == "verified");
        REQUIRE(completed->tls_verified);
        CHECK(*completed->tls_verified);
    }

    SECTION("trusted certificate with the wrong identity is rejected")
    {
        auto server  = jb::test::HttpTestServer{jb::test::HttpTestTransport::TlsMismatchedIdentity};
        auto created = SystemHttpClient::create(*app.event_loop(), {.ca_bundle = ca_path});
        REQUIRE(created);
        auto completed = complete_request(app, **created, minimal_request(server.url("/wrong-identity")));
        REQUIRE_FALSE(completed);
        CHECK(completed.error().kind == jb::net::HttpErrorKind::TlsVerification);
        CHECK_FALSE(completed.error().tls_verified);
        check_safe_project_error(completed.error(), {"stage-5.7-secret-ca", "wrong-identity"});
    }

    SECTION("unsafe origin mode disables both checks and records false")
    {
        auto server = jb::test::HttpTestServer{jb::test::HttpTestTransport::TlsMismatchedIdentity};
        server.enqueue_response({.body = bytes("unsafe")});
        server.release_responses();
        auto created = SystemHttpClient::create(*app.event_loop());
        REQUIRE(created);
        auto request       = minimal_request(server.url("/unsafe"));
        request.verify_tls = false;
        auto completed     = complete_request(app, **created, std::move(request));
        REQUIRE(completed);
        CHECK(byte_text(completed->body.bytes) == "unsafe");
        REQUIRE(completed->tls_verified);
        CHECK_FALSE(*completed->tls_verified);
    }
}

TEST_CASE("system HTTP client keeps CA transfer failures safe", "[net][http]")
{
    auto app       = jb::core::Application{0, nullptr};
    auto directory = jb::test::TemporaryDirectory{};
    auto ca_path   = directory.path() / "stage-5.7-invalid-ca-secret.pem";
    auto output    = std::ofstream{ca_path, std::ios::binary};
    REQUIRE(output.is_open());
    output << "invalid CA contents";
    output.close();

    auto server  = jb::test::HttpTestServer{jb::test::HttpTestTransport::Tls};
    auto created = SystemHttpClient::create(*app.event_loop(), {.ca_bundle = ca_path});
    REQUIRE(created);
    auto completed = complete_request(app, **created, minimal_request(server.url("/ca-secret-url")));
    REQUIRE_FALSE(completed);
    CHECK(completed.error().kind == jb::net::HttpErrorKind::TlsHandshake);
    CHECK_FALSE(completed.error().tls_verified);
    check_safe_project_error(completed.error(), {"stage-5.7-invalid-ca-secret", "ca-secret-url"});
}

TEST_CASE("system HTTP client ignores ambient proxies and enforces explicit proxy routing", "[net][http]")
{
    auto app = jb::core::Application{0, nullptr};

    SECTION("absent option ignores every ambient proxy")
    {
        auto origin        = jb::test::HttpTestServer{};
        auto hostile_proxy = jb::test::HttpTestServer{};
        origin.enqueue_response({.body = bytes("direct")});
        origin.release_responses();
        hostile_proxy.enqueue_response({.body = bytes("ambient-proxy")});
        hostile_proxy.release_responses();

        auto const proxy       = server_origin(hostile_proxy);
        auto       http_proxy  = ScopedEnvironmentVariable{"http_proxy", proxy};
        auto       https_proxy = ScopedEnvironmentVariable{"https_proxy", proxy};
        auto       all_proxy   = ScopedEnvironmentVariable{"all_proxy", proxy};
        auto       upper_http  = ScopedEnvironmentVariable{"HTTP_PROXY", proxy};
        auto       upper_https = ScopedEnvironmentVariable{"HTTPS_PROXY", proxy};
        auto       upper_all   = ScopedEnvironmentVariable{"ALL_PROXY", proxy};
        auto       no_proxy    = ScopedEnvironmentVariable{"no_proxy", std::string{}};
        auto       upper_no    = ScopedEnvironmentVariable{"NO_PROXY", std::string{}};

        auto created = SystemHttpClient::create(*app.event_loop());
        REQUIRE(created);
        auto completed = complete_request(app, **created, minimal_request(origin.url("/direct")));
        REQUIRE(completed);
        CHECK(byte_text(completed->body.bytes) == "direct");
        CHECK(origin.accepted_connection_count() == 1U);
        CHECK(hostile_proxy.accepted_connection_count() == 0U);
    }

    SECTION("explicit HTTP proxy overrides ambient no-proxy")
    {
        auto proxy = jb::test::HttpTestServer{};
        proxy.enqueue_response({.body = bytes("http-proxy")});
        proxy.release_responses();
        auto no_proxy = ScopedEnvironmentVariable{"no_proxy", std::string{"*"}};
        auto upper_no = ScopedEnvironmentVariable{"NO_PROXY", std::string{"*"}};

        auto created = SystemHttpClient::create(*app.event_loop(), {.proxy = server_origin(proxy)});
        REQUIRE(created);
        auto const target    = std::string{"http://stage57-target.example.test/proxied?value=1"};
        auto       completed = complete_request(app, **created, minimal_request(target));
        REQUIRE(completed);
        CHECK(byte_text(completed->body.bytes) == "http-proxy");
        REQUIRE(proxy.wait_for_requests(1U, 0ms));
        CHECK(proxy.requests().front().target == target);
        CHECK(proxy.accepted_connection_count() == 1U);
        CHECK_FALSE(completed->tls_verified);
    }

    SECTION("explicit HTTPS proxy keeps proxy verification enabled")
    {
        auto directory = jb::test::TemporaryDirectory{};
        auto ca_path   = write_test_ca(directory, "stage-5.7-proxy-ca.pem");
        auto proxy     = jb::test::HttpTestServer{jb::test::HttpTestTransport::Tls};
        proxy.enqueue_response({.body = bytes("https-proxy")});
        proxy.release_responses();

        auto created =
            SystemHttpClient::create(*app.event_loop(), {.ca_bundle = ca_path, .proxy = server_origin(proxy)});
        REQUIRE(created);
        auto const target    = std::string{"http://stage57-secure-proxy-target.example.test/resource"};
        auto       completed = complete_request(app, **created, minimal_request(target));
        REQUIRE(completed);
        CHECK(byte_text(completed->body.bytes) == "https-proxy");
        REQUIRE(proxy.wait_for_requests(1U, 0ms));
        CHECK(proxy.requests().front().target == target);
        CHECK_FALSE(completed->tls_verified);

        auto unsafe_created = SystemHttpClient::create(*app.event_loop(), {.proxy = server_origin(proxy)});
        REQUIRE(unsafe_created);
        auto unsafe_request       = minimal_request(target);
        unsafe_request.verify_tls = false;
        auto unsafe_completed     = complete_request(app, **unsafe_created, std::move(unsafe_request));
        REQUIRE_FALSE(unsafe_completed);
        CHECK(unsafe_completed.error().kind == jb::net::HttpErrorKind::TlsVerification);
        CHECK_FALSE(unsafe_completed.error().tls_verified);
    }

    SECTION("proxy connection errors omit proxy and request values")
    {
        auto const proxy   = std::string{"http://127.0.0.1:1/stage-5.7-proxy-secret"};
        auto       created = SystemHttpClient::create(*app.event_loop(), {.proxy = proxy});
        REQUIRE(created);
        auto completed =
            complete_request(app, **created, minimal_request("http://stage57-request-secret.example.test/resource"));
        REQUIRE_FALSE(completed);
        CHECK(completed.error().kind == jb::net::HttpErrorKind::Connect);
        check_safe_project_error(completed.error(), {"stage-5.7-proxy-secret", "stage57-request-secret"});
    }
}

TEST_CASE("system HTTP client reapplies TLS policy across a secure redirect", "[net][http]")
{
    auto app       = jb::core::Application{0, nullptr};
    auto directory = jb::test::TemporaryDirectory{};
    auto ca_path   = write_test_ca(directory, "stage-5.7-redirect-ca.pem");
    auto first     = jb::test::HttpTestServer{};
    auto final     = jb::test::HttpTestServer{jb::test::HttpTestTransport::Tls};
    first.enqueue_response(redirect_response(302U, final.url("/secure-final")));
    final.enqueue_response({.body = bytes("secure-final")});
    first.release_responses();
    final.release_responses();

    auto created = SystemHttpClient::create(*app.event_loop(), {.ca_bundle = ca_path});
    REQUIRE(created);
    auto request    = minimal_request(first.url("/start"));
    request.headers = {
        {.name = "Authorization", .value = "stage-5.7-credential"},
        {.name = "X-Sensitive", .value = "stage-5.7-sensitive", .sensitive = true},
        {.name = "X-Retained", .value = "retained"},
    };
    request.follow_redirects = true;
    auto completed           = complete_request(app, **created, std::move(request));
    REQUIRE(completed);
    CHECK(byte_text(completed->body.bytes) == "secure-final");
    CHECK(completed->redirect_count == 1U);
    REQUIRE(completed->tls_verified);
    CHECK(*completed->tls_verified);

    REQUIRE(final.wait_for_requests(1U, 0ms));
    auto const  final_requests = final.requests();
    auto const& final_headers  = final_requests.front().headers;
    CHECK_FALSE(header_value(final_headers, "Authorization"));
    CHECK_FALSE(header_value(final_headers, "X-Sensitive"));
    CHECK(header_value(final_headers, "X-Retained") == "retained");
}

TEST_CASE("system HTTP client rejects a verified HTTPS downgrade before the next connection", "[net][http]")
{
    auto app       = jb::core::Application{0, nullptr};
    auto directory = jb::test::TemporaryDirectory{};
    auto ca_path   = write_test_ca(directory, "stage-5.7-downgrade-ca.pem");
    auto secure    = jb::test::HttpTestServer{jb::test::HttpTestTransport::Tls};
    auto plain     = jb::test::HttpTestServer{};
    secure.enqueue_response(redirect_response(302U, plain.url("/forbidden")));
    secure.release_responses();

    auto created = SystemHttpClient::create(*app.event_loop(), {.ca_bundle = ca_path});
    REQUIRE(created);
    auto request             = minimal_request(secure.url("/start"));
    request.follow_redirects = true;
    auto completed           = complete_request(app, **created, std::move(request));
    REQUIRE_FALSE(completed);
    CHECK(completed.error().kind == jb::net::HttpErrorKind::Redirect);
    CHECK(completed.error().error.detail == "redirect.https_downgrade");
    CHECK(completed.error().status_code == 302U);
    CHECK(completed.error().redirect_count == 0U);
    CHECK_FALSE(completed.error().tls_verified);
    CHECK(plain.accepted_connection_count() == 0U);
}

TEST_CASE("curl redirect URL policy resolves targets and compares canonical origins", "[net][http]")
{
    using jb::net::http::detail::resolve_redirect_target;

    auto relative = resolve_redirect_target("http://Example.TEST:80/a/b", "../next?value=1");
    REQUIRE(relative);
    CHECK_FALSE(relative->cross_origin);
    CHECK_FALSE(relative->uses_tls);
    CHECK(relative->url.ends_with("/next?value=1"));

    auto implicit_port = resolve_redirect_target("http://example.test:80/a", "http://EXAMPLE.test/b");
    REQUIRE(implicit_port);
    CHECK_FALSE(implicit_port->cross_origin);

    auto other_port = resolve_redirect_target("http://example.test/a", "http://example.test:81/b");
    REQUIRE(other_port);
    CHECK(other_port->cross_origin);

    auto same_zone = resolve_redirect_target("http://[fe80::1%25eth0]/a", "http://[fe80::1%25eth0]/b");
    REQUIRE(same_zone);
    CHECK_FALSE(same_zone->cross_origin);

    auto other_zone = resolve_redirect_target("http://[fe80::1%25eth0]/a", "http://[fe80::1%25eth1]/b");
    REQUIRE(other_zone);
    CHECK(other_zone->cross_origin);

    auto removed_zone = resolve_redirect_target("http://[fe80::1%25eth0]/a", "http://[fe80::1]/b");
    REQUIRE(removed_zone);
    CHECK(removed_zone->cross_origin);

    auto upgrade = resolve_redirect_target("http://example.test/a", "https://example.test/b");
    REQUIRE(upgrade);
    CHECK(upgrade->cross_origin);
    CHECK(upgrade->uses_tls);

    auto downgrade = resolve_redirect_target("https://example.test/a", "http://example.test/b");
    REQUIRE_FALSE(downgrade);
    CHECK(downgrade.error().code == "net.http.redirect_failed");
    CHECK(downgrade.error().detail == "redirect.https_downgrade");

    auto const invalid_targets = std::array{
        std::string_view{""},
        std::string_view{"ftp://redirect-secret.example/path"},
        std::string_view{"http://redirect-secret@example.test/path"},
        std::string_view{"/next#redirect-secret-fragment"},
        std::string_view{"/%0G-redirect-secret-escape"},
    };
    for (auto target : invalid_targets) {
        auto resolved = resolve_redirect_target("http://example.test/start", target);
        REQUIRE_FALSE(resolved);
        CHECK(resolved.error().code == "net.http.redirect_failed");
        CHECK(resolved.error().message.find("redirect-secret") == std::string::npos);
        CHECK(resolved.error().detail.find("redirect-secret") == std::string::npos);
    }
}

TEST_CASE("system HTTP client returns redirect responses when following is disabled", "[net][http]")
{
    auto app     = jb::core::Application{0, nullptr};
    auto server  = jb::test::HttpTestServer{};
    auto created = SystemHttpClient::create(*app.event_loop());
    REQUIRE(created);
    auto client = std::move(created).value();

    server.enqueue_response(redirect_response(302U, "/not-followed", "intermediate-body"));
    server.release_responses();

    auto completed = complete_request(app, *client, minimal_request(server.url("/start")));
    REQUIRE(completed);
    CHECK(completed->status_code == 302U);
    CHECK(completed->redirect_count == 0U);
    CHECK(byte_text(completed->body.bytes) == "intermediate-body");
    REQUIRE(server.wait_for_requests(1U, 0ms));
    CHECK(server.requests().size() == 1U);
}

TEST_CASE("system HTTP client applies every manual redirect method transition", "[net][http]")
{
    struct Case {
        std::uint16_t                   status;
        std::string_view                method;
        std::optional<std::string_view> body;
        std::string_view                expected_method;
        std::string_view                expected_body;
        std::optional<std::string_view> expected_content_length;
    };

    auto const cases = std::array{
        Case{.status = 301U, .method = "POST", .body = "post-301", .expected_method = "GET", .expected_body = ""},
        Case{.status = 302U, .method = "POST", .body = "post-302", .expected_method = "GET", .expected_body = ""},
        Case{.status                  = 301U,
             .method                  = "PUT",
             .body                    = "put-301",
             .expected_method         = "PUT",
             .expected_body           = "put-301",
             .expected_content_length = "7"},
        Case{.status = 303U, .method = "POST", .body = "post-303", .expected_method = "GET", .expected_body = ""},
        Case{.status = 303U, .method = "HEAD", .body = std::nullopt, .expected_method = "HEAD", .expected_body = ""},
        Case{.status                  = 307U,
             .method                  = "POST",
             .body                    = "post-307",
             .expected_method         = "POST",
             .expected_body           = "post-307",
             .expected_content_length = "8"},
        Case{.status                  = 308U,
             .method                  = "PATCH",
             .body                    = "patch-308",
             .expected_method         = "PATCH",
             .expected_body           = "patch-308",
             .expected_content_length = "9"},
        Case{.status                  = 307U,
             .method                  = "PUT",
             .body                    = "",
             .expected_method         = "PUT",
             .expected_body           = "",
             .expected_content_length = "0"},
        Case{.status          = 308U,
             .method          = "DELETE",
             .body            = std::nullopt,
             .expected_method = "DELETE",
             .expected_body   = ""},
    };

    auto app     = jb::core::Application{0, nullptr};
    auto server  = jb::test::HttpTestServer{};
    auto created = SystemHttpClient::create(*app.event_loop());
    REQUIRE(created);
    auto client = std::move(created).value();

    for (std::size_t index = 0; index < cases.size(); ++index) {
        auto const& test       = cases[index];
        auto const  target     = "/redirect-target-" + std::to_string(index);
        auto const  final_body = "final-body-" + std::to_string(index);
        auto        final      = jb::test::HttpTestResponse{};
        final.headers.push_back({.name = "X-Final-Leg", .value = std::to_string(index)});
        final.body = bytes(final_body);
        server.enqueue_response(redirect_response(test.status, target, "discarded-intermediate-body"));
        server.enqueue_response(std::move(final));
    }
    server.release_responses();

    for (std::size_t index = 0; index < cases.size(); ++index) {
        auto const& test    = cases[index];
        auto        request = minimal_request(server.url("/redirect-start-" + std::to_string(index)));
        request.method      = test.method;
        if (test.body) {
            request.body = bytes(*test.body);
        }
        request.headers = {
            {.name = "X-Marked-Same-Origin", .value = "retained", .sensitive = true}
        };
        request.follow_redirects = true;

        auto completed = complete_request(app, *client, std::move(request));
        REQUIRE(completed);
        CHECK(completed->status_code == 200U);
        CHECK(completed->redirect_count == 1U);
        auto const expected_final_body =
            test.expected_method == "HEAD" ? std::string{} : "final-body-" + std::to_string(index);
        CHECK(byte_text(completed->body.bytes) == expected_final_body);
        CHECK(byte_text(completed->raw_headers.bytes).find("Location:") == std::string::npos);
    }

    auto const requests = server.requests();
    REQUIRE(requests.size() == cases.size() * 2U);
    for (std::size_t index = 0; index < cases.size(); ++index) {
        auto const& followed = requests[(index * 2U) + 1U];
        CHECK(followed.method == cases[index].expected_method);
        CHECK(byte_text(followed.body) == cases[index].expected_body);
        CHECK(followed.target == "/redirect-target-" + std::to_string(index));
        CHECK(header_value(followed.headers, "X-Marked-Same-Origin") == "retained");
        CHECK(header_value(followed.headers, "Content-Length") == cases[index].expected_content_length);
    }
}

TEST_CASE("system HTTP client strips credentials permanently across redirect origins", "[net][http]")
{
    auto app          = jb::core::Application{0, nullptr};
    auto first_origin = jb::test::HttpTestServer{};
    auto next_origin  = jb::test::HttpTestServer{};
    auto created      = SystemHttpClient::create(*app.event_loop());
    REQUIRE(created);
    auto client = std::move(created).value();

    first_origin.enqueue_response(
        redirect_response(302U, next_origin.url("/cross-origin"), "first-intermediate-secret"));
    auto final = jb::test::HttpTestResponse{};
    final.headers.push_back({.name = "X-Final-Leg", .value = "final-only"});
    final.body = bytes("final-only-body");
    first_origin.enqueue_response(std::move(final));
    next_origin.enqueue_response(
        redirect_response(307U, first_origin.url("/return-origin"), "second-intermediate-secret"));
    first_origin.release_responses();
    next_origin.release_responses();

    auto request    = minimal_request(first_origin.url("/initial"));
    request.headers = {
        {.name = "Authorization", .value = "authorization-secret"},
        {.name = "cOoKiE", .value = "cookie-secret"},
        {.name = "Proxy-Authorization", .value = "proxy-secret"},
        {.name = "X-Marked", .value = "marked-secret", .sensitive = true},
        {.name = "X-Ordinary", .value = "ordinary-value"},
        {.name = "X-JobU-Run-ID", .value = "run-metadata"},
    };
    request.follow_redirects = true;

    auto completed = complete_request(app, *client, std::move(request));
    REQUIRE(completed);
    CHECK(completed->redirect_count == 2U);
    CHECK(byte_text(completed->body.bytes) == "final-only-body");
    CHECK(byte_text(completed->raw_headers.bytes).find("X-Final-Leg: final-only") != std::string::npos);
    CHECK(byte_text(completed->raw_headers.bytes).find("intermediate-secret") == std::string::npos);

    auto const first_requests = first_origin.requests();
    auto const next_requests  = next_origin.requests();
    REQUIRE(first_requests.size() == 2U);
    REQUIRE(next_requests.size() == 1U);

    CHECK(header_value(first_requests[0].headers, "Authorization") == "authorization-secret");
    CHECK(header_value(first_requests[0].headers, "Cookie") == "cookie-secret");
    CHECK(header_value(first_requests[0].headers, "Proxy-Authorization") == "proxy-secret");
    CHECK(header_value(first_requests[0].headers, "X-Marked") == "marked-secret");

    for (auto const* followed : {next_requests.data(), first_requests.data() + 1U}) {
        CHECK_FALSE(header_value(followed->headers, "Authorization"));
        CHECK_FALSE(header_value(followed->headers, "Cookie"));
        CHECK_FALSE(header_value(followed->headers, "Proxy-Authorization"));
        CHECK_FALSE(header_value(followed->headers, "X-Marked"));
        CHECK(header_value(followed->headers, "X-Ordinary") == "ordinary-value");
        CHECK(header_value(followed->headers, "X-JobU-Run-ID") == "run-metadata");
    }
}

TEST_CASE("system HTTP client returns safe redirect policy failures", "[net][http]")
{
    struct Case {
        std::string_view                      name;
        std::vector<jb::test::HttpTestHeader> headers;
        std::string_view                      reason;
    };

    auto const cases = std::array{
        Case{.name = "missing Location",     .headers = {},                                       .reason = "redirect.missing_location"},
        Case{.name    = "multiple Location",
             .headers = {{.name = "Location", .value = "/first-secret"},
                         {.name = "lOcAtIoN", .value = "/second-secret"}},
             .reason  = "redirect.invalid_location"                                                                                    },
        Case{.name    = "non-HTTP target",
             .headers = {{.name = "Location", .value = "ftp://redirect-secret.example/path"}},
             .reason  = "redirect.invalid_target"                                                                                      },
        Case{.name    = "fragment target",
             .headers = {{.name = "Location", .value = "/next#redirect-secret-fragment"}},
             .reason  = "redirect.invalid_target"                                                                                      },
        Case{.name    = "userinfo target",
             .headers = {{.name = "Location", .value = "http://redirect-secret@127.0.0.1/path"}},
             .reason  = "redirect.invalid_target"                                                                                      },
    };

    for (auto const& test : cases) {
        DYNAMIC_SECTION(test.name)
        {
            auto app     = jb::core::Application{0, nullptr};
            auto server  = jb::test::HttpTestServer{};
            auto created = SystemHttpClient::create(*app.event_loop());
            REQUIRE(created);
            auto client = std::move(created).value();

            auto response        = jb::test::HttpTestResponse{};
            response.status_code = 302U;
            response.reason      = "Redirect";
            response.headers     = test.headers;
            response.body        = bytes("redirect-secret-body");
            server.enqueue_response(std::move(response));
            server.release_responses();

            auto callback_count = 0;
            auto completion     = std::optional<jb::net::HttpCompletionResult>{};
            auto request        = minimal_request(server.url("/redirect-secret-url"));
            request.headers     = {
                {.name = "X-Secret", .value = "redirect-secret-header"}
            };
            request.follow_redirects = true;
            auto started = client->start(std::move(request),
                                         [&](jb::net::HttpRequestId, jb::net::HttpCompletionResult result) -> void {
                                             ++callback_count;
                                             completion = std::move(result);
                                         });
            REQUIRE(started);
            REQUIRE(process_until(app, [&completion]() -> bool { return completion.has_value(); }));
            REQUIRE_FALSE(*completion);
            CHECK(completion->error().kind == jb::net::HttpErrorKind::Redirect);
            CHECK(completion->error().error.code == "net.http.redirect_failed");
            CHECK(completion->error().error.detail == test.reason);
            CHECK(completion->error().status_code == 302U);
            CHECK(completion->error().redirect_count == 0U);
            check_safe_project_error(completion->error(),
                                     {"redirect-secret-url",
                                      "redirect-secret-header",
                                      "redirect-secret-body",
                                      "first-secret",
                                      "second-secret",
                                      "redirect-secret.example"});
            CHECK(callback_count == 1);
            drain_fake_loop(*app.event_loop());
            CHECK(callback_count == 1);
        }
    }
}

TEST_CASE("system HTTP client terminates redirect loops at the request-wide limit", "[net][http]")
{
    auto app     = jb::core::Application{0, nullptr};
    auto server  = jb::test::HttpTestServer{};
    auto created = SystemHttpClient::create(*app.event_loop());
    REQUIRE(created);
    auto client = std::move(created).value();

    server.enqueue_response(redirect_response(302U, "/loop", "first-loop-body"));
    server.enqueue_response(redirect_response(302U, "/loop", "second-loop-body"));
    server.release_responses();

    auto request             = minimal_request(server.url("/loop"));
    request.follow_redirects = true;
    request.max_redirects    = 1U;
    auto completed           = complete_request(app, *client, std::move(request));
    REQUIRE_FALSE(completed);
    CHECK(completed.error().kind == jb::net::HttpErrorKind::Redirect);
    CHECK(completed.error().error.detail == "redirect.limit_exceeded");
    CHECK(completed.error().status_code == 302U);
    CHECK(completed.error().redirect_count == 1U);
    REQUIRE(server.wait_for_requests(2U, 0ms));
    CHECK(server.requests().size() == 2U);
}

TEST_CASE("system HTTP redirect chains keep one deadline and asynchronous cancellation", "[net][http]")
{
    SECTION("whole-request timeout")
    {
        auto app       = jb::core::Application{0, nullptr};
        auto directory = jb::test::TemporaryDirectory{};
        auto ca_path   = write_test_ca(directory, "stage-5.7-deadline-ca.pem");
        auto first     = jb::test::HttpTestServer{};
        auto final     = jb::test::HttpTestServer{jb::test::HttpTestTransport::Tls};
        auto created   = SystemHttpClient::create(*app.event_loop(), {.ca_bundle = ca_path});
        REQUIRE(created);
        auto client = std::move(created).value();

        first.enqueue_response(redirect_response(302U, final.url("/held-timeout")));
        auto held    = jb::test::HttpTestResponse{};
        held.headers = {
            {.name = "X-Observed", .value = "second-leg"}
        };
        held.body                       = bytes("held-body");
        held.pause_after_response_bytes = 0U;
        final.enqueue_response(std::move(held));
        first.release_responses();
        final.release_responses();

        auto callback_count      = 0;
        auto completion          = std::optional<jb::net::HttpCompletionResult>{};
        auto request             = minimal_request(first.url("/start-timeout"));
        request.follow_redirects = true;
        request.timeout          = 100ms;
        auto started = client->start(std::move(request),
                                     [&](jb::net::HttpRequestId, jb::net::HttpCompletionResult result) -> void {
                                         ++callback_count;
                                         completion = std::move(result);
                                     });
        REQUIRE(started);
        REQUIRE(process_until(app, [&final]() -> bool { return final.wait_for_response_segments(1U, 0ms); }));
        REQUIRE(process_until(app, [&completion]() -> bool { return completion.has_value(); }));
        REQUIRE_FALSE(*completion);
        CHECK(completion->error().kind == jb::net::HttpErrorKind::Timeout);
        CHECK(completion->error().redirect_count == 1U);
        CHECK(completion->error().status_code == 200U);
        CHECK_FALSE(completion->error().tls_verified);
        CHECK(callback_count == 1);
        final.release_response_segment();
        drain_fake_loop(*app.event_loop());
        CHECK(callback_count == 1);
    }

    SECTION("cancellation during the final leg")
    {
        auto app     = jb::core::Application{0, nullptr};
        auto server  = jb::test::HttpTestServer{};
        auto created = SystemHttpClient::create(*app.event_loop());
        REQUIRE(created);
        auto client = std::move(created).value();

        server.enqueue_response(redirect_response(307U, "/held-cancellation"));
        auto held    = jb::test::HttpTestResponse{};
        held.headers = {
            {.name = "X-Observed", .value = "second-leg"}
        };
        held.body                       = bytes("held-body");
        held.pause_after_response_bytes = 0U;
        server.enqueue_response(std::move(held));
        server.release_responses();

        auto callback_count      = 0;
        auto completion          = std::optional<jb::net::HttpCompletionResult>{};
        auto request             = minimal_request(server.url("/start-cancellation"));
        request.follow_redirects = true;
        auto started = client->start(std::move(request),
                                     [&](jb::net::HttpRequestId, jb::net::HttpCompletionResult result) -> void {
                                         ++callback_count;
                                         completion = std::move(result);
                                     });
        REQUIRE(started);
        REQUIRE(process_until(app, [&server]() -> bool { return server.wait_for_response_segments(1U, 0ms); }));
        REQUIRE(client->cancel(*started));
        CHECK_FALSE(completion);
        REQUIRE(process_until(app, [&completion]() -> bool { return completion.has_value(); }));
        REQUIRE_FALSE(*completion);
        CHECK(completion->error().kind == jb::net::HttpErrorKind::Cancelled);
        CHECK(completion->error().redirect_count == 1U);
        CHECK(callback_count == 1);
        server.release_response_segment();
        drain_fake_loop(*app.event_loop());
        CHECK(callback_count == 1);
    }
}

TEST_CASE("system HTTP redirect chains retain the hop count on final-leg transport failure", "[net][http]")
{
    auto app     = jb::core::Application{0, nullptr};
    auto server  = jb::test::HttpTestServer{};
    auto created = SystemHttpClient::create(*app.event_loop());
    REQUIRE(created);
    auto client = std::move(created).value();

    server.enqueue_response(redirect_response(308U, "/truncated-final"));
    auto truncated    = jb::test::HttpTestResponse{};
    truncated.headers = {
        {.name = "X-Observed", .value = "final-leg"}
    };
    truncated.body                       = bytes("partial-response");
    truncated.close_after_response_bytes = 3U;
    server.enqueue_response(std::move(truncated));
    server.release_responses();

    auto request             = minimal_request(server.url("/start"));
    request.follow_redirects = true;
    auto completed           = complete_request(app, *client, std::move(request));
    REQUIRE_FALSE(completed);
    CHECK(completed.error().kind == jb::net::HttpErrorKind::Receive);
    CHECK(completed.error().error.code == "net.http.receive_failed");
    CHECK(completed.error().redirect_count == 1U);
    CHECK(completed.error().status_code == 200U);
    CHECK(byte_text(completed.error().body.bytes) == "par");
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
    large_request.body   = jb::core::ByteBuffer(kSustainedTransferSize, std::byte{0x5a});
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
    CHECK(large.body.size() == kSustainedTransferSize);
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

TEST_CASE("system HTTP client drains a sustained response while capturing first and last bytes", "[net][http]")
{
    auto app    = jb::core::Application{0, nullptr};
    auto server = jb::test::HttpTestServer{};
    auto body   = std::string(kSustainedTransferSize, 'm');
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
                                              "Content-Length: "} +
                                  std::to_string(kSustainedTransferSize) + "\r\nConnection: close\r\n\r\n";
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

TEST_CASE("system HTTP client applies one whole-request timeout while the peer is held", "[net][http]")
{
    auto app     = jb::core::Application{0, nullptr};
    auto server  = jb::test::HttpTestServer{};
    auto created = SystemHttpClient::create(*app.event_loop());
    REQUIRE(created);
    auto client = std::move(created).value();

    auto callback_count = 0;
    auto completion     = std::optional<jb::net::HttpCompletionResult>{};
    auto request        = minimal_request(server.url("/secret-timeout-url"));
    request.headers     = {
        {.name = "X-Secret", .value = "secret-timeout-header"}
    };
    request.body    = bytes("secret-timeout-body");
    request.timeout = 75ms;
    auto started =
        client->start(std::move(request), [&](jb::net::HttpRequestId, jb::net::HttpCompletionResult result) -> void {
            ++callback_count;
            completion = std::move(result);
        });
    REQUIRE(started);
    REQUIRE(process_until(app, [&]() -> bool { return server.wait_for_requests(1U, 0ms); }));
    CHECK(callback_count == 0);

    REQUIRE(process_until(app, [&]() -> bool { return completion.has_value(); }));
    REQUIRE(completion);
    REQUIRE_FALSE(*completion);
    CHECK(callback_count == 1);
    CHECK(completion->error().kind == jb::net::HttpErrorKind::Timeout);
    CHECK(completion->error().error.category == jb::core::ErrorCategory::Timeout);
    CHECK(completion->error().error.code == "net.http.timeout");
    CHECK(completion->error().elapsed >= 1ms);
    check_safe_project_error(completion->error(),
                             {"secret-timeout-url", "secret-timeout-header", "secret-timeout-body"});
    CHECK(client->active_request_count() == 0U);
    CHECK(client->is_available());
    server.release_responses();
}

TEST_CASE("system HTTP client re-arms its shared timer for the next request deadline", "[net][http]")
{
    auto app     = jb::core::Application{0, nullptr};
    auto server  = jb::test::HttpTestServer{};
    auto created = SystemHttpClient::create(*app.event_loop());
    REQUIRE(created);
    auto client = std::move(created).value();

    auto later_completion   = std::optional<jb::net::HttpCompletionResult>{};
    auto earlier_completion = std::optional<jb::net::HttpCompletionResult>{};
    auto later_request      = minimal_request(server.url("/later-deadline"));
    later_request.timeout   = 250ms;
    auto later_started      = client->start(std::move(later_request),
                                            [&](jb::net::HttpRequestId, jb::net::HttpCompletionResult result) -> void {
                                           later_completion = std::move(result);
                                            });
    REQUIRE(later_started);

    auto earlier_request    = minimal_request(server.url("/earlier-deadline"));
    earlier_request.timeout = 75ms;
    auto earlier_started    = client->start(std::move(earlier_request),
                                            [&](jb::net::HttpRequestId, jb::net::HttpCompletionResult result) -> void {
                                             earlier_completion = std::move(result);
                                            });
    REQUIRE(earlier_started);

    REQUIRE(process_until(app, [&]() -> bool { return earlier_completion.has_value(); }));
    REQUIRE_FALSE(*earlier_completion);
    CHECK(earlier_completion->error().kind == jb::net::HttpErrorKind::Timeout);
    CHECK_FALSE(later_completion.has_value());
    CHECK(client->active_request_count() == 1U);

    REQUIRE(process_until(app, [&]() -> bool { return later_completion.has_value(); }));
    REQUIRE_FALSE(*later_completion);
    CHECK(later_completion->error().kind == jb::net::HttpErrorKind::Timeout);
    CHECK(client->active_request_count() == 0U);
    server.release_responses();
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

TEST_CASE("system HTTP cancellation cannot override an elapsed undispatched deadline", "[net][http]")
{
    auto app     = jb::core::Application{0, nullptr};
    auto created = SystemHttpClient::create(*app.event_loop());
    REQUIRE(created);
    auto client = std::move(created).value();

    constexpr auto timeout        = 25ms;
    auto           callback_count = 0;
    auto           completion     = std::optional<jb::net::HttpCompletionResult>{};
    auto           request        = minimal_request("http://127.0.0.1:1/expired-before-cancel");
    request.timeout               = timeout;
    auto started =
        client->start(std::move(request), [&](jb::net::HttpRequestId, jb::net::HttpCompletionResult result) -> void {
            ++callback_count;
            completion = std::move(result);
        });
    REQUIRE(started);

    // Poll only watchers until the accepted deadline must have elapsed, deliberately leaving timer dispatch pending.
    auto const definitely_expired = jb::core::Clock::now() + timeout;
    auto       polling_failed     = false;
    while (!polling_failed && jb::core::Clock::now() < definitely_expired) {
        polling_failed = app.process_events(jb::core::EventFlag::Watchers, 25) == jb::core::ProcessEventsResult::Failed;
    }
    REQUIRE_FALSE(polling_failed);

    auto cancelled = client->cancel(*started);
    REQUIRE_FALSE(cancelled);
    CHECK(cancelled.error().code == "net.http.request_not_found");
    CHECK(callback_count == 0);

    REQUIRE(process_until(app, [&]() -> bool { return completion.has_value(); }));
    REQUIRE_FALSE(*completion);
    CHECK(completion->error().kind == jb::net::HttpErrorKind::Timeout);
    CHECK(completion->error().error.code == "net.http.timeout");
    CHECK(callback_count == 1);
    CHECK(client->active_request_count() == 0U);
}

TEST_CASE("system HTTP cancellation retains response observations captured before a body barrier", "[net][http]")
{
    auto app      = jb::core::Application{0, nullptr};
    auto server   = jb::test::HttpTestServer{};
    auto response = jb::test::HttpTestResponse{
        .headers                    = {{.name = "X-Observed", .value = "safe-observed-value"}},
        .body                       = bytes("prefix-and-secret-response-tail"),
        .pause_after_response_bytes = 6U,
    };
    server.enqueue_response(std::move(response));
    server.release_responses();

    auto created = SystemHttpClient::create(*app.event_loop());
    REQUIRE(created);
    auto client = std::move(created).value();

    auto callback_count = 0;
    auto completion     = std::optional<jb::net::HttpCompletionResult>{};
    auto started        = client->start(minimal_request(server.url("/mid-body-cancel")),
                                        [&](jb::net::HttpRequestId, jb::net::HttpCompletionResult result) -> void {
                                     ++callback_count;
                                     completion = std::move(result);
                                        });
    REQUIRE(started);
    REQUIRE(process_until(app, [&]() -> bool { return server.wait_for_response_segments(1U, 0ms); }));
    REQUIRE(app.process_events(jb::core::EventFlag::All, 100) != jb::core::ProcessEventsResult::Failed);

    auto cancelled_result = client->cancel(*started);
    REQUIRE(cancelled_result);
    CHECK(callback_count == 0);
    server.release_response_segment();

    REQUIRE(process_until(app, [&]() -> bool { return completion.has_value(); }));
    REQUIRE(completion);
    REQUIRE_FALSE(*completion);
    CHECK(callback_count == 1);
    auto const& error = completion->error();
    CHECK(error.kind == jb::net::HttpErrorKind::Cancelled);
    CHECK(error.error.code == "net.http.cancelled");
    CHECK(error.status_code == 200U);
    CHECK(error.body.total_bytes == 6U);
    CHECK(byte_text(error.body.bytes) == "prefix");
    CHECK_FALSE(error.raw_headers.bytes.empty());
    CHECK(error.elapsed >= jb::core::Duration::zero());
    CHECK(client->active_request_count() == 0U);
}

TEST_CASE("system HTTP cancellation can win after the peer closes but before readiness is consumed", "[net][http]")
{
    auto app      = jb::core::Application{0, nullptr};
    auto server   = jb::test::HttpTestServer{};
    auto response = jb::test::HttpTestResponse{
        .body                       = bytes("unread-response"),
        .close_after_response_bytes = 0U,
    };
    server.enqueue_response(std::move(response));

    auto created = SystemHttpClient::create(*app.event_loop());
    REQUIRE(created);
    auto client = std::move(created).value();

    auto callback_count = 0;
    auto completion     = std::optional<jb::net::HttpCompletionResult>{};
    auto started        = client->start(minimal_request(server.url("/closed-before-cancel")),
                                        [&](jb::net::HttpRequestId, jb::net::HttpCompletionResult result) -> void {
                                     ++callback_count;
                                     completion = std::move(result);
                                        });
    REQUIRE(started);
    REQUIRE(process_until(app, [&]() -> bool { return server.wait_for_requests(1U, 0ms); }));

    server.release_responses();
    REQUIRE(server.wait_for_peer_closes(1U, 2s));
    REQUIRE(client->cancel(*started));
    CHECK(callback_count == 0);
    auto repeated = client->cancel(*started);
    REQUIRE_FALSE(repeated);
    CHECK(repeated.error().code == "net.http.request_not_found");

    REQUIRE(process_until(app, [&]() -> bool { return completion.has_value(); }));
    REQUIRE_FALSE(*completion);
    CHECK(completion->error().kind == jb::net::HttpErrorKind::Cancelled);
    CHECK(callback_count == 1);
    for (auto index = 0; index < 4; ++index) {
        REQUIRE(app.process_events(jb::core::EventFlag::All, 0) != jb::core::ProcessEventsResult::Failed);
    }
    CHECK(callback_count == 1);
}

TEST_CASE("system HTTP client maps a peer close during the response and retains partial data", "[net][http]")
{
    auto app      = jb::core::Application{0, nullptr};
    auto server   = jb::test::HttpTestServer{};
    auto response = jb::test::HttpTestResponse{
        .headers                    = {{.name = "X-Secret", .value = "secret-response-header"}},
        .body                       = bytes("response-secret-body"),
        .close_after_response_bytes = 4U,
    };
    server.enqueue_response(std::move(response));
    server.release_responses();

    auto created = SystemHttpClient::create(*app.event_loop());
    REQUIRE(created);
    auto client = std::move(created).value();

    auto completed = complete_request(app, *client, minimal_request(server.url("/secret-response-url")));
    REQUIRE_FALSE(completed);
    auto const& error = completed.error();
    CHECK(error.kind == jb::net::HttpErrorKind::Receive);
    CHECK(error.error.category == jb::core::ErrorCategory::Io);
    CHECK(error.error.code == "net.http.receive_failed");
    CHECK(error.status_code == 200U);
    CHECK(error.body.total_bytes == 4U);
    CHECK(byte_text(error.body.bytes) == "resp");
    CHECK_FALSE(error.raw_headers.bytes.empty());
    check_safe_project_error(error, {"secret-response-url", "secret-response-header", "response-secret-body"});
    CHECK(client->is_available());
}

TEST_CASE("system HTTP client keeps a peer reset during request transmission request-local", "[net][http]")
{
    auto app    = jb::core::Application{0, nullptr};
    auto server = jb::test::HttpTestServer{};
    server.reset_next_request_after_headers();

    auto created = SystemHttpClient::create(*app.event_loop());
    REQUIRE(created);
    auto client = std::move(created).value();

    auto request    = minimal_request(server.url("/secret-send-url"));
    request.method  = "POST";
    request.headers = {
        {.name = "X-Secret", .value = "secret-send-header"}
    };
    request.body = jb::core::ByteBuffer(std::size_t{8} * 1024U * 1024U, std::byte{0x5a});

    auto completed = complete_request(app, *client, std::move(request));
    REQUIRE_FALSE(completed);
    CHECK(server.wait_for_request_headers(1U, 0ms));
    auto const& error = completed.error();
    // A reset racing the final upload bytes may be surfaced by the kernel through curl's send or receive path.
    CHECK((error.kind == jb::net::HttpErrorKind::Send || error.kind == jb::net::HttpErrorKind::Receive));
    CHECK(error.error.category == jb::core::ErrorCategory::Io);
    CHECK((error.error.code == "net.http.send_failed" || error.error.code == "net.http.receive_failed"));
    check_safe_project_error(error, {"secret-send-url", "secret-send-header"});
    CHECK(client->is_available());
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

TEST_CASE("system HTTP client suppresses a queued cancellation completion during destruction", "[net][http]")
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
    REQUIRE(client->cancel(*started));
    CHECK(callback_count == 0);

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

TEST_CASE("curl multi adapter checks absolute deadlines before the deferred initial drive", "[net][http]")
{
    auto fake         = jb::core::priv::make_fake_event_loop();
    auto current_loop = jb::core::priv::ScopedCurrentEventLoop{fake.loop.get()};
    REQUIRE(jb::net::http::detail::preflight_curl_runtime());

    auto failure_count       = 0;
    auto initial_drive_count = 0;
    auto created             = jb::net::http::detail::CurlMultiAdapter::create(
        *fake.loop,
        [](auto*, auto) -> void {},
        [&](jb::core::Error const&) -> void { ++failure_count; },
        [&]() -> void { ++initial_drive_count; });
    REQUIRE(created);
    auto adapter = std::move(created).value();

    REQUIRE(adapter->queue_initial_drive());
    CHECK(initial_drive_count == 0);
    REQUIRE(fake.loop->process_events(jb::core::EventFlag::Tasks, 0) != jb::core::ProcessEventsResult::Failed);
    CHECK(initial_drive_count == 1);
    CHECK(failure_count == 0);
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
    CHECK(fake.backend->last_added_trigger_mode == jb::core::FdTriggerMode::Level);
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
    CHECK(fake.backend->last_added_trigger_mode == jb::core::FdTriggerMode::Level);

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
