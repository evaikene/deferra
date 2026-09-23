#include "statistics_repository_priv.hpp"

#include "domain_storage_priv.hpp"
#include "query.hpp"
#include "value.hpp"

#include <fmt/format.h>

#include <chrono>
#include <cmath>
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
using RepositoryResult = jb::core::Result<T, jb::core::Error>;

using Bindings = std::vector<std::pair<std::string, jb::db::Value>>;

auto invariant(std::string_view reason) -> jb::core::Error
{
    return {.category = jb::core::ErrorCategory::Internal,
            .code     = "jobu.storage.invariant",
            .message  = "Persisted statistics data is invalid",
            .detail   = std::string{reason}};
}

auto group_column(StatisticsGroupBy group_by) -> std::string_view
{
    switch (group_by) {
        case StatisticsGroupBy::Queue:
            return "r.queue_id";
        case StatisticsGroupBy::Job:
            return "r.job_id";
        case StatisticsGroupBy::Type:
            return "r.type";
        case StatisticsGroupBy::Origin:
            return "r.origin";
        case StatisticsGroupBy::State:
            return "r.state";
        case StatisticsGroupBy::None:
            break;
    }
    return {};
}

auto key_value(StatisticsGroupBy group_by, StatisticsGroupKey const& key) -> RepositoryResult<jb::db::Value>
{
    if (group_by == StatisticsGroupBy::Queue || group_by == StatisticsGroupBy::Job) {
        if (auto const* id = std::get_if<jb::core::Uuid>(&key)) {
            return RepositoryResult<jb::db::Value>::success(uuid_to_storage(*id));
        }
    }
    else if (group_by == StatisticsGroupBy::Type) {
        if (auto const* type = std::get_if<JobType>(&key)) {
            return RepositoryResult<jb::db::Value>::success(jb::db::make_text(storage_text(*type)));
        }
    }
    else if (group_by == StatisticsGroupBy::Origin) {
        if (auto const* origin = std::get_if<RunOrigin>(&key); origin && *origin != RunOrigin::Submitted) {
            return RepositoryResult<jb::db::Value>::success(jb::db::make_text(storage_text(*origin)));
        }
    }
    else if (group_by == StatisticsGroupBy::State) {
        if (auto const* state = std::get_if<RunState>(&key)) {
            return RepositoryResult<jb::db::Value>::success(jb::db::make_text(storage_text(*state)));
        }
    }
    return RepositoryResult<jb::db::Value>::failure(invariant("invalid_group_key"));
}

auto read_group_key(jb::db::Record const& record, StatisticsGroupBy group_by) -> RepositoryResult<StatisticsGroupKey>
{
    if (group_by == StatisticsGroupBy::Queue || group_by == StatisticsGroupBy::Job) {
        auto id = read_uuid(record, "group_key");
        if (!id) {
            return RepositoryResult<StatisticsGroupKey>::failure(std::move(id).error());
        }
        return RepositoryResult<StatisticsGroupKey>::success(*id);
    }
    if (group_by == StatisticsGroupBy::Type) {
        auto type = read_job_type(record, "group_key");
        if (!type) {
            return RepositoryResult<StatisticsGroupKey>::failure(std::move(type).error());
        }
        return RepositoryResult<StatisticsGroupKey>::success(*type);
    }
    if (group_by == StatisticsGroupBy::Origin) {
        auto origin = read_run_origin(record, "group_key");
        if (!origin) {
            return RepositoryResult<StatisticsGroupKey>::failure(std::move(origin).error());
        }
        if (*origin == RunOrigin::Submitted) {
            return RepositoryResult<StatisticsGroupKey>::failure(invariant("reserved_origin"));
        }
        return RepositoryResult<StatisticsGroupKey>::success(*origin);
    }
    auto state = read_run_state(record, "group_key");
    if (!state) {
        return RepositoryResult<StatisticsGroupKey>::failure(std::move(state).error());
    }
    return RepositoryResult<StatisticsGroupKey>::success(*state);
}

