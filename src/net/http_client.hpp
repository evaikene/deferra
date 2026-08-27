/** @file http_client.hpp
 * @brief Defines the project-owned asynchronous HTTP client contract.
 */
#pragma once

#include "byte_buffer.hpp"
#include "error.hpp"
#include "event_loop_types.hpp"
#include "object.hpp"
#include "result.hpp"
#include "signal.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace jb::net {

/// Identifies one request accepted by an HttpClient.
/// Zero is reserved as an invalid identifier; concrete clients allocate positive, monotonically increasing values.
using HttpRequestId = std::uint64_t;

/// Owns one HTTP field name and value.
struct HttpHeader {
    /// Field name using HTTP token syntax.
    std::string name;
    /// Field value without line terminators or NUL bytes.
    std::string value;
    /// Marks a request field that must not cross an origin boundary during redirects.
    /// Known credential headers are sensitive regardless of this value.
    bool        sensitive{false};
};

/// Owns bounded retained bytes and metadata for the complete observed stream.
struct HttpCapturedData {
    /// Retained bytes in first/last capture order.
    jb::core::ByteBuffer bytes;
    /// Complete number of observed bytes, including bytes omitted from @ref bytes.
    std::uint64_t        total_bytes{0};
    /// True when @ref total_bytes exceeds the number of retained bytes.
    bool                 truncated{false};
};

/// Owns all configuration and data for one HTTP transfer and its redirect chain.
struct HttpRequest {
    /// Exact HTTP method; defaults to `GET` and must be a nonempty HTTP token of at most 32 bytes.
    std::string                         method{"GET"};
    /// Final-wire-form absolute HTTP or HTTPS URL, without user information or a fragment.
    std::string                         url;
    /// Ordered request fields. Names must be unique under ASCII case-insensitive comparison.
    std::vector<HttpHeader>             headers;
    /// Optional raw request body. Absence remains distinct from a present empty buffer.
    std::optional<jb::core::ByteBuffer> body;
    /// Whole-request monotonic deadline budget, including every redirect leg.
    /// Valid values range inclusively from 1 millisecond through 30 days.
    jb::core::Duration                  timeout{std::chrono::seconds{120}};
    /// Enables certificate and hostname verification for HTTPS requests.
    bool                                verify_tls{true};
    /// Enables secure manual redirect processing. Redirects are returned as ordinary responses when false.
    bool                                follow_redirects{false};
    /// Maximum redirect hops. Values above 20 are invalid, and enabled redirect processing requires at least one.
    std::uint32_t                       max_redirects{5};
    /// Maximum decompressed final-response body bytes retained by first/last capture.
    std::size_t                         response_body_limit{std::size_t{1024} * 1024U};
    /// Maximum raw final-response header bytes retained by first/last capture.
    std::size_t                         response_header_limit{std::size_t{64} * 1024U};
};

/// Owns the final successful transfer observation.
struct HttpResponse {
    /// Final HTTP status code.
    std::uint16_t           status_code{0};
    /// Parsed fields from the final non-informational response.
    std::vector<HttpHeader> headers;
    /// Bounded decompressed final-response body capture.
    HttpCapturedData        body;
    /// Bounded raw final-response status line and header block capture.
    HttpCapturedData        raw_headers;
    /// Number of redirect hops followed before this response.
    std::uint32_t           redirect_count{0};
    /// Monotonic elapsed time from accepted start through completion.
    jb::core::Duration      elapsed{};
    /// Whether peer verification was enabled and observed for HTTPS; absent for transfers without TLS.
    std::optional<bool>     tls_verified;
};

/// Stable transport-independent categories for unsuccessful HTTP requests.
enum class HttpErrorKind : std::uint8_t {
    InvalidRequest,  ///< The request failed project-owned validation.
    Resolve,         ///< Hostname resolution failed.
    Connect,         ///< Connection establishment, including proxy connection, failed.
    TlsVerification, ///< Certificate or hostname verification failed.
    TlsHandshake,    ///< TLS setup failed for a reason other than peer verification.
    Timeout,         ///< The whole-request deadline expired.
    Send,            ///< Request transmission failed.
    Receive,         ///< Response transfer failed.
    Redirect,        ///< Redirect policy, target, downgrade, or limit validation failed.
    Protocol,        ///< The HTTP response or fields were unusable.
    Cancelled,       ///< Caller-requested cancellation was observed.
    Internal,        ///< A bounded request-specific internal failure occurred.
};

