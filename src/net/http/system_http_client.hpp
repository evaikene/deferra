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
     * support. Absence means proxy environment variables will not be inherited when transfers are implemented.
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
 * Stage 5.2 constructs only the checked backend boundary. Until the EventLoop transfer engine is installed, the
 * client reports unavailable and rejects every request without retaining its completion handler.
 *
 * @par Stable factory error codes
 * `net.http.event_loop_unavailable` reports an invalid or non-current loop; `net.http.invalid_options` reports a
 * rejected option using only a safe reason token; `net.http.runtime_unavailable` reports missing libcurl
 * initialization, version, protocol, TLS, compression, asynchronous-DNS, or requested HTTPS-proxy support.
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

    /** Releases the retained option snapshot. No transfer callbacks exist in this stage. */
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
     * @return False until the EventLoop transfer engine is installed.
     */
    [[nodiscard]] auto is_available() const noexcept -> bool override;

    /** Rejects a request without retaining either owning argument.
     * @param request Owning request discarded when this construction-only backend rejects admission.
     * @param completion Owning handler discarded without invocation.
     * @return `net.http.unavailable`; no request identifier or callback obligation is created.
     */
    [[nodiscard]] auto start(jb::net::HttpRequest request, jb::net::HttpCompletionHandler completion)
        -> jb::core::Result<jb::net::HttpRequestId, jb::core::Error> override;

    /** Rejects cancellation because this stage cannot have active requests.
     * @param request_id Request identifier that cannot name active work in this stage.
     * @return `net.http.request_not_found`.
     */
    [[nodiscard]] auto cancel(jb::net::HttpRequestId request_id) -> jb::core::Result<void, jb::core::Error> override;

    /** Returns the number of retained completion obligations.
     * @return Zero because request admission is disabled.
     */
    [[nodiscard]] auto active_request_count() const noexcept -> std::size_t override;

    /** Returns a shared backend failure recorded after construction.
     * @return No value because this stage has no transfer engine that can enter a failed state.
     */
    [[nodiscard]] auto failure() const -> std::optional<jb::core::Error> override;

private:
    explicit SystemHttpClient(SystemHttpClientOptions options);

    struct Private;
    std::unique_ptr<Private> _data;
};

} // namespace jb::net::http