auto add_filters(std::string& sql, Bindings& bindings, StatisticsRequest const& request) -> RepositoryResult<void>
{
    auto from = timestamp_to_storage(*request.planned.from);
    auto to   = timestamp_to_storage(*request.planned.to);
    if (!from) {
        return RepositoryResult<void>::failure(std::move(from).error());
    }
    if (!to) {
        return RepositoryResult<void>::failure(std::move(to).error());
    }
    sql += " AND r.planned_at_us >= :planned_from AND r.planned_at_us < :planned_to";
    bindings.emplace_back(":planned_from", std::move(from).value());
    bindings.emplace_back(":planned_to", std::move(to).value());

    if (request.queue_id) {
        sql += " AND r.queue_id = :queue_id";
        bindings.emplace_back(":queue_id", uuid_to_storage(*request.queue_id));
    }
    if (request.job_id) {
        sql += " AND r.job_id = :job_id";
        bindings.emplace_back(":job_id", uuid_to_storage(*request.job_id));
    }
    if (request.type) {
        sql += " AND r.type = :type";
        bindings.emplace_back(":type", jb::db::make_text(storage_text(*request.type)));
    }
    if (request.origin) {
        sql += " AND r.origin = :origin";
        bindings.emplace_back(":origin", jb::db::make_text(storage_text(*request.origin)));
    }
    return RepositoryResult<void>::success();
}

auto add_group_filter(std::string& sql, Bindings& bindings, StatisticsGroupBy group_by, StatisticsGroupKey const& key)
    -> RepositoryResult<void>
{
    if (group_by == StatisticsGroupBy::None) {
        return RepositoryResult<void>::success();
    }
    auto value = key_value(group_by, key);
    if (!value) {
        return RepositoryResult<void>::failure(std::move(value).error());
    }
    sql += fmt::format(" AND {} = :group_key", group_column(group_by));
    bindings.emplace_back(":group_key", std::move(value).value());
    return RepositoryResult<void>::success();
}

template <typename Consume>
auto scan(jb::db::Database& database, std::string const& sql, Bindings& bindings, Consume&& consume)
    -> RepositoryResult<void>
{
    auto query = jb::db::Query{database};
    if (auto prepared = query.prepare(sql); !prepared) {
        return RepositoryResult<void>::failure(std::move(prepared).error());
    }
    for (auto& [name, value] : bindings) {
        if (auto bound = query.bind_value(name, std::move(value)); !bound) {
            return RepositoryResult<void>::failure(std::move(bound).error());
        }
    }
    if (auto executed = query.exec(); !executed) {
        return RepositoryResult<void>::failure(std::move(executed).error());
    }
    while (true) {
        auto next = query.next();
        if (!next) {
            return RepositoryResult<void>::failure(std::move(next).error());
        }
        if (!*next) {
            return RepositoryResult<void>::success();
        }
        if (auto consumed = consume(query.record()); !consumed) {
            return RepositoryResult<void>::failure(std::move(consumed).error());
        }
    }
}

auto increment(std::uint64_t& count) -> RepositoryResult<void>
{
    if (count == std::numeric_limits<std::uint64_t>::max()) {
        return RepositoryResult<void>::failure(invariant("count_overflow"));
    }
    ++count;
    return RepositoryResult<void>::success();
}

struct DurationAccumulator {
    StatisticsDuration result;
    long double        sum_ms{0};

    auto observe(jb::core::UtcTimePoint start, jb::core::UtcTimePoint end) -> RepositoryResult<void>
    {
        auto const start_us = std::chrono::duration_cast<std::chrono::microseconds>(start.time_since_epoch()).count();
        auto const end_us   = std::chrono::duration_cast<std::chrono::microseconds>(end.time_since_epoch()).count();
        if (end_us < start_us) {
            return RepositoryResult<void>::success();
        }
        // Unsigned subtraction represents the full positive difference even across the signed epoch boundary.
        auto const elapsed_us = static_cast<std::uint64_t>(end_us) - static_cast<std::uint64_t>(start_us);
        auto const sample_ms  = static_cast<long double>(elapsed_us) / 1000.0L;
        if (!std::isfinite(sample_ms)) {
            return RepositoryResult<void>::success();
        }
        if (auto counted = increment(result.samples); !counted) {
            return counted;
        }
        sum_ms            += sample_ms;
        auto const sample  = static_cast<double>(sample_ms);
        if (!result.maximum || sample > *result.maximum) {
            result.maximum = sample;
        }
        return RepositoryResult<void>::success();
    }