/// Owns an unsuccessful transfer observation and bounded response data when available.
/// The embedded project error excludes URLs, header values, bodies, proxies, and backend diagnostics.
struct HttpError {
    /// Stable error category used for retry and outcome policy.
    HttpErrorKind                kind{HttpErrorKind::Internal};
    /// Safe project-owned error code and explanation.
    jb::core::Error              error;
    /// Final status code when one was observed before failure.
    std::optional<std::uint16_t> status_code;
    /// Parsed fields from the final response when available.
    std::vector<HttpHeader>      headers;
    /// Bounded decompressed response bytes observed before failure.
    HttpCapturedData             body;
    /// Bounded raw final-response header bytes observed before failure.
    HttpCapturedData             raw_headers;
    /// Number of redirect hops followed before failure.
    std::uint32_t                redirect_count{0};
    /// Monotonic elapsed time from accepted start through failure.
    jb::core::Duration           elapsed{};
    /// Whether peer verification was enabled and observed for HTTPS; absent when no TLS result exists.
    std::optional<bool>          tls_verified;
};

/// Owning successful response or unsuccessful transfer observation delivered for one accepted request.
using HttpCompletionResult  = jb::core::Result<HttpResponse, HttpError>;
/// Owning completion callable invoked exactly once for an accepted request.
/// The callable runs later on the client's owner thread and may re-enter the client after active state is retired.
using HttpCompletionHandler = std::function<void(HttpRequestId, HttpCompletionResult)>;

/// Abstract owner-thread asynchronous HTTP client.
///
/// Except for subscribing to @ref failed, callers use this object only on its EventLoop owner thread. A successful
/// start takes ownership of the request and handler and creates exactly one later completion. A rejected start retains
/// neither. Accepted cancellation also completes later and never invokes the handler from inside cancel(). Concrete
/// clients remove active state before invoking handlers so callbacks may safely start or cancel other requests.
///
/// Destroying a client removes active backend work and suppresses retained callbacks. Production owners must still
/// destroy callback consumers before the client. Concrete clients enter a permanent unavailable state after a shared
/// engine failure and emit @ref failed exactly once after recording that failure.
///
/// @par Stable error codes
/// `net.http.invalid_request`, `net.http.unavailable`, and `net.http.identifier_exhausted` reject starts;
/// `net.http.request_not_found` rejects cancellation of inactive identifiers. Later completions use
/// `net.http.resolve_failed`, `net.http.connect_failed`, `net.http.tls_verification_failed`,
/// `net.http.tls_handshake_failed`, `net.http.timeout`, `net.http.send_failed`, `net.http.receive_failed`,
/// `net.http.redirect_failed`, `net.http.protocol_error`, `net.http.cancelled`, or `net.http.internal`. A shared
/// adapter failure is reported as `net.http.backend_failed`. Error messages and details never contain request or
/// server-supplied data.
class HttpClient : public jb::core::Object {
public:
    /// Constructs a client optionally owned by @p parent.
    /// @param[in] parent Optional same-thread Object parent that owns this client.
    explicit HttpClient(jb::core::Object* parent = nullptr);

    /// Destroys the client through its abstract interface.
    ~HttpClient() override;

    /// Disables copy construction because backend callbacks retain this object's identity.
    HttpClient(HttpClient const&)                    = delete;
    /// Disables move construction because backend callbacks retain this object's identity.
    HttpClient(HttpClient&&)                         = delete;
    /// Disables copy assignment because backend callbacks retain this object's identity.
    auto operator=(HttpClient const&) -> HttpClient& = delete;
    /// Disables move assignment because backend callbacks retain this object's identity.
    auto operator=(HttpClient&&) -> HttpClient&      = delete;

    /// Reports whether the client currently accepts new requests.
    [[nodiscard]] virtual auto is_available() const noexcept -> bool = 0;

    /// Validates and accepts one asynchronous request.
    /// @param[in] request Owning request data transferred into the client on success.
    /// @param[in] completion Owning nonempty handler invoked exactly once later after an accepted start.
    /// @return A positive request identifier, or a safe `net.http.*` error. Failure creates no callback obligation.
    [[nodiscard]] virtual auto start(HttpRequest request, HttpCompletionHandler completion)
        -> jb::core::Result<HttpRequestId, jb::core::Error> = 0;

    /// Requests asynchronous cancellation of an active request.
    /// @param[in] request_id Positive identifier returned by start().
    /// @return Success when cancellation was accepted, or `net.http.request_not_found` for an inactive identifier.
    /// The original handler later receives HttpErrorKind::Cancelled after a successful return.
    [[nodiscard]] virtual auto cancel(HttpRequestId request_id) -> jb::core::Result<void, jb::core::Error> = 0;

    /// Returns the number of accepted requests whose handlers have not yet run.
    [[nodiscard]] virtual auto active_request_count() const noexcept -> std::size_t = 0;

    /// Returns the first shared backend failure after availability is lost, or no value while no such failure exists.
    [[nodiscard]] virtual auto failure() const -> std::optional<jb::core::Error> = 0;

    /// Emitted exactly once after a shared backend failure has made the client unavailable and updated active state.
    /// Signal subscription is thread-safe; the safe error excludes request and backend-provided data.
    jb::core::Signal<jb::core::Error> failed;
};

} // namespace jb::net
