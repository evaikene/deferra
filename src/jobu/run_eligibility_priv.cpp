#include "run_eligibility_priv.hpp"

#include "domain_storage_priv.hpp"
#include "query.hpp"
#include "record.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <variant>

namespace jb::jobu::detail {
namespace {

auto invalid_relationship() -> TelemetryFailure
{
    return {
        .error =
            {
                    .category = jb::core::ErrorCategory::Internal,
                    .code     = "jobu.telemetry.invalid_state",
                    .message  = "Persisted eligibility relationship is invalid",
                    },
        .origin = StorageFailureOrigin::PersistedData,
    };
}

auto integer(jb::db::Record const& record, std::string_view name) -> std::int64_t const*
{
    auto const* value = record.value(name);
    return value ? std::get_if<std::int64_t>(value) : nullptr;
}

auto decode(jb::db::Record const& record) -> TelemetryResult<RunEligibility>
{
    auto id        = read_uuid(record, "id");
    auto job_id    = read_uuid(record, "job_id");
    auto queue_id  = read_uuid(record, "queue_id");
    auto job_queue = read_uuid(record, "job_queue_id");

    auto state       = read_run_state(record, "state");
    auto origin      = read_run_origin(record, "origin");
    auto owned       = read_boolean(record, "schedule_owned");
    auto type        = read_job_type(record, "type");
    auto runnable    = read_timestamp(record, "runnable_at_us");
    auto job_state   = read_job_state(record, "job_state");
    auto queue_state = read_queue_state(record, "queue_state");
    auto kind        = read_text(record, "schedule_kind");

    auto const* scheduled     = integer(record, "scheduled_count");
    auto const* manual        = integer(record, "manual_count");
    auto const* live          = integer(record, "live_count");
    auto const* attempts      = integer(record, "attempt_count");
    auto const* retry_history = integer(record, "retry_history_count");
    if (!id || !job_id || !queue_id || !job_queue || !state || !origin || !owned || !type || !runnable || !job_state ||
        !queue_state || !kind || !scheduled || !manual || !live || !attempts || !retry_history || *attempts < 0 ||
        *retry_history < 0 || *scheduled < 0 || *manual < 0 || *live < 0 || (*kind != "once" && *kind != "cron")) {
        return TelemetryResult<RunEligibility>::failure(invalid_relationship());
    }

    NonterminalRunCounts counts{
        .scheduled = static_cast<std::uint64_t>(*scheduled),
        .manual    = static_cast<std::uint64_t>(*manual),
    };
    bool const pending = *state == RunState::Scheduled || *state == RunState::RetryWait;
    // Check relationships even for excluded work. A malformed manual row must not silently
    // disappear behind a suspension, future deadline or unavailable executor.
    if (pending && ((*state == RunState::Scheduled && *attempts != 0) ||
                    (*state == RunState::RetryWait && (*attempts == 0 || *attempts != *retry_history)) ||
                    *queue_id != *job_queue || *job_state == JobState::Deleted || *queue_state == QueueState::Deleted ||
                    counts.scheduled > 1 || counts.manual > 1 ||
                    static_cast<std::uint64_t>(*live) != counts.scheduled + counts.manual ||
                    !valid_nonterminal_run_relationship(*job_state, *kind == "once", counts) ||
                    (*origin == RunOrigin::Scheduled && (!*owned || counts.scheduled != 1)) ||
                    (*origin == RunOrigin::Manual && (*owned || counts.manual != 1)))) {
        return TelemetryResult<RunEligibility>::failure(invalid_relationship());
    }

    return TelemetryResult<RunEligibility>::success({
        .id             = *id,
        .job_id         = *job_id,
        .queue_id       = *queue_id,
        .state          = *state,
        .origin         = *origin,
        .schedule_owned = *owned,
        .type           = *type,
        .runnable_at    = *runnable,
        .job_state      = *job_state,
        .queue_state    = *queue_state,
        .siblings       = counts,
    });
}

} // namespace

auto AvailableJobTypes::contains(JobType type) const noexcept -> bool
{
    return (type == JobType::Cli && cli) || (type == JobType::Http && http);
}

auto runnable_owner_predicate() noexcept -> std::string_view
{
    return "AND jobu_queues.state = 'active' "
           "AND ((jobu_runs.origin = 'scheduled' AND jobu_jobs.state = 'active') "
           "OR (jobu_runs.origin = 'manual' AND jobu_jobs.state IN ('active', 'suspending', 'suspended'))) "
           "AND (jobu_runs.schedule_owned = 0 OR NOT EXISTS ("
           "SELECT 1 FROM jobu_runs AS manual_runs WHERE manual_runs.job_id = jobu_runs.job_id "
           "AND manual_runs.origin = 'manual' AND manual_runs.schedule_owned = 0 "
           "AND manual_runs.state IN ('scheduled', 'running', 'retry_wait')))";
}

auto eligible_owners(RunOrigin  origin,
                     bool       schedule_owned,
                     JobState   job,
                     QueueState queue,
                     bool       manual_barrier) noexcept -> bool
{
    if (queue != QueueState::Active) {
        return false;
    }
    if (origin == RunOrigin::Manual) {
        return !schedule_owned &&
               (job == JobState::Active || job == JobState::Suspending || job == JobState::Suspended);
    }
    return origin == RunOrigin::Scheduled && schedule_owned && job == JobState::Active && !manual_barrier;
}

auto RunEligibility::eligible(jb::core::UtcTimePoint now, AvailableJobTypes types) const noexcept -> bool
{
    return (state == RunState::Scheduled || state == RunState::RetryWait) && runnable_at <= now &&
           types.contains(type) &&
           eligible_owners(origin, schedule_owned, job_state, queue_state, siblings.manual != 0);
}

auto list_eligibility(jb::db::Database& database, EligibilityScope scope, std::optional<jb::core::Uuid> after)
    -> TelemetryResult<std::vector<RunEligibility>>
{
    // Left joins expose missing owners instead of filtering corruption out. The fixed projection
    // and UUID keyset bound memory; traversing an affected scope still costs proportional work.
    std::string sql =
        "SELECT r.id, r.job_id, r.queue_id, r.state, r.origin, r.schedule_owned, r.type, r.runnable_at_us, "
        "j.queue_id AS job_queue_id, j.state AS job_state, j.schedule_kind, q.state AS queue_state, "
        "(SELECT COUNT(*) FROM jobu_runs s WHERE s.job_id = r.job_id AND s.origin = 'scheduled' "
        "AND s.schedule_owned = 1 AND s.state IN ('scheduled', 'running', 'retry_wait')) AS scheduled_count, "
        "(SELECT COUNT(*) FROM jobu_runs m WHERE m.job_id = r.job_id AND m.origin = 'manual' "
        "AND m.schedule_owned = 0 AND m.state IN ('scheduled', 'running', 'retry_wait')) AS manual_count, "
        "(SELECT COUNT(*) FROM jobu_runs l WHERE l.job_id = r.job_id "
        "AND l.state IN ('scheduled', 'running', 'retry_wait')) AS live_count, "
        "(SELECT COUNT(*) FROM jobu_attempts a WHERE a.run_id = r.id) AS attempt_count, "
        "(SELECT COUNT(*) FROM jobu_attempts a WHERE a.run_id = r.id AND a.state = 'completed' "
        "AND a.outcome IN ('failed', 'interrupted')) AS retry_history_count "
        "FROM jobu_runs r LEFT JOIN jobu_jobs j ON j.id = r.job_id "
        "LEFT JOIN jobu_queues q ON q.id = r.queue_id LEFT JOIN jobu_run_timing t ON t.run_id = r.id "
        "WHERE (r.state IN ('scheduled', 'retry_wait') OR t.open_epoch IS NOT NULL) AND ";
    sql += scope.kind == EligibilityScope::Kind::Job ? "r.job_id = :scope " : "r.queue_id = :scope ";
    if (after) {
        sql += "AND r.id > :after ";
    }
    sql += "ORDER BY r.id LIMIT 200";

    jb::db::Query query{database};
    auto          result = query.prepare(sql);
    if (result) {
        result = query.bind_value(":scope", uuid_to_storage(scope.id));
    }
    if (result && after) {
        result = query.bind_value(":after", uuid_to_storage(*after));
    }
    if (result) {
        result = query.exec();
    }
    if (!result) {
        return TelemetryResult<std::vector<RunEligibility>>::failure({.error = std::move(result).error()});
    }

    std::vector<RunEligibility> rows;
    rows.reserve(200);
    for (;;) {
        auto next = query.next();
        if (!next) {
            return TelemetryResult<std::vector<RunEligibility>>::failure({.error = std::move(next).error()});
        }
        if (!*next) {
            break;
        }
        auto row = decode(query.record());
        if (!row) {
            return TelemetryResult<std::vector<RunEligibility>>::failure(std::move(row).error());
        }
        rows.push_back(*row);
    }
    auto finished = query.finish();
    if (!finished) {
        return TelemetryResult<std::vector<RunEligibility>>::failure({.error = std::move(finished).error()});
    }
    return TelemetryResult<std::vector<RunEligibility>>::success(std::move(rows));
}

auto list_timing_scope(jb::db::Database& database, EligibilityScope scope, std::optional<jb::core::Uuid> after)
    -> TelemetryResult<std::vector<jb::core::Uuid>>
{
    std::string sql  = "SELECT r.id FROM jobu_runs r LEFT JOIN jobu_run_timing t ON t.run_id = r.id "
                       "WHERE (r.state IN ('scheduled', 'retry_wait') OR t.open_epoch IS NOT NULL) AND ";
    sql             += scope.kind == EligibilityScope::Kind::Job ? "r.job_id = :scope " : "r.queue_id = :scope ";
    if (after) {
        sql += "AND r.id > :after ";
    }
    sql += "ORDER BY r.id LIMIT 200";

    jb::db::Query query{database};
    auto          result = query.prepare(sql);
    if (result) {
        result = query.bind_value(":scope", uuid_to_storage(scope.id));
    }
    if (result && after) {
        result = query.bind_value(":after", uuid_to_storage(*after));
    }
    if (result) {
        result = query.exec();
    }
    if (!result) {
        return TelemetryResult<std::vector<jb::core::Uuid>>::failure({.error = std::move(result).error()});
    }

    std::vector<jb::core::Uuid> ids;
    ids.reserve(200);
    for (;;) {
        auto next = query.next();
        if (!next) {
            return TelemetryResult<std::vector<jb::core::Uuid>>::failure({.error = std::move(next).error()});
        }
        if (!*next) {
            break;
        }
        auto id = read_uuid(query.record(), "id");
        if (!id) {
            return TelemetryResult<std::vector<jb::core::Uuid>>::failure(invalid_relationship());
        }
        ids.push_back(*id);
    }
    auto finished = query.finish();
    if (!finished) {
        return TelemetryResult<std::vector<jb::core::Uuid>>::failure({.error = std::move(finished).error()});
    }
    return TelemetryResult<std::vector<jb::core::Uuid>>::success(std::move(ids));
}

} // namespace jb::jobu::detail