    auto finish() -> StatisticsDuration
    {
        if (result.samples != 0) {
            result.average = static_cast<double>(sum_ms / static_cast<long double>(result.samples));
        }
        return result;
    }
};

auto count_run(jb::db::Record const& record, StatisticsGroup& group, DurationAccumulator& lateness)
    -> RepositoryResult<void>
{
    auto state     = read_run_state(record, "run_state");
    auto type      = read_job_type(record, "run_type");
    auto origin    = read_run_origin(record, "run_origin");
    auto planned   = read_timestamp(record, "planned_at_us");
    auto started   = read_optional_timestamp(record, "started_at_us");
    auto completed = read_optional_timestamp(record, "completed_at_us");
    if (!state || !type || !origin || !planned || !started || !completed) {
        return RepositoryResult<void>::failure(invariant("run_projection"));
    }
    auto const terminal         = *state == RunState::Succeeded || *state == RunState::Failed ||
                                *state == RunState::Interrupted || *state == RunState::Cancelled;
    auto const started_required = *state != RunState::Scheduled && *state != RunState::Cancelled;
    if (*origin == RunOrigin::Submitted || terminal != completed->has_value() ||
        (started_required && !started->has_value()) || (*state == RunState::Scheduled && started->has_value())) {
        return RepositoryResult<void>::failure(invariant("run_state_fields"));
    }
    auto& counts      = group.runs;
    auto* state_count = &counts.scheduled;
    switch (*state) {
        case RunState::Scheduled:
            break;
        case RunState::Running:
            state_count = &counts.running;
            break;
        case RunState::RetryWait:
            state_count = &counts.retry_wait;
            break;
        case RunState::Succeeded:
            state_count = &counts.succeeded;
            break;
        case RunState::Failed:
            state_count = &counts.failed;
            break;
        case RunState::Interrupted:
            state_count = &counts.interrupted;
            break;
        case RunState::Cancelled:
            state_count = &counts.cancelled;
            break;
    }
    if (auto counted = increment(counts.total); !counted) {
        return counted;
    }
    if (auto counted = increment(*state_count); !counted) {
        return counted;
    }
    if (auto counted = increment(*type == JobType::Cli ? counts.cli : counts.http); !counted) {
        return counted;
    }
    if (auto counted = increment(*origin == RunOrigin::Scheduled ? counts.scheduled_origin : counts.manual_origin);
        !counted) {
        return counted;
    }
    if (started->has_value()) {
        return lateness.observe(*planned, **started);
    }
    return RepositoryResult<void>::success();
}

