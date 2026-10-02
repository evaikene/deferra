#pragma once

#include "error.hpp"
#include "result.hpp"
#include "scheduler.hpp"
#include "time_source.hpp"
#include "uuid.hpp"
#include "wait_repository_priv.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>

namespace jb::db {
class Database;
}

namespace jb::jobu {

class AttemptExecutor;
class AttributeRegistry;
class SecretProvider;
class CronEngine;
struct DelayedRun;

namespace detail {

struct SchedulerCoreOptions {
    std::uint32_t       cli_concurrency{4};
    std::uint32_t       http_concurrency{16};
    std::size_t         candidate_batch_size{200};
    ExecutionTelemetry* telemetry{nullptr};
};

struct SchedulerCycleResult {
    jb::core::UtcTimePoint                sampled_utc_now;
    std::optional<jb::core::UtcTimePoint> next_wake;
    /// Absolute monotonic deadline, independent of UTC policy and immutable sample identity.
    std::optional<jb::core::TimePoint>    next_warning;
    /// A post-commit receiver changed already-observed eligibility or warning policy.
    bool                                  rescan{false};
    /// Delivery stopped/destroyed telemetry; the public adapter must enter terminal shutdown.
    bool                                  shutdown_requested{false};
};

struct SchedulerCoreCallbacks {
    std::function<void()>                       rescan_requested;
    std::function<void(jb::core::Error const&)> failure_reported;
};

class SchedulerCore final {
public:
    SchedulerCore(jb::db::Database&        database,
                  AttributeRegistry const& attributes,
                  CronEngine const&        cron,
                  jb::core::UuidGenerator& uuid_generator,
                  jb::core::TimeSource&    time_source,
                  AttemptExecutor&         executor,
                  SecretProvider&          secrets,
                  SchedulerCoreOptions     options   = SchedulerCoreOptions(),
                  SchedulerCoreCallbacks   callbacks = {});
    ~SchedulerCore();

    // Retained tokens identify one fixed owner address and must never be shared by copied cores.
    SchedulerCore(SchedulerCore const&)                    = delete;
    SchedulerCore(SchedulerCore&&)                         = delete;
    auto operator=(SchedulerCore const&) -> SchedulerCore& = delete;
    auto operator=(SchedulerCore&&) -> SchedulerCore&      = delete;

    [[nodiscard]] auto cancel_run(jb::core::Uuid const& run_id) -> jb::core::Result<CancelRunResult, jb::core::Error>;
    [[nodiscard]] auto process_cycle() -> jb::core::Result<SchedulerCycleResult, jb::core::Error>;
    void               reset() noexcept;
    void               shutdown() noexcept;

    /// Explicit context of the first latched failure, for the public sanitizer.
    [[nodiscard]] auto failure_operation() const noexcept -> StorageOperation { return _failure_operation; }

    [[nodiscard]] auto failure_origin() const noexcept -> StorageFailureOrigin { return _failure_origin; }

private:
    struct CompletionToken {
        SchedulerCore* owner{nullptr};
        bool           terminal{false};
    };

    [[nodiscard]] auto process_cycle_impl() -> jb::core::Result<SchedulerCycleResult, jb::core::Error>;
    [[nodiscard]] auto observe_backlog(bool& rescan)
        -> jb::core::Result<std::optional<jb::core::TimePoint>, jb::core::Error>;
    [[nodiscard]] auto deliver_delay(DelayedRun value) -> bool;
    [[nodiscard]] auto cancel_run_impl(jb::core::Uuid const& run_id)
        -> jb::core::Result<CancelRunResult, jb::core::Error>;
    void               fail(jb::core::Error const& error, bool notify = true);
    /// Post-unwind failure boundary; gates retained completions before user notifications.
    [[nodiscard]] auto fail_operation(jb::core::Error error, StorageOperation operation, bool notify)
        -> jb::core::Error;

    jb::db::Database&                       _database;
    AttributeRegistry const&                _attributes;
    CronEngine const&                       _cron;
    jb::core::UuidGenerator&                _uuid_generator;
    jb::core::TimeSource&                   _time_source;
    AttemptExecutor&                        _executor;
    SecretProvider&                         _secrets;
    SchedulerCoreOptions                    _options;
    SchedulerCoreCallbacks                  _callbacks;
    std::shared_ptr<CompletionToken>        _completion_token;
    std::map<jb::core::Uuid, std::uint32_t> _queue_weights;
    std::map<jb::core::Uuid, std::int64_t>  _cli_credits;
    std::map<jb::core::Uuid, std::int64_t>  _http_credits;
    std::map<jb::core::Uuid, std::uint64_t> _active_attempts;
    std::set<jb::core::Uuid>                _cancellation_requests;
    std::optional<jb::core::Error>          _failure;
    std::optional<TelemetryFailure>         _telemetry_failure;
    StorageOperation                        _failure_operation{StorageOperation::Dispatch};
    StorageFailureOrigin                    _failure_origin{StorageFailureOrigin::Operation};
    bool                                    _cli_first{true};
};

} // namespace detail

} // namespace jb::jobu
