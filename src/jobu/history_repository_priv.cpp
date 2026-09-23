#include "history_repository_priv.hpp"

#include "attempt_repository_priv.hpp"
#include "domain_storage_priv.hpp"
#include "history_cursor_priv.hpp"
#include "query.hpp"
#include "run_repository_priv.hpp"
#include "value.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace jb::jobu::detail {

namespace {

template <typename T>
using RepositoryResult = jb::core::Result<T, jb::core::Error>;

constexpr auto run_summary_columns =
    "id AS run_id, job_id AS run_job_id, job_revision AS run_job_revision, queue_id AS run_queue_id, "
    "origin AS run_origin, schedule_owned AS run_schedule_owned, planned_at_us AS run_planned_at_us, "
    "runnable_at_us AS run_runnable_at_us, started_at_us AS run_started_at_us, "
    "completed_at_us AS run_completed_at_us, type AS run_type, priority AS run_priority, state AS run_state";

constexpr auto attempt_summary_columns =
    "run_id AS attempt_run_id, attempt_number AS attempt_number, due_at_us AS attempt_due_at_us, "
    "started_at_us AS attempt_started_at_us, completed_at_us AS attempt_completed_at_us, "
    "state AS attempt_state, outcome AS attempt_outcome";

auto invariant(std::string_view reason) -> jb::core::Error
{
    return {.category = jb::core::ErrorCategory::Internal,
            .code     = "jobu.storage.invariant",
            .message  = "Persisted history violates a JobU invariant",
            .detail   = std::string{reason}};
}

auto invalid_limit() -> jb::core::Error
{
    return {.category = jb::core::ErrorCategory::InvalidArgument,
            .code     = "jobu.storage.invalid_limit",
            .message  = "History repository limit is invalid"};
}

auto read_optional_outcome(jb::db::Record const& record) -> RepositoryResult<std::optional<AttemptOutcome>>
{
    auto const* value = record.value("attempt_outcome");
    if (!value) {
        return RepositoryResult<std::optional<AttemptOutcome>>::failure(invariant("missing_attempt_outcome"));
    }
    if (std::holds_alternative<jb::db::Null>(*value)) {
        return RepositoryResult<std::optional<AttemptOutcome>>::success(std::nullopt);
    }
    auto outcome = read_attempt_outcome(record, "attempt_outcome");
    if (!outcome) {
        return RepositoryResult<std::optional<AttemptOutcome>>::failure(std::move(outcome).error());
    }
    return RepositoryResult<std::optional<AttemptOutcome>>::success(*outcome);
}

auto decode_run_summary(jb::db::Record const& record) -> RepositoryResult<RunSummary>
{
    auto id = read_uuid(record, "run_id");
    if (!id) {
        return RepositoryResult<RunSummary>::failure(std::move(id).error());
    }
    auto job_id = read_uuid(record, "run_job_id");
    if (!job_id) {
        return RepositoryResult<RunSummary>::failure(std::move(job_id).error());
    }
    auto revision = read_revision(record, "run_job_revision");
    if (!revision) {
        return RepositoryResult<RunSummary>::failure(std::move(revision).error());
    }
    auto queue_id = read_uuid(record, "run_queue_id");
    if (!queue_id) {
        return RepositoryResult<RunSummary>::failure(std::move(queue_id).error());
    }
    auto origin = read_run_origin(record, "run_origin");
    if (!origin) {
        return RepositoryResult<RunSummary>::failure(std::move(origin).error());
    }
    auto schedule_owned = read_boolean(record, "run_schedule_owned");
    if (!schedule_owned) {
        return RepositoryResult<RunSummary>::failure(std::move(schedule_owned).error());
    }
    auto planned = read_timestamp(record, "run_planned_at_us");
    if (!planned) {
        return RepositoryResult<RunSummary>::failure(std::move(planned).error());
    }
    auto runnable = read_timestamp(record, "run_runnable_at_us");
    if (!runnable) {
        return RepositoryResult<RunSummary>::failure(std::move(runnable).error());
    }
    auto started = read_optional_timestamp(record, "run_started_at_us");
    if (!started) {
        return RepositoryResult<RunSummary>::failure(std::move(started).error());
    }
    auto completed = read_optional_timestamp(record, "run_completed_at_us");
    if (!completed) {
        return RepositoryResult<RunSummary>::failure(std::move(completed).error());
    }
    auto type = read_job_type(record, "run_type");
    if (!type) {
        return RepositoryResult<RunSummary>::failure(std::move(type).error());
    }
    auto priority = read_int32(record, "run_priority");
    if (!priority) {
        return RepositoryResult<RunSummary>::failure(std::move(priority).error());
    }
    auto state = read_run_state(record, "run_state");
    if (!state) {
        return RepositoryResult<RunSummary>::failure(std::move(state).error());
    }

    // The summary projection still checks lifecycle relationships before publishing a durable row.
    auto const terminal         = *state == RunState::Succeeded || *state == RunState::Failed ||
                                *state == RunState::Interrupted || *state == RunState::Cancelled;
    auto const started_required = *state != RunState::Scheduled && *state != RunState::Cancelled;
    if (*origin == RunOrigin::Submitted || (*schedule_owned && *origin != RunOrigin::Scheduled) ||
        terminal != completed->has_value() || (started_required && !started->has_value()) ||
        (*state == RunState::Scheduled && started->has_value())) {
        return RepositoryResult<RunSummary>::failure(invariant("run_state_fields"));
    }

    return RepositoryResult<RunSummary>::success(RunSummary{
        .id             = *id,
        .job_id         = *job_id,
        .queue_id       = *queue_id,
        .job_revision   = *revision,
        .origin         = *origin,
        .type           = *type,
        .state          = *state,
        .schedule_owned = *schedule_owned,
        .priority       = *priority,
        .planned_at     = *planned,
        .runnable_at    = *runnable,
        .started_at     = *started,
        .completed_at   = *completed,
    });
}

auto decode_attempt_summary(jb::db::Record const& record) -> RepositoryResult<AttemptSummary>
{
    auto run_id = read_uuid(record, "attempt_run_id");
    if (!run_id) {
        return RepositoryResult<AttemptSummary>::failure(std::move(run_id).error());
    }
    auto number = read_attempt_number(record, "attempt_number");
    if (!number) {
        return RepositoryResult<AttemptSummary>::failure(std::move(number).error());
    }
    auto due = read_timestamp(record, "attempt_due_at_us");
    if (!due) {
        return RepositoryResult<AttemptSummary>::failure(std::move(due).error());
    }
    auto started = read_optional_timestamp(record, "attempt_started_at_us");
    if (!started) {
        return RepositoryResult<AttemptSummary>::failure(std::move(started).error());
    }
    auto completed = read_optional_timestamp(record, "attempt_completed_at_us");
    if (!completed) {
        return RepositoryResult<AttemptSummary>::failure(std::move(completed).error());
    }
    auto state = read_attempt_state(record, "attempt_state");
    if (!state) {
        return RepositoryResult<AttemptSummary>::failure(std::move(state).error());
    }
    auto outcome = read_optional_outcome(record);
    if (!outcome) {
        return RepositoryResult<AttemptSummary>::failure(std::move(outcome).error());
    }

    auto const pending =
        *state == AttemptState::Pending && !started->has_value() && !completed->has_value() && !outcome->has_value();
    auto const running =
        *state == AttemptState::Running && started->has_value() && !completed->has_value() && !outcome->has_value();
    auto const complete = *state == AttemptState::Completed && completed->has_value() && outcome->has_value() &&
                          (started->has_value() || **outcome == AttemptOutcome::Cancelled);
    if (!pending && !running && !complete) {
        return RepositoryResult<AttemptSummary>::failure(invariant("attempt_state_fields"));
    }

    return RepositoryResult<AttemptSummary>::success(AttemptSummary{
        .run_id         = *run_id,
        .attempt_number = *number,
        .due_at         = *due,
        .started_at     = *started,
        .completed_at   = *completed,
        .state          = *state,
        .outcome        = *outcome,
    });
}

auto summary_of(JobRun const& run) -> RunSummary
{
    return {.id             = run.id,
            .job_id         = run.job_id,
            .queue_id       = run.queue_id,
            .job_revision   = run.job_revision,
            .origin         = run.origin,
            .type           = run.type,
            .state          = run.state,
            .schedule_owned = run.schedule_owned,
            .priority       = run.priority,
            .planned_at     = run.planned_at,
            .runnable_at    = run.runnable_at,
            .started_at     = run.started_at,
            .completed_at   = run.completed_at};
}

auto summary_of(JobAttempt const& attempt) -> AttemptSummary
{
    return {.run_id         = attempt.run_id,
            .attempt_number = attempt.attempt_number,
            .due_at         = attempt.due_at,
            .started_at     = attempt.started_at,
            .completed_at   = attempt.completed_at,
            .state          = attempt.state,
            .outcome        = attempt.outcome};
}

using Bindings = std::vector<std::pair<std::string, jb::db::Value>>;

auto add_range(std::string&     sql,
               Bindings&        bindings,
               std::string_view column,
               std::string_view name,
               UtcRange const&  range) -> RepositoryResult<void>
{
    if (range.from) {
        auto value = timestamp_to_storage(*range.from);
        if (!value) {
            return RepositoryResult<void>::failure(std::move(value).error());
        }
        auto key  = ":" + std::string{name} + "_from";
        sql      += " AND " + std::string{column} + " >= " + key;
        bindings.emplace_back(std::move(key), std::move(value).value());
    }
    if (range.to) {
        auto value = timestamp_to_storage(*range.to);
        if (!value) {
            return RepositoryResult<void>::failure(std::move(value).error());
        }
        auto key  = ":" + std::string{name} + "_to";
        sql      += " AND " + std::string{column} + " < " + key;
        bindings.emplace_back(std::move(key), std::move(value).value());
    }
    return RepositoryResult<void>::success();
}

auto bind_all(jb::db::Query& query, Bindings& bindings) -> RepositoryResult<void>
{
    for (auto& [name, value] : bindings) {
        auto bound = query.bind_value(name, std::move(value));
        if (!bound) {
            return RepositoryResult<void>::failure(std::move(bound).error());
        }
    }
    return RepositoryResult<void>::success();
}

} // namespace