auto count_attempt(jb::db::Record const& record, StatisticsGroup& group, DurationAccumulator& execution)
    -> RepositoryResult<void>
{
    auto number    = read_attempt_number(record, "attempt_number");
    auto state     = read_attempt_state(record, "attempt_state");
    auto started   = read_optional_timestamp(record, "started_at_us");
    auto completed = read_optional_timestamp(record, "completed_at_us");
    if (!number || !state || !started || !completed) {
        return RepositoryResult<void>::failure(invariant("attempt_projection"));
    }
    auto const* outcome_value = record.value("attempt_outcome");
    if (!outcome_value) {
        return RepositoryResult<void>::failure(invariant("missing_outcome"));
    }
    auto outcome = std::optional<AttemptOutcome>{};
    if (!std::holds_alternative<jb::db::Null>(*outcome_value)) {
        auto decoded = read_attempt_outcome(record, "attempt_outcome");
        if (!decoded) {
            return RepositoryResult<void>::failure(std::move(decoded).error());
        }
        outcome = *decoded;
    }
    auto const pending =
        *state == AttemptState::Pending && !started->has_value() && !completed->has_value() && !outcome.has_value();
    auto const running =
        *state == AttemptState::Running && started->has_value() && !completed->has_value() && !outcome.has_value();
    auto const complete = *state == AttemptState::Completed && completed->has_value() && outcome.has_value() &&
                          (started->has_value() || *outcome == AttemptOutcome::Cancelled);
    if (!pending && !running && !complete) {
        return RepositoryResult<void>::failure(invariant("attempt_outcome_state"));
    }

    auto& counts = group.attempts;
    if (auto counted = increment(counts.total); !counted) {
        return counted;
    }
    switch (*state) {
        case AttemptState::Pending:
            if (auto counted = increment(counts.pending); !counted) {
                return counted;
            }
            break;
        case AttemptState::Running:
            if (auto counted = increment(counts.running); !counted) {
                return counted;
            }
            break;
        case AttemptState::Completed:
            if (auto counted = increment(counts.completed); !counted) {
                return counted;
            }
            break;
    }
    if (*number > 1) {
        if (auto counted = increment(counts.retries); !counted) {
            return counted;
        }
    }
    if (outcome) {
        auto* outcome_count = &counts.succeeded;
        switch (*outcome) {
            case AttemptOutcome::Succeeded:
                break;
            case AttemptOutcome::Failed:
                outcome_count = &counts.failed;
                break;
            case AttemptOutcome::Interrupted:
                outcome_count = &counts.interrupted;
                break;
            case AttemptOutcome::Cancelled:
                outcome_count = &counts.cancelled;
                break;
        }
        if (auto counted = increment(*outcome_count); !counted) {
            return counted;
        }
    }
    if (*state == AttemptState::Completed && started->has_value() && completed->has_value()) {
        if (auto observed = execution.observe(**started, **completed); !observed) {
            return observed;
        }
    }

    auto const* output_id = record.value("output_run_id");
    if (!output_id) {
        return RepositoryResult<void>::failure(invariant("missing_output_presence"));
    }
    if (!std::holds_alternative<jb::db::Null>(*output_id)) {
        auto present          = read_uuid(record, "output_run_id");
        auto stdout_truncated = read_boolean(record, "stdout_truncated");
        auto stderr_truncated = read_boolean(record, "stderr_truncated");
        auto capture_lost     = read_boolean(record, "capture_lost");
        if (!present || !stdout_truncated || !stderr_truncated || !capture_lost) {
            return RepositoryResult<void>::failure(invariant("capture_flags"));
        }
        if (*stdout_truncated || *stderr_truncated) {
            if (auto counted = increment(group.capture.truncated_attempts); !counted) {
                return counted;
            }
        }
        if (*capture_lost) {
            if (auto counted = increment(group.capture.lost_attempts); !counted) {
                return counted;
            }
        }
    }
    return RepositoryResult<void>::success();
}

} // namespace

StatisticsRepository::StatisticsRepository(jb::db::Database& database) noexcept
    : _database{database}
{}

auto StatisticsRepository::list_groups(StatisticsRequest const&          request,
                                       std::optional<StatisticsGroupKey> after,
                                       std::size_t limit) -> RepositoryResult<std::vector<StatisticsGroupKey>>
{
    if (limit < 1 || limit > 201) {
        return RepositoryResult<std::vector<StatisticsGroupKey>>::failure(invariant("group_limit"));
    }
    if (request.group_by == StatisticsGroupBy::None) {
        return RepositoryResult<std::vector<StatisticsGroupKey>>::success({StatisticsGroupKey{std::monostate{}}});
    }

    auto const column   = group_column(request.group_by);
    auto       sql      = fmt::format("SELECT DISTINCT {} AS group_key FROM jobu_runs AS r WHERE 1 = 1", column);
    auto       bindings = Bindings{};
    if (auto filters = add_filters(sql, bindings, request); !filters) {
        return RepositoryResult<std::vector<StatisticsGroupKey>>::failure(std::move(filters).error());
    }
    if (after) {
        auto value = key_value(request.group_by, *after);
        if (!value) {
            return RepositoryResult<std::vector<StatisticsGroupKey>>::failure(std::move(value).error());
        }
        sql += fmt::format(" AND {} > :after_key", column);
        bindings.emplace_back(":after_key", std::move(value).value());
    }
    sql += fmt::format(" ORDER BY {} LIMIT :limit", column);
    bindings.emplace_back(":limit", static_cast<std::int64_t>(limit));

    auto groups  = std::vector<StatisticsGroupKey>{};
    auto scanned = scan(_database, sql, bindings, [&](jb::db::Record const& record) -> RepositoryResult<void> {
        auto key = read_group_key(record, request.group_by);
        if (!key) {
            return RepositoryResult<void>::failure(std::move(key).error());
        }
        groups.push_back(std::move(key).value());
        return RepositoryResult<void>::success();
    });
    if (!scanned) {
        return RepositoryResult<std::vector<StatisticsGroupKey>>::failure(std::move(scanned).error());
    }
    return RepositoryResult<std::vector<StatisticsGroupKey>>::success(std::move(groups));
}

