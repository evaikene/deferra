/// @file control_client.hpp
/// @brief Typed asynchronous control calls over a borrowed JSON-RPC client.
///
#pragma once

#include "control_json.hpp"
#include "history.hpp"
#include "management.hpp"
#include "protocol.hpp"
#include "scheduler.hpp"
#include "secret.hpp"
#include "statistics.hpp"
#include "statistics_json.hpp"
#include "system_info.hpp"

#include "error.hpp"
#include "object.hpp"
#include "result.hpp"
#include "signal.hpp"

#include <chrono>
#include <cstdint>
#include <string_view>
#include <variant>
#include <vector>

namespace jb::rpc {
class Client;
}

namespace jb::jobu {

/// Nonzero identifier for one accepted typed call; never reused by a ControlClient instance.
using ControlCallId = std::uint64_t;

/// Timeout measured with the owner event loop's monotonic clock.
struct ControlCallOptions {
    std::chrono::milliseconds timeout{5000};
};

/// Successful null result from a method with no return value.
struct EmptyReply {};

/// Successful cron validation; invalid schedules are represented errors.
struct ScheduleValidationReply {};

/// Ordered UTC occurrences from a schedule preview.
struct ScheduleNextReply {
    std::vector<jb::core::UtcTimePoint> occurrences;
};

/// Owning result of a supported control operation. The call ID identifies which method produced it.
using ControlReply = std::variant<SystemInfo,
                                  Queue,
                                  QueuePage,
                                  JobDefinition,
                                  JobPage,
                                  RunDetails,
                                  RunPage,
                                  AttemptDetails,
                                  AttemptPage,
                                  AttemptOutputChunk,
                                  CancelRunResult,
                                  SecretMetadata,
                                  SecretPage,
                                  ScheduleValidationReply,
                                  ScheduleNextReply,
                                  StatisticsPage,
                                  EmptyReply>;

/// Distinguishes an observed remote JSON-RPC failure from a local client failure.
enum class ControlFailureKind : std::uint8_t {
    Local,
    Remote
};

/// One owning failure for an accepted typed call.
///
/// `outcome_unknown` is conservative for mutations whose request might have been transmitted but whose successful
/// result was not delivered. It is false for represented remote errors and known pre-write failures. Inspect the
/// corresponding resource or replay with an explicit idempotency key before deciding whether to repeat a mutation.
struct ControlFailure {
    ControlFailureKind                               kind{ControlFailureKind::Local};
    std::variant<jb::core::Error, jb::rpc::RpcError> error;
    bool                                             outcome_unknown{false};
};

/// Provides typed owner-thread JobU calls over an already-connected RPC client.
///
/// The RPC client and immutable attribute registry are borrowed and must outlive this object. All three objects must
/// share one non-null event loop and be used on its thread. The wrapper owns neither the device nor the event loop.
/// Request arguments are borrowed only while the typed member encodes them. A successful method return supplies a
/// local call ID, not an observed remote result; an immediate error means no request was written. Once accepted, a
/// mutation is never retried automatically.
/// Every accepted typed method call emits one reply or failure unless this wrapper is destroyed. Signals for
/// synchronous raw responses are delivered later through the event loop, after the accepting method has returned.
/// A ready, reply_received, failed, or call_failed handler may destroy the wrapper or its parent, stopping subsequent
/// wrapper outcomes. An emission already in progress retains Signal's connection-snapshot semantics, including any
/// queued receiver deliveries from that emission. Destruction emits no new outcomes. Handlers may call close() or
/// cancel_call() reentrantly; close() during terminal failed delivery does not discard its latched per-call failures.
///
/// Local `jobu.client.*` errors have fixed safe text and no backend detail:
///
/// @verbatim
/// Code                Category           Meaning
/// ------------------  -----------------  --------------------------------------------------------
/// not_ready           Unavailable        Wrong lifecycle state; no request written.
/// unsupported_method  Unsupported        Method not advertised; no request written.
/// invalid_timeout     InvalidArgument    Nonpositive or unrepresentable timeout; no write.
/// pending_limit       ResourceExhausted  Typed pending limit reached; no request written.
/// invalid_response    InvalidArgument    Malformed success result; call or handshake fails.
/// timeout             Timeout            Local deadline expired; mutation outcome may be unknown.
/// cancelled           Cancelled          Observation cancelled; remote work continues.
/// closed              Cancelled          Wrapper closed; pending observation ends.
/// handshake_failed    Unavailable        system.info returned an error; initialization ends.
/// unsupported_api     Unsupported        Incompatible API major; initialization ends.
/// @endverbatim
///
/// Raw `rpc.*` errors retain their own stable codes. A raw terminal error ends this wrapper and emits failed before
/// per-call failures in local-ID order. Represented remote errors retain their RpcError identity and are not terminal.
class ControlClient final : public jb::core::Object {
public:
    /// Constructs an uninitialized wrapper without sending a request.
    ControlClient(jb::rpc::Client& rpc, AttributeRegistry const& attributes, jb::core::Object* parent = nullptr);
    ~ControlClient() override;

