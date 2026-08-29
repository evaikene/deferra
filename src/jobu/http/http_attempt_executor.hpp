/** @file http_attempt_executor.hpp
 * @brief Defines the JobU HTTP attempt executor.
 */
#pragma once

#include "attempt_executor.hpp"
#include "http_client.hpp"
#include "time_source.hpp"

#include <memory>

namespace jb::jobu::http {

/** Executes durable JobU HTTP attempts through a project-owned asynchronous client.
 *
 * The executor borrows @p client and @p time_source; both must outlive it. Construct, use, and destroy the executor on
 * the shared client/scheduler owner thread. A successful start retains one completion handler until the client reports
 * a terminal observation. The handler runs directly on that owner thread after all executor active state has been
 * retired, so it may re-enter the executor. Rejected starts invoke no handler and retain no callback.
 *
 * HTTP availability follows the borrowed client. CLI and unknown runner families are unavailable. Cancellation is
 * asynchronous: success leaves the original completion obligation active until the client reports cancellation.
 * Destruction first suppresses retained attempt handlers, then requests cancellation of accepted client work; late
 * client callbacks cannot invoke scheduler code.
 *
 * @par Stable error codes
 * `jobu.http.unsupported_type` and `jobu.http.invalid_start` reject invalid executor inputs;
 * `jobu.http.invalid_snapshot` rejects corrupt payload or materialized attributes; `jobu.http.duplicate_attempt`
 * rejects an already-active key; `jobu.http.start_failed` wraps a safe client start rejection;
 * `jobu.http.attempt_not_found` rejects cancellation of an inactive key; and `jobu.http.cancel_failed` wraps a safe
 * client cancellation rejection. Errors and TLS warnings contain no URL, header value, body, or attribute value.
 */
class HttpAttemptExecutor final : public AttemptExecutor {
public:
    /** Constructs an idle HTTP executor without starting external work.
     * @param client Asynchronous HTTP client borrowed for the executor lifetime.
     * @param time_source Wall-clock source borrowed for completion and Retry-After policy.
     * @warning Both dependencies and this call belong to the shared owner thread.
     */
    HttpAttemptExecutor(jb::net::HttpClient& client, jb::core::TimeSource& time_source);

    /** Suppresses retained attempt handlers and requests cancellation of accepted client work.
     *
     * The borrowed client must remain alive through this call and any later client-side cancellation completion.
     */
    ~HttpAttemptExecutor() override;

    /// Prevents copying an executor whose callbacks retain its identity.
    HttpAttemptExecutor(HttpAttemptExecutor const&)                    = delete;
    /// Prevents moving an executor whose callbacks retain its identity.
    HttpAttemptExecutor(HttpAttemptExecutor&&)                         = delete;
    /// Prevents copy assignment of borrowed dependencies and active callback state.
    auto operator=(HttpAttemptExecutor const&) -> HttpAttemptExecutor& = delete;
    /// Prevents move assignment of borrowed dependencies and active callback state.
    auto operator=(HttpAttemptExecutor&&) -> HttpAttemptExecutor&      = delete;

    /** Reports whether one runner family can currently be accepted.
     * @param type Runner family to query.
     * @return The client's availability for HTTP; false for CLI and unknown values.
     * @warning Call only on the shared owner thread.
     */
    [[nodiscard]] auto is_available(JobType type) const noexcept -> bool override;

    /** Starts one HTTP request for an already durable running attempt.
     * @param request Owning immutable execution snapshot.
     * @param completion Owning nonempty exactly-once completion handler.
     * @return Success after client acceptance, or a safe `jobu.http.*` error with no callback obligation.
     * @warning Call only on the shared owner thread. The handler never runs inside this call.
     */
    [[nodiscard]] auto start(AttemptStartRequest request, AttemptCompletionHandler completion)
        -> jb::core::Result<void, jb::core::Error> override;

    /** Requests cancellation of an active HTTP attempt.
     * @param key Active durable attempt identity.
     * @return Success when the client accepts cancellation, or a safe `jobu.http.*` error.
     * @warning Call only on the shared owner thread. Success does not invoke or discard the completion handler.
     */
    [[nodiscard]] auto cancel(AttemptKey const& key) -> jb::core::Result<void, jb::core::Error> override;

private:
    struct Private;
    std::unique_ptr<Private> _data;
};

} // namespace jb::jobu::http