HistoryRepository::HistoryRepository(jb::db::Database& database, AttributeRegistry const& attributes) noexcept
    : _database{database}
    , _attributes{attributes}
{}

auto HistoryRepository::get_run(jb::core::Uuid const& id) -> RepositoryResult<std::optional<RunDetails>>
{
    auto repository = RunRepository{_database, _attributes};
    auto found      = repository.find_by_id(id);
    if (!found) {
        return RepositoryResult<std::optional<RunDetails>>::failure(std::move(found).error());
    }
    if (!found->has_value()) {
        return RepositoryResult<std::optional<RunDetails>>::success(std::nullopt);
    }

    auto run = std::move(**found);
    if (run.origin == RunOrigin::Submitted) {
        return RepositoryResult<std::optional<RunDetails>>::failure(invariant("reserved_run_origin"));
    }
    auto details                      = RunDetails{};
    static_cast<RunSummary&>(details) = summary_of(run);
    details.attributes                = std::move(run.attributes);
    details.payload                   = std::move(run.payload);
    details.result                    = std::move(run.result);
    return RepositoryResult<std::optional<RunDetails>>::success(std::move(details));
}

auto HistoryRepository::get_attempt(AttemptKey const& key) -> RepositoryResult<std::optional<AttemptDetails>>
{
    auto repository = AttemptRepository{_database};
    auto found      = repository.find(key.run_id, key.attempt_number);
    if (!found) {
        return RepositoryResult<std::optional<AttemptDetails>>::failure(std::move(found).error());
    }
    if (!found->has_value()) {
        return RepositoryResult<std::optional<AttemptDetails>>::success(std::nullopt);
    }

    auto attempt                          = std::move(**found);
    auto details                          = AttemptDetails{};
    static_cast<AttemptSummary&>(details) = summary_of(attempt);
    details.result                        = std::move(attempt.result);
    return RepositoryResult<std::optional<AttemptDetails>>::success(std::move(details));
}

