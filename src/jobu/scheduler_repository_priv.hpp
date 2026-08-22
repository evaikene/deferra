#pragma once

#include "attempt.hpp"
#include "attribute.hpp"
#include "job.hpp"
#include "queue.hpp"
#include "result.hpp"
#include "run.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace jb::db {
class Database;
}

namespace jb::jobu::detail {

struct QueueRuntime {
    jb::core::Uuid id;
    std::uint32_t  weight{1};
    std::uint32_t  concurrency_limit{1};

    auto operator==(QueueRuntime const&) const -> bool = default;
};

struct CapacityUsage {
    std::uint32_t cli_running{0};
    std::uint32_t http_running{0};
    std::uint32_t queue_slots{0};

    auto operator==(CapacityUsage const&) const -> bool = default;
};

struct BlockingRetryCandidate {
    jb::core::Uuid         run_id;
    jb::core::Uuid         queue_id;
    JobType                type{JobType::Cli};
    std::int32_t           priority{0};
    jb::core::UtcTimePoint runnable_at;
    jb::core::UtcTimePoint planned_at;

    auto operator==(BlockingRetryCandidate const&) const -> bool = default;
};

struct CapacityRow {
    jb::core::Uuid                        run_id;
    jb::core::Uuid                        queue_id;
    CapacityUsage                         usage;
    std::optional<BlockingRetryCandidate> blocking_retry;

    auto operator==(CapacityRow const&) const -> bool = default;
};

struct ManualBarrier {
    jb::core::Uuid run_id;
    jb::core::Uuid job_id;

    auto operator==(ManualBarrier const&) const -> bool = default;
};

struct DispatchCandidate {
    JobRun     run;
    JobState   job_state{JobState::Active};
    QueueState queue_state{QueueState::Active};
};

struct DispatchContext {
    JobRun        run;
    JobState      job_state{JobState::Active};
    Queue         queue;
    AttemptNumber next_attempt{1};
};

struct CompletionContext {
    JobRun     run;
    JobState   job_state{JobState::Active};
    QueueState queue_state{QueueState::Active};
};

class SchedulerRepository final {
public:
    SchedulerRepository(jb::db::Database& database, AttributeRegistry const& attributes) noexcept;

    [[nodiscard]] auto list_runtime_queues(std::size_t limit, std::optional<jb::core::Uuid> after_id)
        -> jb::core::Result<std::vector<QueueRuntime>, jb::core::Error>;
    [[nodiscard]] auto list_capacity_rows(std::size_t limit, std::optional<jb::core::Uuid> after_run_id)
        -> jb::core::Result<std::vector<CapacityRow>, jb::core::Error>;
    [[nodiscard]] auto list_manual_barriers(std::size_t limit, std::optional<jb::core::Uuid> after_run_id)
        -> jb::core::Result<std::vector<ManualBarrier>, jb::core::Error>;
    [[nodiscard]] auto
    list_runnable(jb::core::Uuid const& queue_id, JobType type, jb::core::UtcTimePoint now, std::size_t limit)
        -> jb::core::Result<std::vector<DispatchCandidate>, jb::core::Error>;
    [[nodiscard]] auto earliest_future_runnable(JobType type, jb::core::UtcTimePoint now)
        -> jb::core::Result<std::optional<jb::core::UtcTimePoint>, jb::core::Error>;
    [[nodiscard]] auto find_dispatch_context(jb::core::Uuid const& run_id, jb::core::UtcTimePoint now)
        -> jb::core::Result<std::optional<DispatchContext>, jb::core::Error>;
    [[nodiscard]] auto
    mark_dispatch_running(jb::core::Uuid const& run_id, RunState expected_state, jb::core::UtcTimePoint started_at)
        -> jb::core::Result<bool, jb::core::Error>;
    [[nodiscard]] auto find_completion_context(jb::core::Uuid const& run_id, AttemptNumber attempt_number)
        -> jb::core::Result<CompletionContext, jb::core::Error>;
    [[nodiscard]] auto complete_attempt(jb::core::Uuid const&  run_id,
                                        AttemptNumber          attempt_number,
                                        jb::core::UtcTimePoint completed_at,
                                        AttemptOutcome         outcome,
                                        std::string_view       result_json) -> jb::core::Result<void, jb::core::Error>;
    [[nodiscard]] auto set_run_retry_wait(jb::core::Uuid const& run_id, jb::core::UtcTimePoint runnable_at)
        -> jb::core::Result<void, jb::core::Error>;
    [[nodiscard]] auto set_run_terminal(jb::core::Uuid const&  run_id,
                                        RunState               state,
                                        jb::core::UtcTimePoint completed_at,
                                        std::string_view       result_json) -> jb::core::Result<void, jb::core::Error>;
    [[nodiscard]] auto complete_drained_suspensions(jb::core::Uuid const&  queue_id,
                                                    jb::core::Uuid const&  job_id,
                                                    jb::core::UtcTimePoint updated_at)
        -> jb::core::Result<void, jb::core::Error>;
    [[nodiscard]] auto has_any_running_state() -> jb::core::Result<bool, jb::core::Error>;

private:
    jb::db::Database&        _database;
    AttributeRegistry const& _attributes;
};

} // namespace jb::jobu::detail
