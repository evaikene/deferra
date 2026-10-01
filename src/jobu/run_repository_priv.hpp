#pragma once

#include "attribute.hpp"
#include "result.hpp"
#include "run.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace jb::db {
class Database;
}

namespace jb::jobu::detail {

/// Quality assigned in the run's insertion transaction. Complete is reserved for work observed from birth.
/// This selects only the initial closed, zero-wait row; it does not activate telemetry or recover earlier wait.
enum class InitialRunMeasurement : std::uint8_t {
    Unmeasured,
    Complete,
};

/// Non-owning fields for inserting the known initial schedule-owned run.
/// The caller must keep both JSON views alive until insert_schedule_owned() returns.
///
struct ScheduleOwnedRunInsert {
    jb::core::Uuid         id;
    jb::core::Uuid         job_id;
    JobRevision            job_revision{1};
    jb::core::Uuid         queue_id;
    jb::core::UtcTimePoint planned_at;
    jb::core::UtcTimePoint runnable_at;
    JobType                type{JobType::Cli};
    std::int32_t           priority{0};
    std::string_view       attributes_json;
    std::string_view       payload_json;
};

struct RunSnapshot {
    JobRevision            job_revision{1};
    jb::core::Uuid         queue_id;
    jb::core::UtcTimePoint planned_at;
    jb::core::UtcTimePoint runnable_at;
    JobType                type{JobType::Cli};
    std::int32_t           priority{0};
    AttributeSet           attributes;
    jb::core::JsonValue    payload;
};

/// Non-owning serialized fields for refreshing one pending schedule-owned run.
/// The caller must keep both JSON views alive until refresh_unstarted_schedule_owned() returns.
///
struct ScheduleOwnedRunUpdate {
    JobRevision            job_revision{1};
    jb::core::Uuid         queue_id;
    jb::core::UtcTimePoint planned_at;
    jb::core::UtcTimePoint runnable_at;
    JobType                type{JobType::Cli};
    std::int32_t           priority{0};
    std::string_view       attributes_json;
    std::string_view       payload_json;
};

/// Owning continuation for terminal history, ordered by completion time then UUID.
struct TerminalRunKey {
    jb::core::UtcTimePoint completed_at;
    jb::core::Uuid         id;
};

class RunRepository final {
public:
    RunRepository(jb::db::Database& database, AttributeRegistry const& attributes) noexcept;

    /// Insertion methods require a caller-owned transaction and insert both the run and its timing row.
    /// Roll back the entire transaction on failure; no nested transaction or commit is performed here.
    /// Omit measurement when no active telemetry owner covers the run from its creation.
    [[nodiscard]] auto insert_schedule_owned(JobRun const&         run,
                                             InitialRunMeasurement measurement = InitialRunMeasurement::Unmeasured)
        -> jb::core::Result<void, jb::core::Error>;
    [[nodiscard]] auto insert_schedule_owned(ScheduleOwnedRunInsert const& run,
                                             InitialRunMeasurement measurement = InitialRunMeasurement::Unmeasured)
        -> jb::core::Result<void, jb::core::Error>;
    [[nodiscard]] auto insert_manual(JobRun const&         run,
                                     InitialRunMeasurement measurement = InitialRunMeasurement::Unmeasured)
        -> jb::core::Result<void, jb::core::Error>;
    [[nodiscard]] auto find_schedule_owned(jb::core::Uuid const& job_id)
        -> jb::core::Result<std::optional<JobRun>, jb::core::Error>;
    [[nodiscard]] auto find_by_id(jb::core::Uuid const& run_id)
        -> jb::core::Result<std::optional<JobRun>, jb::core::Error>;
    [[nodiscard]] auto has_non_terminal_manual_run(jb::core::Uuid const& job_id)
        -> jb::core::Result<bool, jb::core::Error>;
    [[nodiscard]] auto has_running_or_retrying_run(jb::core::Uuid const& job_id)
        -> jb::core::Result<bool, jb::core::Error>;
    [[nodiscard]] auto refresh_unstarted_schedule_owned(jb::core::Uuid const& job_id, RunSnapshot const& snapshot)
        -> jb::core::Result<bool, jb::core::Error>;
    [[nodiscard]] auto refresh_unstarted_schedule_owned(jb::core::Uuid const&         job_id,
                                                        ScheduleOwnedRunUpdate const& snapshot)
        -> jb::core::Result<bool, jb::core::Error>;
    [[nodiscard]] auto move_non_terminal(jb::core::Uuid const& job_id,
                                         jb::core::Uuid const& target_queue_id,
                                         JobRevision next_revision) -> jb::core::Result<std::size_t, jb::core::Error>;
    [[nodiscard]] auto
    cancel_pending_for_job(jb::core::Uuid const& job_id, jb::core::UtcTimePoint completed_at, std::string_view reason)
        -> jb::core::Result<std::size_t, jb::core::Error>;
    [[nodiscard]] auto cancel_pending_for_queue(jb::core::Uuid const&  queue_id,
                                                jb::core::UtcTimePoint completed_at,
                                                std::string_view       reason)
        -> jb::core::Result<std::size_t, jb::core::Error>;
    [[nodiscard]] auto count_running_for_job(jb::core::Uuid const& job_id)
        -> jb::core::Result<std::uint64_t, jb::core::Error>;
    [[nodiscard]] auto count_running_for_queue(jb::core::Uuid const& queue_id)
        -> jb::core::Result<std::uint64_t, jb::core::Error>;
    /// Reads only IDs and valid completion timestamps; equality with cutoff is retained.
    /// The caller owns the transaction. Limit is 1..1000; after is an exclusive keyset.
    [[nodiscard]] auto list_terminal_before(jb::core::Uuid const&         queue_id,
                                            jb::core::UtcTimePoint        cutoff,
                                            std::size_t                   limit,
                                            std::optional<TerminalRunKey> after = std::nullopt)
        -> jb::core::Result<std::vector<TerminalRunKey>, jb::core::Error>;
    /// Deletes at most 1000 selected IDs, rechecking queue, terminal state and strict cutoff.
    /// The caller must roll back its transaction on failure; cascades are part of that unit.
    [[nodiscard]] auto delete_selected_terminal(jb::core::Uuid const&           queue_id,
                                                jb::core::UtcTimePoint          cutoff,
                                                std::span<jb::core::Uuid const> run_ids)
        -> jb::core::Result<std::size_t, jb::core::Error>;

private:
    jb::db::Database&        _database;
    AttributeRegistry const& _attributes;
};

} // namespace jb::jobu::detail