auto HistoryRepository::list_runs(RunQuery const& query, std::optional<RunCursorKey> after, std::size_t limit)
    -> RepositoryResult<std::vector<RunSummary>>
{
    if (limit < 1 || limit > 201) {
        return RepositoryResult<std::vector<RunSummary>>::failure(invalid_limit());
    }

    // All optional predicates are bound; the parenthesized keyset boundary follows the v2 index order.
    auto        sql      = std::string{"SELECT "} + run_summary_columns + " FROM jobu_runs WHERE 1 = 1";
    auto        bindings = Bindings{};
    auto const& filters  = query.filters;
    if (filters.queue_id) {
        sql += " AND queue_id = :queue_id";
        bindings.emplace_back(":queue_id", uuid_to_storage(*filters.queue_id));
    }
    if (filters.job_id) {
        sql += " AND job_id = :job_id";
        bindings.emplace_back(":job_id", uuid_to_storage(*filters.job_id));
    }
    if (filters.state) {
        sql += " AND state = :state";
        bindings.emplace_back(":state", jb::db::make_text(storage_text(*filters.state)));
    }
    if (filters.origin) {
        sql += " AND origin = :origin";
        bindings.emplace_back(":origin", jb::db::make_text(storage_text(*filters.origin)));
    }
    if (filters.type) {
        sql += " AND type = :type";
        bindings.emplace_back(":type", jb::db::make_text(storage_text(*filters.type)));
    }
    auto planned_range = add_range(sql, bindings, "planned_at_us", "planned", filters.planned);
    if (!planned_range) {
        return RepositoryResult<std::vector<RunSummary>>::failure(std::move(planned_range).error());
    }
    auto started_range = add_range(sql, bindings, "started_at_us", "started", filters.started);
    if (!started_range) {
        return RepositoryResult<std::vector<RunSummary>>::failure(std::move(started_range).error());
    }
    auto completed_range = add_range(sql, bindings, "completed_at_us", "completed", filters.completed);
    if (!completed_range) {
        return RepositoryResult<std::vector<RunSummary>>::failure(std::move(completed_range).error());
    }
    if (after) {
        auto planned = timestamp_to_storage(after->planned_at);
        if (!planned) {
            return RepositoryResult<std::vector<RunSummary>>::failure(std::move(planned).error());
        }
        sql += " AND (planned_at_us < :after_planned OR (planned_at_us = :after_planned AND id < :after_id))";
        bindings.emplace_back(":after_planned", std::move(planned).value());
        bindings.emplace_back(":after_id", uuid_to_storage(after->id));
    }
    sql += " ORDER BY planned_at_us DESC, id DESC LIMIT :limit";
    bindings.emplace_back(":limit", static_cast<std::int64_t>(limit));

    auto query_handle = jb::db::Query{_database};
    auto prepared     = query_handle.prepare(sql);
    if (!prepared) {
        return RepositoryResult<std::vector<RunSummary>>::failure(std::move(prepared).error());
    }
    auto bound = bind_all(query_handle, bindings);
    if (!bound) {
        return RepositoryResult<std::vector<RunSummary>>::failure(std::move(bound).error());
    }
    auto executed = query_handle.exec();
    if (!executed) {
        return RepositoryResult<std::vector<RunSummary>>::failure(std::move(executed).error());
    }

    auto rows = std::vector<RunSummary>{};
    rows.reserve(limit);
    while (true) {
        auto next = query_handle.next();
        if (!next) {
            return RepositoryResult<std::vector<RunSummary>>::failure(std::move(next).error());
        }
        if (!*next) {
            break;
        }
        auto decoded = decode_run_summary(query_handle.record());
        if (!decoded) {
            return RepositoryResult<std::vector<RunSummary>>::failure(std::move(decoded).error());
        }
        rows.push_back(std::move(decoded).value());
    }
    return RepositoryResult<std::vector<RunSummary>>::success(std::move(rows));
}