auto StatisticsRepository::aggregate(StatisticsRequest const& request, StatisticsGroupKey const& key)
    -> RepositoryResult<StatisticsGroup>
{
    auto group     = StatisticsGroup{.key = key};
    auto lateness  = DurationAccumulator{};
    auto execution = DurationAccumulator{};

    // Keep the run scan separate from attempts so retries cannot multiply run counts or first-start samples.
    auto run_sql      = std::string{"SELECT r.state AS run_state, r.type AS run_type, r.origin AS run_origin, "
                                    "r.planned_at_us AS planned_at_us, r.started_at_us AS started_at_us, "
                                    "r.completed_at_us AS completed_at_us "
                                    "FROM jobu_runs AS r WHERE 1 = 1"};
    auto run_bindings = Bindings{};
    if (auto filters = add_filters(run_sql, run_bindings, request); !filters) {
        return RepositoryResult<StatisticsGroup>::failure(std::move(filters).error());
    }
    if (auto filtered = add_group_filter(run_sql, run_bindings, request.group_by, key); !filtered) {
        return RepositoryResult<StatisticsGroup>::failure(std::move(filtered).error());
    }
    auto runs = scan(_database, run_sql, run_bindings, [&](jb::db::Record const& record) {
        return count_run(record, group, lateness);
    });
    if (!runs) {
        return RepositoryResult<StatisticsGroup>::failure(std::move(runs).error());
    }

    // Output joins contain flags only; absent output rows cannot create a capture observation.
    auto attempt_sql = std::string{
        "SELECT a.attempt_number AS attempt_number, a.state AS attempt_state, a.outcome AS attempt_outcome, "
        "a.started_at_us AS started_at_us, a.completed_at_us AS completed_at_us, "
        "o.run_id AS output_run_id, o.stdout_truncated AS stdout_truncated, "
        "o.stderr_truncated AS stderr_truncated, o.capture_lost AS capture_lost "
        "FROM jobu_runs AS r JOIN jobu_attempts AS a ON a.run_id = r.id "
        "LEFT JOIN jobu_attempt_output AS o ON o.run_id = a.run_id AND o.attempt_number = a.attempt_number "
        "WHERE 1 = 1"};
    auto attempt_bindings = Bindings{};
    if (auto filters = add_filters(attempt_sql, attempt_bindings, request); !filters) {
        return RepositoryResult<StatisticsGroup>::failure(std::move(filters).error());
    }
    if (auto filtered = add_group_filter(attempt_sql, attempt_bindings, request.group_by, key); !filtered) {
        return RepositoryResult<StatisticsGroup>::failure(std::move(filtered).error());
    }
    auto attempts = scan(_database, attempt_sql, attempt_bindings, [&](jb::db::Record const& record) {
        return count_attempt(record, group, execution);
    });
    if (!attempts) {
        return RepositoryResult<StatisticsGroup>::failure(std::move(attempts).error());
    }

    group.schedule_lateness_ms       = lateness.finish();
    group.execution_wall_duration_ms = execution.finish();
    return RepositoryResult<StatisticsGroup>::success(group);
}

} // namespace jb::jobu::detail