    ControlClient(ControlClient const&)                    = delete;
    ControlClient(ControlClient&&)                         = delete;
    auto operator=(ControlClient const&) -> ControlClient& = delete;
    auto operator=(ControlClient&&) -> ControlClient&      = delete;

    /// Sends one system.info handshake. Only known pre-write failures return an error.
    /// Successful acceptance later emits ready or failed, once. Repeated initialization is rejected.
    [[nodiscard]] auto initialize(ControlCallOptions options = {}) -> jb::core::Result<void, jb::core::Error>;

    /// Reads the current daemon information after initialization; replies with SystemInfo.
    [[nodiscard]] auto get_system_info(ControlCallOptions options = {})
        -> jb::core::Result<ControlCallId, jb::core::Error>;

    /// Reads one system-wide statistics page; replies with StatisticsPage.
    [[nodiscard]] auto system_statistics(StatisticsListRequest const& request, ControlCallOptions options = {})
        -> jb::core::Result<ControlCallId, jb::core::Error>;

    /// Creates a queue; an unobserved transmitted call may have committed and should be reconciled by key.
    [[nodiscard]] auto create_queue(CreateQueueRequest const& request, ControlCallOptions options = {})
        -> jb::core::Result<ControlCallId, jb::core::Error>;

    /// Reads one queue by ID or exact name; replies with Queue.
    [[nodiscard]] auto get_queue(QueueSelector const& selector, ControlCallOptions options = {})
        -> jb::core::Result<ControlCallId, jb::core::Error>;

    /// Reads one bounded queue page using its ascending-ID continuation; replies with QueuePage.
    [[nodiscard]] auto list_queues(QueueListRequest const& request, ControlCallOptions options = {})
        -> jb::core::Result<ControlCallId, jb::core::Error>;

    /// Updates supplied queue fields; replies with Queue after commit.
    [[nodiscard]] auto update_queue(UpdateQueueRequest const& request, ControlCallOptions options = {})
        -> jb::core::Result<ControlCallId, jb::core::Error>;

    /// Requests queue suspension; replies with the updated Queue.
    [[nodiscard]] auto suspend_queue(QueueSelector const& selector, ControlCallOptions options = {})
        -> jb::core::Result<ControlCallId, jb::core::Error>;

    /// Resumes a queue; replies with the updated Queue.
    [[nodiscard]] auto resume_queue(QueueSelector const& selector, ControlCallOptions options = {})
        -> jb::core::Result<ControlCallId, jb::core::Error>;

    /// Deletes a queue; a successful JSON null replies as EmptyReply.
    [[nodiscard]] auto delete_queue(QueueSelector const& selector, ControlCallOptions options = {})
        -> jb::core::Result<ControlCallId, jb::core::Error>;

    /// Reads queue-scoped statistics by selector or cursor; replies with StatisticsPage.
    [[nodiscard]] auto queue_statistics(QueueStatisticsListRequest const& request, ControlCallOptions options = {})
        -> jb::core::Result<ControlCallId, jb::core::Error>;

    /// Creates a job using the shared public codec; a possibly sent failure may have created it.
    [[nodiscard]] auto create_job(CreateJobRequest const& request, ControlCallOptions options = {})
        -> jb::core::Result<ControlCallId, jb::core::Error>;

    /// Reads one job definition by ID; replies with JobDefinition.
    [[nodiscard]] auto get_job(jb::core::Uuid const& id, ControlCallOptions options = {})
        -> jb::core::Result<ControlCallId, jb::core::Error>;

    /// Reads one bounded job-definition page; replies with JobPage.
    [[nodiscard]] auto list_jobs(JobListRequest const& request, ControlCallOptions options = {})
        -> jb::core::Result<ControlCallId, jb::core::Error>;

    /// Updates a job at its expected revision; replies with the committed JobDefinition.
    [[nodiscard]] auto update_job(UpdateJobRequest const& request, ControlCallOptions options = {})
        -> jb::core::Result<ControlCallId, jb::core::Error>;

    /// Requests job suspension; replies with the updated JobDefinition.
    [[nodiscard]] auto suspend_job(jb::core::Uuid const& id, ControlCallOptions options = {})
        -> jb::core::Result<ControlCallId, jb::core::Error>;