auto HistoryRepository::list_attempts(AttemptQuery const&          query,
                                      std::optional<AttemptNumber> before_number,
                                      std::size_t limit) -> RepositoryResult<std::vector<AttemptSummary>>
{
    if (limit < 1 || limit > 201) {
        return RepositoryResult<std::vector<AttemptSummary>>::failure(invalid_limit());
    }

    auto sql      = std::string{"SELECT "} + attempt_summary_columns + " FROM jobu_attempts WHERE run_id = :run_id";
    auto bindings = Bindings{
        {":run_id", uuid_to_storage(query.run_id)}
    };
    if (before_number) {
        auto number = attempt_number_to_storage(*before_number);
        if (!number) {
            return RepositoryResult<std::vector<AttemptSummary>>::failure(std::move(number).error());
        }
        sql += " AND attempt_number < :before_number";
        bindings.emplace_back(":before_number", std::move(number).value());
    }
    sql += " ORDER BY attempt_number DESC LIMIT :limit";
    bindings.emplace_back(":limit", static_cast<std::int64_t>(limit));

    auto query_handle = jb::db::Query{_database};
    auto prepared     = query_handle.prepare(sql);
    if (!prepared) {
        return RepositoryResult<std::vector<AttemptSummary>>::failure(std::move(prepared).error());
    }
    auto bound = bind_all(query_handle, bindings);
    if (!bound) {
        return RepositoryResult<std::vector<AttemptSummary>>::failure(std::move(bound).error());
    }
    auto executed = query_handle.exec();
    if (!executed) {
        return RepositoryResult<std::vector<AttemptSummary>>::failure(std::move(executed).error());
    }

    auto rows = std::vector<AttemptSummary>{};
    rows.reserve(limit);
    while (true) {
        auto next = query_handle.next();
        if (!next) {
            return RepositoryResult<std::vector<AttemptSummary>>::failure(std::move(next).error());
        }
        if (!*next) {
            break;
        }
        auto decoded = decode_attempt_summary(query_handle.record());
        if (!decoded) {
            return RepositoryResult<std::vector<AttemptSummary>>::failure(std::move(decoded).error());
        }
        rows.push_back(std::move(decoded).value());
    }
    return RepositoryResult<std::vector<AttemptSummary>>::success(std::move(rows));
}

} // namespace jb::jobu::detail
