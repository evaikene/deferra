/** @file system_http_client.hpp
 * @brief Defines the libcurl-backed system HTTP client factory.
 */
#pragma once

#include "error.hpp"
#include "event_loop.hpp"
#include "http_client.hpp"
#include "result.hpp"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace jb::net::http {

/** Owns process-level configuration copied by SystemHttpClient::create(). */
struct SystemHttpClientOptions {
    /** Optional certificate-authority bundle used instead of the backend default.
     *
     * When present, the path must identify a readable regular file. Errors never include the path value.
     */
    std::optional<std::filesystem::path> ca_bundle;
    /** Optional explicit HTTP or HTTPS proxy URL.
     *
     * The URL must be absolute and contain no user information. An HTTPS proxy requires corresponding runtime
     * support. The factory validates this value, but proxy-backed starts remain unavailable until Stage 5.7. Absence
     * makes admitted transfers explicitly ignore proxy environment variables.
     */
    std::optional<std::string>           proxy;
    /** Inclusive hard limit for parsed response-header bytes, independent of user-visible raw capture.
     *
     * Valid values range from one byte through 64 MiB.
     */
    std::size_t                          maximum_parsed_response_header_bytes{std::size_t{8} * 1024U * 1024U};
};

/** Sole-owning system implementation of the project HTTP client contract.
 *
 * create() performs process-wide libcurl initialization and runtime/option preflight before returning an instance.
 * The supplied EventLoop is borrowed and must outlive the client. Factory creation and all client operations occur on
 * that loop's owner thread. The returned unique pointer is the sole owner; the client is a root Object without a
 * parent.
 *
 * The client owns one libcurl multi handle driven by the borrowed EventLoop. It supports non-redirect HTTP transfers
 * with exact validated methods, owning request fields and optional binary bodies, automatic response decompression,
 * parsed final-response fields, and bounded first/last body and raw-header capture. One monotonic deadline covers the
 * complete admitted transfer. Request-local resolve, connect, TLS, timeout, send, receive, and protocol failures use
 * stable safe `net.http.*` errors and retain bounded response observations when available. Plain HTTP responses report
 * no TLS observation. The handler of every accepted request runs exactly once later on the owner thread, after active
 * transfer state is retired, and a rejected start retains no handler. Redirects, HTTPS/CA/unsafe-TLS behavior, and
 * configured proxies remain deferred to their later implementation stages.
 *
 * A fatal multi, watch, timer, or deferred-drive error permanently makes the client unavailable, records a safe
 * `net.http.backend_failed` error, completes accepted work once with HttpErrorKind::Internal, and emits failed once.
 * Destruction removes backend work and suppresses retained handlers; callback consumers must still be destroyed first.
 *
 * @par Stable factory error codes
 * `net.http.event_loop_unavailable` reports an invalid or non-current loop; `net.http.invalid_options` reports a
 * rejected option using only a safe reason token; `net.http.runtime_unavailable` reports missing libcurl
 * initialization, version, protocol, TLS, compression, asynchronous-DNS, or requested HTTPS-proxy support.
 * `net.http.backend_failed` reports failure to construct the multi adapter.
 */
class SystemHttpClient final : public jb::net::HttpClient {
public:
    /** Creates a checked root system HTTP client.
     * @param loop Borrowed valid EventLoop currently installed on the calling thread.
     * @param options Owning options copied into the client after complete validation.
     * @return Sole ownership of a completely initialized client, or a safe `net.http.*` error. Failure returns no
     * partially usable object and does not catch allocation failures.
     */
    [[nodiscard]] static auto create(jb::core::EventLoop& loop, SystemHttpClientOptions options = {})
        -> jb::core::Result<std::unique_ptr<SystemHttpClient>, jb::core::Error>;

    /** Removes all multi handles, watches, and timers and suppresses running or queued request handlers. */
    ~SystemHttpClient() override;

    /// Prevents copying a thread-affine client with fixed backend ownership.
    SystemHttpClient(SystemHttpClient const&)                    = delete;
    /// Prevents moving a thread-affine client with fixed backend ownership.
    SystemHttpClient(SystemHttpClient&&)                         = delete;
    /// Prevents copy assignment of a thread-affine client.
    auto operator=(SystemHttpClient const&) -> SystemHttpClient& = delete;
    /// Prevents move assignment of a thread-affine client.
    auto operator=(SystemHttpClient&&) -> SystemHttpClient&      = delete;

    /** Reports transfer admission availability.
     * @return True while the multi adapter accepts the implemented non-redirect HTTP request scope; false after shared
     * failure.
     */
    [[nodiscard]] auto is_available() const noexcept -> bool override;

    /** Validates and starts one deadline-bounded non-redirect asynchronous HTTP transfer.
     * @param request Owning request transferred into the client only on success.
     * @param completion Owning nonempty handler invoked exactly once later after success.
     * @return A positive request identifier, or a safe `net.http.*` error. Failure retains no handler and creates no
     * callback obligation. A successful transfer or stable request-local error is delivered with elapsed and bounded
     * response observations. Redirects, HTTPS, unsafe TLS, and configured-proxy transfers remain rejected until their
     * later implementation stages.
     */
    [[nodiscard]] auto start(jb::net::HttpRequest request, jb::net::HttpCompletionHandler completion)
        -> jb::core::Result<jb::net::HttpRequestId, jb::core::Error> override;

    /** Requests asynchronous cancellation of an active transfer.
     * @param request_id Positive identifier returned by start().
     * @return Success when cancellation is accepted, or `net.http.request_not_found` when the identifier is inactive.
     * A successful call delivers one later HttpErrorKind::Cancelled completion with elapsed and any bounded response
     * observations captured before removal. Repeated cancellation is rejected.
     */
    [[nodiscard]] auto cancel(jb::net::HttpRequestId request_id) -> jb::core::Result<void, jb::core::Error> override;

    /** Returns the number of retained completion obligations.
     * @return Accepted requests whose handlers have not yet run, including queued cancellation/failure completions.
     */
    [[nodiscard]] auto active_request_count() const noexcept -> std::size_t override;

    /** Returns the first shared backend failure recorded after construction.
     * @return Safe `net.http.backend_failed` value after availability is lost, or no value beforehand.
     */
    [[nodiscard]] auto failure() const -> std::optional<jb::core::Error> override;

private:
    explicit SystemHttpClient(jb::core::EventLoop& loop, SystemHttpClientOptions options);

    struct Private;
};

} // namespace jb::net::http
