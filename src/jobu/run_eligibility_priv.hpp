#pragma once

#include "job.hpp"
#include "job_lifecycle_priv.hpp"
#include "queue.hpp"
#include "run.hpp"
#include "wait_repository_priv.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace jb::db {
class Database;
}

namespace jb::jobu::detail {

/// Fixed executor capabilities, sampled from the actual executor before telemetry activation.
/// Capacity and fairness do not affect eligibility or this snapshot.
struct AvailableJobTypes {
    bool cli{false};
    bool http{false};

    [[nodiscard]] auto contains(JobType type) const noexcept -> bool;
    auto               operator==(AvailableJobTypes const&) const -> bool = default;
};

/// Shared owner/barrier SQL gate, using the scheduler's table names. Values remain bound by callers.
[[nodiscard]] auto runnable_owner_predicate() noexcept -> std::string_view;

/// Capacity-independent owner policy, also used at the durable scheduler claim boundary.
[[nodiscard]] auto
eligible_owners(RunOrigin origin, bool schedule_owned, JobState job, QueueState queue, bool manual_barrier) noexcept
    -> bool;

struct EligibilityScope {
    enum class Kind : std::uint8_t {
        Job,
        Queue,
        All,
    };
    Kind           kind{Kind::Job};
    jb::core::Uuid id;
};

/// Owning eligibility metadata only; never decodes payloads, output or attribute documents.
struct RunEligibility {
    jb::core::Uuid            id;
    jb::core::Uuid            job_id;
    jb::core::Uuid            queue_id;
    RunState                  state{RunState::Scheduled};
    RunOrigin                 origin{RunOrigin::Scheduled};
    bool                      schedule_owned{true};
    JobType                   type{JobType::Cli};
    jb::core::UtcTimePoint    runnable_at;
    JobState                  job_state{JobState::Active};
    QueueState                queue_state{QueueState::Active};
    NonterminalRunCounts      siblings;
    std::chrono::milliseconds warning_threshold{0};

    [[nodiscard]] auto eligible(jb::core::UtcTimePoint now, AvailableJobTypes types) const noexcept -> bool;
};

/// Returns at most 200 pending/open rows after the exclusive UUID keyset. Includes ineligible rows
/// so suspension closes intervals immediately. No query survives return and no transaction is owned here.
[[nodiscard]] auto
list_eligibility(jb::db::Database& database, EligibilityScope scope, std::optional<jb::core::Uuid> after = std::nullopt)
    -> TelemetryResult<std::vector<RunEligibility>>;

/// Reads only pending/open IDs for the inactive-owner guard. Domain validation remains with
/// the original mutation so this guard does not change embedded cancellation error semantics.
[[nodiscard]] auto list_timing_scope(jb::db::Database&             database,
                                     EligibilityScope              scope,
                                     std::optional<jb::core::Uuid> after = std::nullopt)
    -> TelemetryResult<std::vector<jb::core::Uuid>>;

} // namespace jb::jobu::detail