    /// Resumes a job; replies with the updated JobDefinition.
    [[nodiscard]] auto resume_job(jb::core::Uuid const& id, ControlCallOptions options = {})
        -> jb::core::Result<ControlCallId, jb::core::Error>;

    /// Moves a suspended job at its expected revision; replies with JobDefinition.
    [[nodiscard]] auto move_job(MoveJobRequest const& request, ControlCallOptions options = {})
        -> jb::core::Result<ControlCallId, jb::core::Error>;

    /// Deletes a job at its expected revision; a successful JSON null replies as EmptyReply.
    [[nodiscard]] auto delete_job(DeleteJobRequest const& request, ControlCallOptions options = {})
        -> jb::core::Result<ControlCallId, jb::core::Error>;

    /// Creates a manual run without changing the scheduled occurrence; replies with RunDetails.
    [[nodiscard]] auto run_now(RunNowRequest const& request, ControlCallOptions options = {})
        -> jb::core::Result<ControlCallId, jb::core::Error>;

    /// Reads one retained run by ID; replies with RunDetails.
    [[nodiscard]] auto get_run(jb::core::Uuid const& id, ControlCallOptions options = {})
        -> jb::core::Result<ControlCallId, jb::core::Error>;

    /// Reads one run-summary page using an initial query or cursor-only continuation.
    [[nodiscard]] auto list_runs(RunListRequest const& request, ControlCallOptions options = {})
        -> jb::core::Result<ControlCallId, jb::core::Error>;

    /// Requests cancellation of a run; replies with CancelRunResult, whose requested disposition may still be active.
    [[nodiscard]] auto cancel_run(jb::core::Uuid const& id, ControlCallOptions options = {})
        -> jb::core::Result<ControlCallId, jb::core::Error>;

    /// Reads one retained attempt by run ID and number in [1, maximum_attempt_number]; replies with AttemptDetails.
    [[nodiscard]] auto get_attempt(AttemptKey const& key, ControlCallOptions options = {})
        -> jb::core::Result<ControlCallId, jb::core::Error>;

    /// Reads one bounded attempt page using an initial query or cursor; replies with AttemptPage.
    [[nodiscard]] auto list_attempts(AttemptListRequest const& request, ControlCallOptions options = {})
        -> jb::core::Result<ControlCallId, jb::core::Error>;

    /// Reads one bounded retained-output slice for a valid attempt number; replies with AttemptOutputChunk.
    [[nodiscard]] auto read_attempt_output(AttemptOutputRequest const& request, ControlCallOptions options = {})
        -> jb::core::Result<ControlCallId, jb::core::Error>;

    /// Sets raw secret bytes without exposing them in the reply; replies with SecretMetadata.
    [[nodiscard]] auto set_secret(SetSecretRequest const& request, ControlCallOptions options = {})
        -> jb::core::Result<ControlCallId, jb::core::Error>;

    /// Reads a metadata-only secret page; replies with SecretPage.
    [[nodiscard]] auto list_secrets(SecretListRequest const& request, ControlCallOptions options = {})
        -> jb::core::Result<ControlCallId, jb::core::Error>;

    /// Deletes a secret by name; a successful JSON null replies as EmptyReply.
    [[nodiscard]] auto delete_secret(std::string_view name, ControlCallOptions options = {})
        -> jb::core::Result<ControlCallId, jb::core::Error>;

    /// Validates a cron schedule; replies with ScheduleValidationReply only when valid.
    [[nodiscard]] auto validate_schedule(CronSchedule const& schedule, ControlCallOptions options = {})
        -> jb::core::Result<ControlCallId, jb::core::Error>;

    /// Previews strictly later cron occurrences; replies with ScheduleNextReply.
    [[nodiscard]] auto next_schedule_occurrences(ScheduleNextRequest const& request, ControlCallOptions options = {})
        -> jb::core::Result<ControlCallId, jb::core::Error>;

    /// Forgets local observation of a call without sending run.cancel to the daemon.
    /// Unknown and already-completed IDs do nothing; a live call emits one local cancellation failure.
    void cancel_call(ControlCallId id);

    /// Fails outstanding calls and releases local correlations, without closing the borrowed RPC client or device.
    void close();

    /// Emitted once after a compatible system.info result is decoded. Missing methods remain usable as local errors.
    jb::core::Signal<SystemInfo>                    ready;
    /// Emitted after an accepted call's typed result has been decoded.
    jb::core::Signal<ControlCallId, ControlReply>   reply_received;
    /// Emitted once per accepted call that did not deliver a typed result.
    jb::core::Signal<ControlCallId, ControlFailure> call_failed;
    /// Emitted once on handshake failure or terminal raw-client failure, before affected call_failed emissions.
    jb::core::Signal<jb::core::Error>               failed;

private:
    struct Private;
};

} // namespace jb::jobu
