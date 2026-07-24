/** @file fake_attempt_executor.hpp
 * @brief Defines a deterministic owner-thread attempt executor for tests.
 */
#pragma once

#include "attempt_executor.hpp"

#include <optional>
#include <vector>

namespace jb::test {

/** Test-only AttemptExecutor with explicit completion and call recording.
 *
 * CLI and HTTP availability are configured independently. A start for a disabled type is recorded and returns
 * `test.executor.type_unavailable` without retaining its handler. Starts and cancellations run only on the calling test
 * thread, no thread or external operation is created, and successful starts remain pending until complete() is called.
 */
class FakeAttemptExecutor final : public jobu::AttemptExecutor {
public:
    /// Enables or disables starts for one runner family; both families are unavailable initially.
    void set_available(jobu::JobType type, bool available) noexcept;

    /// Injects or clears the error returned by start().
    void set_start_error(std::optional<core::Error> error);

    /// Injects or clears the error returned by cancel().
    void set_cancel_error(std::optional<core::Error> error);

    /// Returns all owning start requests in observation order, including failed starts.
    [[nodiscard]] auto start_requests() const noexcept -> std::vector<jobu::AttemptStartRequest> const&;

    /// Returns successfully started attempt keys that have not completed, in start order.
    [[nodiscard]] auto pending_keys() const -> std::vector<jobu::AttemptKey>;

    /// Returns all cancellation requests in observation order, including failed requests.
    [[nodiscard]] auto cancel_calls() const noexcept -> std::vector<jobu::AttemptKey> const&;

    /** Completes one selected pending attempt synchronously on the calling test thread.
     *
     * The helper validates selection/key consistency, exact-once completion, outcome fields, the result-object shape,
     * deterministic JSON serialization, and the 256 KiB result limit before invoking the retained handler.
     *
     * @param key Pending attempt selected by the test.
     * @param completion Owning completion whose key must equal `key`.
     * @return Success after invoking the handler, or a stable `test.executor.*` error without consuming the pending
     * attempt.
     */
    [[nodiscard]] auto complete(jobu::AttemptKey const& key, jobu::AttemptCompletion completion)
        -> core::Result<void, core::Error>;

    [[nodiscard]] auto is_available(jobu::JobType type) const noexcept -> bool override;

    [[nodiscard]] auto start(jobu::AttemptStartRequest request, jobu::AttemptCompletionHandler completion)
        -> core::Result<void, core::Error> override;

    [[nodiscard]] auto cancel(jobu::AttemptKey const& key) -> core::Result<void, core::Error> override;

private:
    struct PendingAttempt {
        jobu::AttemptKey               key;
        jobu::AttemptCompletionHandler completion;
    };

    bool                                   _cli_available{false};
    bool                                   _http_available{false};
    std::optional<core::Error>             _start_error;
    std::optional<core::Error>             _cancel_error;
    std::vector<jobu::AttemptStartRequest> _start_requests;
    std::vector<PendingAttempt>            _pending;
    std::vector<jobu::AttemptKey>          _completed_keys;
    std::vector<jobu::AttemptKey>          _cancel_calls;
};

} // namespace jb::test
