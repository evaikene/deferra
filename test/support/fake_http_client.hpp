/** @file fake_http_client.hpp
 * @brief Defines a deterministic project HTTP client for tests.
 */
#pragma once

#include "error.hpp"
#include "http_client.hpp"
#include "result.hpp"

#include <cstddef>
#include <optional>
#include <vector>

namespace jb::test {

/** Test-only HttpClient with explicit owner-thread completion controls.
 *
 * Successful starts retain their handlers without invoking them. Tests complete requests explicitly, and active state
 * is retired before callback delivery. Accepted cancellation remains pending until complete_cancelled() is called.
 */
class FakeHttpClient final : public net::HttpClient {
public:
    /// Owns one accepted request together with its allocated identifier.
    struct StartRecord {
        /// Positive identifier returned by start().
        net::HttpRequestId id{0};
        /// Complete owning request accepted from the caller.
        net::HttpRequest   request;
    };

    /// Enables or disables new starts without changing active requests.
    void set_available(bool available) noexcept;

    /// Injects or clears a safe error returned before start acceptance.
    void set_start_error(std::optional<core::Error> error);

    /// Injects or clears a safe error returned before cancellation acceptance.
    void set_cancel_error(std::optional<core::Error> error);

    /// Returns every accepted owning request in start order.
    [[nodiscard]] auto start_records() const noexcept -> std::vector<StartRecord> const&;

    /// Returns every cancellation call in observation order, including rejected calls.
    [[nodiscard]] auto cancel_calls() const noexcept -> std::vector<net::HttpRequestId> const&;

    /// Returns active identifiers in start order.
    [[nodiscard]] auto pending_request_ids() const -> std::vector<net::HttpRequestId>;

    /** Completes one active request successfully and invokes its handler.
     * @param request_id Active identifier to retire.
     * @param response Owning response delivered to the handler.
     * @param reported_id Optional deliberately substituted callback identity for protocol tests.
     * @return Success after delivery, or a stable `test.http.*` selection error.
     */
    [[nodiscard]] auto complete_success(net::HttpRequestId                request_id,
                                        net::HttpResponse                 response,
                                        std::optional<net::HttpRequestId> reported_id = std::nullopt)
        -> core::Result<void, core::Error>;

    /** Completes one active request with an HTTP error and invokes its handler.
     * @param request_id Active identifier to retire.
     * @param error Owning error observation delivered to the handler.
     * @param reported_id Optional deliberately substituted callback identity for protocol tests.
     * @return Success after delivery, or a stable `test.http.*` selection error.
     */
    [[nodiscard]] auto complete_error(net::HttpRequestId                request_id,
                                      net::HttpError                    error,
                                      std::optional<net::HttpRequestId> reported_id = std::nullopt)
        -> core::Result<void, core::Error>;

    /** Delivers the required cancelled result for one cancellation-pending request.
     * @param request_id Active identifier whose cancellation was accepted.
     * @param partial Owning partial observation; kind and safe error are normalized to cancellation.
     * @return Success after delivery, or a stable `test.http.*` selection error.
     */
    [[nodiscard]] auto complete_cancelled(net::HttpRequestId request_id, net::HttpError partial = {})
        -> core::Result<void, core::Error>;

    /** Makes the fake permanently unavailable and completes every active request with one shared failure.
     * @param failure Safe `net.http.backend_failed`-style error copied to active observations and the failed signal.
     * @return Success on the first transition, or `test.http.shared_failure_already_set` thereafter.
     */
    [[nodiscard]] auto inject_shared_failure(core::Error failure) -> core::Result<void, core::Error>;

    [[nodiscard]] auto is_available() const noexcept -> bool override;
    [[nodiscard]] auto start(net::HttpRequest request, net::HttpCompletionHandler completion)
        -> core::Result<net::HttpRequestId, core::Error> override;
    [[nodiscard]] auto cancel(net::HttpRequestId request_id) -> core::Result<void, core::Error> override;
    [[nodiscard]] auto active_request_count() const noexcept -> std::size_t override;
    [[nodiscard]] auto failure() const -> std::optional<core::Error> override;

private:
    struct PendingRequest {
        net::HttpRequestId         id{0};
        net::HttpCompletionHandler completion;
        bool                       cancellation_requested{false};
    };

    [[nodiscard]] auto complete(net::HttpRequestId                request_id,
                                net::HttpCompletionResult         result,
                                std::optional<net::HttpRequestId> reported_id) -> core::Result<void, core::Error>;

    bool                            _available{true};
    net::HttpRequestId              _next_request_id{1};
    std::optional<core::Error>      _start_error;
    std::optional<core::Error>      _cancel_error;
    std::optional<core::Error>      _failure;
    std::vector<StartRecord>        _start_records;
    std::vector<PendingRequest>     _pending;
    std::vector<net::HttpRequestId> _completed_ids;
    std::vector<net::HttpRequestId> _cancel_calls;
};

} // namespace jb::test
