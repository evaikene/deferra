#include "job_lifecycle_priv.hpp"

#include "domain_storage_priv.hpp"
#include "job_repository_priv.hpp"
#include "query.hpp"
#include "run_repository_priv.hpp"

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace jb::jobu::detail {

namespace {

template <typename T>
using LifecycleResult = jb::core::Result<T, jb::core::Error>;

constexpr JobRevision kMaximumPersistedJobRevision = static_cast<JobRevision>(std::numeric_limits<std::int64_t>::max());

auto invariant(std::string_view reason) -> jb::core::Error
{
    return {.category = jb::core::ErrorCategory::Internal,
            .code     = "jobu.storage.invariant",
            .message  = "Persisted job lifecycle violates a JobU invariant",
            .detail   = "reason=" + std::string{reason}};
}

auto revision_exhausted() -> jb::core::Error
{
    return {.category = jb::core::ErrorCategory::ResourceExhausted,
            .code     = "jobu.job.revision_exhausted",
            .message  = "Job revision cannot be incremented"};
}

auto terminal_job_state(RunState run_state) -> std::optional<JobState>
{
    switch (run_state) {
        case RunState::Succeeded:
            return JobState::Succeeded;
        case RunState::Failed:
        case RunState::Interrupted:
            return JobState::Failed;
        case RunState::Cancelled:
            return JobState::Cancelled;
        default:
            return std::nullopt;
    }
}

auto read_nonterminal_runs(jb::db::Database& database, JobDefinition const& job)
    -> LifecycleResult<NonterminalRunCounts>
{
    // Two live rows are the largest valid relationship. A third proves corruption without
    // loading unbounded histories or any snapshotted payloads.
    jb::db::Query query{database};
    auto prepared = query.prepare("SELECT origin AS live_origin, schedule_owned AS live_schedule_owned, "
                                  "queue_id AS live_queue_id, job_revision AS live_job_revision FROM jobu_runs "
                                  "WHERE job_id = :job_id AND state IN ('scheduled', 'running', 'retry_wait') LIMIT 3");
    if (!prepared) {
        return LifecycleResult<NonterminalRunCounts>::failure(std::move(prepared).error());
    }
    auto bound = query.bind_value(":job_id", uuid_to_storage(job.id));
    if (!bound) {
        return LifecycleResult<NonterminalRunCounts>::failure(std::move(bound).error());
    }
    auto executed = query.exec();
    if (!executed) {
        return LifecycleResult<NonterminalRunCounts>::failure(std::move(executed).error());
    }

    auto counts = NonterminalRunCounts{};
    while (true) {
        auto next = query.next();
        if (!next) {
            return LifecycleResult<NonterminalRunCounts>::failure(std::move(next).error());
        }
        if (!*next) {
            break;
        }
        auto origin   = read_run_origin(query.record(), "live_origin");
        auto owned    = read_boolean(query.record(), "live_schedule_owned");
        auto queue_id = read_uuid(query.record(), "live_queue_id");
        auto revision = read_revision(query.record(), "live_job_revision");
        if (!origin || !owned || !queue_id || !revision || *queue_id != job.queue_id || *revision > job.revision) {
            return LifecycleResult<NonterminalRunCounts>::failure(invariant("nonterminal_run_ownership"));
        }

        if (*origin == RunOrigin::Scheduled && *owned) {
            ++counts.scheduled;
        }
        else if (*origin == RunOrigin::Manual && !*owned) {
            ++counts.manual;
        }
        else {
            return LifecycleResult<NonterminalRunCounts>::failure(invariant("nonterminal_run_origin"));
        }
        if (counts.scheduled + counts.manual > 2) {
            return LifecycleResult<NonterminalRunCounts>::failure(invariant("nonterminal_run_count"));
        }
    }
    return LifecycleResult<NonterminalRunCounts>::success(counts);
}

} // namespace

JobLifecycleRepository::JobLifecycleRepository(jb::db::Database& database, AttributeRegistry const& attributes) noexcept
    : _database{database}
    , _attributes{attributes}
{}

auto JobLifecycleRepository::finish_after_terminal_run(jb::core::Uuid const&  run_id,
                                                       jb::core::UtcTimePoint transition_time)
    -> jb::core::Result<bool, jb::core::Error>
{
    RunRepository runs{_database, _attributes};
    auto          found = runs.find_by_id(run_id);
    if (!found) {
        return LifecycleResult<bool>::failure(std::move(found).error());
    }
    if (!*found) {
        return LifecycleResult<bool>::failure(invariant("missing_terminal_run"));
    }
    auto const& run        = **found;
    auto        next_state = terminal_job_state(run.state);
    if (!run.completed_at || !run.result || !next_state || run.origin == RunOrigin::Submitted ||
        run.schedule_owned != (run.origin == RunOrigin::Scheduled)) {
        return LifecycleResult<bool>::failure(invariant("invalid_terminal_run"));
    }

    JobRepository jobs{_database, _attributes};
    auto          owner = jobs.find_by_id(run.job_id, true);
    if (!owner) {
        return LifecycleResult<bool>::failure(std::move(owner).error());
    }
    if (!*owner || run.job_revision > (*owner)->revision) {
        return LifecycleResult<bool>::failure(invariant("invalid_terminal_owner"));
    }
    auto const& job = **owner;
    if (job.state == JobState::Deleted || std::holds_alternative<CronSchedule>(job.schedule)) {
        return LifecycleResult<bool>::success(false);
    }

    auto counts = read_nonterminal_runs(_database, job);
    if (!counts) {
        return LifecycleResult<bool>::failure(std::move(counts).error());
    }
    if (!valid_nonterminal_run_relationship(job.state, true, *counts)) {
        return LifecycleResult<bool>::failure(invariant("nonterminal_run_relationship"));
    }
    if (is_terminal_job_state(job.state)) {
        return LifecycleResult<bool>::success(false);
    }
    if (counts->scheduled != 0 || counts->manual != 0) {
        return LifecycleResult<bool>::success(false);
    }

    // The final run and this revision-checked definition transition belong to one caller-owned
    // transaction. A lost compare-and-update is an invariant failure, not a successful no-op.
    if (job.revision >= kMaximumPersistedJobRevision) {
        return LifecycleResult<bool>::failure(revision_exhausted());
    }
    auto transitioned = jobs.set_state(job.id, job.state, *next_state, job.revision, job.revision + 1, transition_time);
    if (!transitioned) {
        return LifecycleResult<bool>::failure(std::move(transitioned).error());
    }
    if (!*transitioned) {
        return LifecycleResult<bool>::failure(invariant("terminal_job_state_changed"));
    }
    return LifecycleResult<bool>::success(true);
}

auto JobLifecycleRepository::validate_job_lifecycle(JobDefinition const& job) -> jb::core::Result<void, jb::core::Error>
{
    if (storage_text(job.state).empty()) {
        return LifecycleResult<void>::failure(invariant("invalid_job_state"));
    }
    auto counts = read_nonterminal_runs(_database, job);
    if (!counts) {
        return LifecycleResult<void>::failure(std::move(counts).error());
    }
    auto const one_time = std::holds_alternative<OnceSchedule>(job.schedule);
    if (!valid_nonterminal_run_relationship(job.state, one_time, *counts)) {
        return LifecycleResult<void>::failure(invariant("nonterminal_run_relationship"));
    }
    if (job.state != JobState::Deleted && one_time && !is_terminal_job_state(job.state) && counts->scheduled == 0 &&
        counts->manual == 0) {
        return LifecycleResult<void>::failure(invariant("unfinished_once_without_work"));
    }
    return LifecycleResult<void>::success();
}

} // namespace jb::jobu::detail
