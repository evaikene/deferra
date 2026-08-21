#include "scheduler_repository_priv.hpp"

#include "domain_storage_priv.hpp"
#include "query.hpp"
#include "retry_policy_priv.hpp"
#include "value.hpp"

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace jb::jobu::detail {

namespace {

template <typename T>
using RepositoryResult = jb::core::Result<T, jb::core::Error>;

constexpr std::size_t kMaximumSchedulerPageRows = 1000U;

auto repository_error(jb::core::ErrorCategory category, std::string_view code, std::string_view message)
    -> jb::core::Error
{
    return {
        .category = category,
        .code     = std::string{code},
        .message  = std::string{message},
    };
}

auto invalid_limit() -> jb::core::Error
{
    return repository_error(jb::core::ErrorCategory::InvalidArgument,
                            "jobu.storage.invalid_limit",
                            "Repository limit is outside its supported range");
}

auto invariant(std::string_view reason, jb::core::Error const* cause = nullptr) -> jb::core::Error
{
    auto error   = repository_error(jb::core::ErrorCategory::Internal,
                                    "jobu.storage.invariant",
                                    "Persisted scheduler data violates a JobU invariant");
    error.detail = "reason=" + std::string{reason};
    if (cause != nullptr) {
        error.detail += " cause=" + cause->code;
        if (!cause->detail.empty()) {
            error.detail += " " + cause->detail;
        }
    }
    return error;
}

auto valid_limit(std::size_t limit) noexcept -> bool
{
    return limit != 0 && limit <= kMaximumSchedulerPageRows && std::in_range<std::int64_t>(limit);
}

auto bind_all(jb::db::Query& query, std::vector<std::pair<std::string_view, jb::db::Value>> values)
    -> RepositoryResult<void>
{
    for (auto& [placeholder, value] : values) {
        auto bound = query.bind_value(placeholder, std::move(value));
        if (!bound) {
            return RepositoryResult<void>::failure(std::move(bound).error());
        }
    }
    return RepositoryResult<void>::success();
}

auto read_count(jb::db::Record const& record, std::string_view field) -> RepositoryResult<std::uint64_t>
{
    auto const* value = record.value(field);
    if (value == nullptr) {
        return RepositoryResult<std::uint64_t>::failure(invariant("missing_count"));
    }
    auto const* integer = std::get_if<std::int64_t>(value);
    if (integer == nullptr || *integer < 0) {
        return RepositoryResult<std::uint64_t>::failure(invariant("invalid_count"));
    }
    return RepositoryResult<std::uint64_t>::success(static_cast<std::uint64_t>(*integer));
}

template <typename T>
auto scheduler_value(RepositoryResult<T> value, std::string_view reason) -> RepositoryResult<T>
{
    if (!value) {
        return RepositoryResult<T>::failure(invariant(reason, &value.error()));
    }
    return value;
}

auto add_after_clause(std::string& sql, std::optional<jb::core::Uuid> const& after_id)
{
    if (after_id) {
        sql += " AND jobu_runs.id > :after_id";
    }
}

auto bind_after(jb::db::Query& query, std::optional<jb::core::Uuid> const& after_id) -> RepositoryResult<void>
{
    if (!after_id) {
        return RepositoryResult<void>::success();
    }
    auto bound = query.bind_value(":after_id", uuid_to_storage(*after_id));
    if (!bound) {
        return RepositoryResult<void>::failure(std::move(bound).error());
    }
    return RepositoryResult<void>::success();
}

auto scheduler_run_columns() -> std::string
{
    return std::string{job_run_columns()} +
           ", jobu_jobs.queue_id AS job_queue_id, jobu_jobs.state AS job_state, "
           "jobu_queues.state AS queue_state, "
           "(SELECT COUNT(*) FROM jobu_attempts AS active_attempts "
           "WHERE active_attempts.run_id = jobu_runs.id AND active_attempts.state IN ('pending', 'running')) "
           "AS active_attempt_count, "
           "(SELECT COUNT(*) FROM jobu_attempts AS running_attempts "
           "WHERE running_attempts.run_id = jobu_runs.id AND running_attempts.state = 'running') "
           "AS running_attempt_count, "
           "(SELECT COUNT(*) FROM jobu_attempts AS completed_attempts "
           "WHERE completed_attempts.run_id = jobu_runs.id AND completed_attempts.state = 'completed') "
           "AS completed_attempt_count, "
           "(SELECT COUNT(*) FROM jobu_attempts AS failed_attempts "
           "WHERE failed_attempts.run_id = jobu_runs.id AND failed_attempts.state = 'completed' "
           "AND failed_attempts.outcome = 'failed') AS failed_attempt_count, "
           "(SELECT COUNT(*) FROM jobu_attempts AS all_attempts WHERE all_attempts.run_id = jobu_runs.id) "
           "AS total_attempt_count";
}

auto scheduler_run_joins() -> std::string_view
{
    return " FROM jobu_runs "
           "JOIN jobu_jobs ON jobu_jobs.id = jobu_runs.job_id "
           "JOIN jobu_queues ON jobu_queues.id = jobu_runs.queue_id ";
}

struct SchedulerRun {
    JobRun         run;
    jb::core::Uuid job_queue_id;
    JobState       job_state{JobState::Active};
    QueueState     queue_state{QueueState::Active};
    std::uint64_t  active_attempts{0};
    std::uint64_t  running_attempts{0};
    std::uint64_t  completed_attempts{0};
    std::uint64_t  failed_attempts{0};
    std::uint64_t  total_attempts{0};
};

auto decode_scheduler_run(jb::db::Record const& record, AttributeRegistry const& attributes)
    -> RepositoryResult<SchedulerRun>
{
    auto run = scheduler_value(read_job_run(record, attributes), "invalid_run");
    if (!run) {
        return RepositoryResult<SchedulerRun>::failure(std::move(run).error());
    }
    auto job_queue_id = scheduler_value(read_uuid(record, "job_queue_id"), "invalid_job_queue");
    if (!job_queue_id) {
        return RepositoryResult<SchedulerRun>::failure(std::move(job_queue_id).error());
    }
    auto job_state = scheduler_value(read_job_state(record, "job_state"), "invalid_job_state");
    if (!job_state) {
        return RepositoryResult<SchedulerRun>::failure(std::move(job_state).error());
    }
    auto queue_state = scheduler_value(read_queue_state(record, "queue_state"), "invalid_queue_state");
    if (!queue_state) {
        return RepositoryResult<SchedulerRun>::failure(std::move(queue_state).error());
    }
    auto active_attempts = read_count(record, "active_attempt_count");
    if (!active_attempts) {
        return RepositoryResult<SchedulerRun>::failure(std::move(active_attempts).error());
    }
    auto running_attempts = read_count(record, "running_attempt_count");
    if (!running_attempts) {
        return RepositoryResult<SchedulerRun>::failure(std::move(running_attempts).error());
    }
    auto completed_attempts = read_count(record, "completed_attempt_count");
    if (!completed_attempts) {
        return RepositoryResult<SchedulerRun>::failure(std::move(completed_attempts).error());
    }
    auto failed_attempts = read_count(record, "failed_attempt_count");
    if (!failed_attempts) {
        return RepositoryResult<SchedulerRun>::failure(std::move(failed_attempts).error());
    }
    auto total_attempts = read_count(record, "total_attempt_count");
    if (!total_attempts) {
        return RepositoryResult<SchedulerRun>::failure(std::move(total_attempts).error());
    }

    if (run->queue_id != *job_queue_id) {
        return RepositoryResult<SchedulerRun>::failure(invariant("run_job_queue_mismatch"));
    }
    if (*job_state == JobState::Deleted || *queue_state == QueueState::Deleted) {
        return RepositoryResult<SchedulerRun>::failure(invariant("non_terminal_owner_deleted"));
    }
    if (*active_attempts + *completed_attempts != *total_attempts || *running_attempts > *active_attempts) {
        return RepositoryResult<SchedulerRun>::failure(invariant("attempt_count_mismatch"));
    }

    return RepositoryResult<SchedulerRun>::success({
        .run                = std::move(run).value(),
        .job_queue_id       = *job_queue_id,
        .job_state          = *job_state,
        .queue_state        = *queue_state,
        .active_attempts    = *active_attempts,
        .running_attempts   = *running_attempts,
        .completed_attempts = *completed_attempts,
        .failed_attempts    = *failed_attempts,
        .total_attempts     = *total_attempts,
    });
}

auto candidate_where(std::string_view time_comparison) -> std::string
{
    auto sql  = std::string{" WHERE jobu_runs.state IN ('scheduled', 'retry_wait') "
                            "AND jobu_runs.type = :type AND jobu_runs.runnable_at_us "};
    sql      += time_comparison;
    sql      += " :now AND jobu_queues.state = 'active' "
                "AND ((jobu_runs.origin = 'scheduled' AND jobu_jobs.state = 'active') "
                "OR (jobu_runs.origin = 'manual' "
                "AND jobu_jobs.state IN ('active', 'suspending', 'suspended'))) "
                "AND (jobu_runs.schedule_owned = 0 OR NOT EXISTS ("
                "SELECT 1 FROM jobu_runs AS manual_runs WHERE manual_runs.job_id = jobu_runs.job_id "
                "AND manual_runs.origin = 'manual' AND manual_runs.schedule_owned = 0 "
                "AND manual_runs.state IN ('scheduled', 'running', 'retry_wait')))";
    return sql;
}

auto validate_candidate(SchedulerRun const&           row,
                        JobType                       type,
                        jb::core::UtcTimePoint        now,
                        bool                          future,
                        std::optional<jb::core::Uuid> queue_id) -> RepositoryResult<void>
{
    if (queue_id && row.run.queue_id != *queue_id) {
        return RepositoryResult<void>::failure(invariant("candidate_queue_mismatch"));
    }
    if (row.run.type != type || row.queue_state != QueueState::Active) {
        return RepositoryResult<void>::failure(invariant("candidate_filter_mismatch"));
    }
    if ((row.run.runnable_at > now) != future) {
        return RepositoryResult<void>::failure(invariant("candidate_time_mismatch"));
    }
    if (row.run.origin == RunOrigin::Scheduled) {
        if (!row.run.schedule_owned || row.job_state != JobState::Active) {
            return RepositoryResult<void>::failure(invariant("scheduled_candidate_relationship"));
        }
    }
    else if (row.run.origin == RunOrigin::Manual) {
        if (row.run.schedule_owned || row.job_state == JobState::Deleted) {
            return RepositoryResult<void>::failure(invariant("manual_candidate_relationship"));
        }
    }
    else {
        return RepositoryResult<void>::failure(invariant("unsupported_candidate_origin"));
    }
    if (row.active_attempts != 0 || row.running_attempts != 0) {
        return RepositoryResult<void>::failure(invariant("candidate_active_attempt"));
    }
    if (row.run.state == RunState::Scheduled && row.total_attempts != 0) {
        return RepositoryResult<void>::failure(invariant("scheduled_candidate_has_attempt"));
    }
    if (row.run.state == RunState::RetryWait &&
        (row.completed_attempts == 0 || row.completed_attempts != row.total_attempts ||
         row.failed_attempts != row.completed_attempts)) {
        return RepositoryResult<void>::failure(invariant("retry_candidate_attempt_history"));
    }
    return RepositoryResult<void>::success();
}

} // anonymous namespace

SchedulerRepository::SchedulerRepository(jb::db::Database& database, AttributeRegistry const& attributes) noexcept
    : _database{database}
    , _attributes{attributes}
{}

auto SchedulerRepository::list_runtime_queues(std::size_t limit, std::optional<jb::core::Uuid> after_id)
    -> jb::core::Result<std::vector<QueueRuntime>, jb::core::Error>
{
    if (!valid_limit(limit)) {
        return RepositoryResult<std::vector<QueueRuntime>>::failure(invalid_limit());
    }
    auto sql = std::string{"SELECT id AS runtime_queue_id, weight AS runtime_queue_weight, "
                           "concurrency_limit AS runtime_queue_concurrency_limit FROM jobu_queues "
                           "WHERE state = 'active'"};
    if (after_id) {
        sql += " AND id > :after_id";
    }
    sql += " ORDER BY id ASC LIMIT :limit";

    jb::db::Query query{_database};
    auto          prepared = query.prepare(sql);
    if (!prepared) {
        return RepositoryResult<std::vector<QueueRuntime>>::failure(std::move(prepared).error());
    }
    if (after_id) {
        auto bound = query.bind_value(":after_id", uuid_to_storage(*after_id));
        if (!bound) {
            return RepositoryResult<std::vector<QueueRuntime>>::failure(std::move(bound).error());
        }
    }
    auto bound = query.bind_value(":limit", static_cast<std::int64_t>(limit));
    if (!bound) {
        return RepositoryResult<std::vector<QueueRuntime>>::failure(std::move(bound).error());
    }
    auto executed = query.exec();
    if (!executed) {
        return RepositoryResult<std::vector<QueueRuntime>>::failure(std::move(executed).error());
    }

    auto queues = std::vector<QueueRuntime>{};
    queues.reserve(limit);
    while (true) {
        auto next = query.next();
        if (!next) {
            return RepositoryResult<std::vector<QueueRuntime>>::failure(std::move(next).error());
        }
        if (!*next) {
            break;
        }
        auto id = scheduler_value(read_uuid(query.record(), "runtime_queue_id"), "invalid_runtime_queue_id");
        if (!id) {
            return RepositoryResult<std::vector<QueueRuntime>>::failure(std::move(id).error());
        }
        auto weight = scheduler_value(read_positive_uint32(query.record(), "runtime_queue_weight"),
                                      "invalid_runtime_queue_weight");
        if (!weight) {
            return RepositoryResult<std::vector<QueueRuntime>>::failure(std::move(weight).error());
        }
        auto concurrency = scheduler_value(read_positive_uint32(query.record(), "runtime_queue_concurrency_limit"),
                                           "invalid_runtime_queue_concurrency");
        if (!concurrency) {
            return RepositoryResult<std::vector<QueueRuntime>>::failure(std::move(concurrency).error());
        }
        queues.push_back({.id = *id, .weight = *weight, .concurrency_limit = *concurrency});
    }
    return RepositoryResult<std::vector<QueueRuntime>>::success(std::move(queues));
}

auto SchedulerRepository::list_capacity_rows(std::size_t limit, std::optional<jb::core::Uuid> after_run_id)
    -> jb::core::Result<std::vector<CapacityRow>, jb::core::Error>
{
    if (!valid_limit(limit)) {
        return RepositoryResult<std::vector<CapacityRow>>::failure(invalid_limit());
    }
    auto sql = "SELECT " + scheduler_run_columns() + std::string{scheduler_run_joins()} +
               "WHERE jobu_runs.state IN ('running', 'retry_wait')";
    add_after_clause(sql, after_run_id);
    sql += " ORDER BY jobu_runs.id ASC LIMIT :limit";

    jb::db::Query query{_database};
    auto          prepared = query.prepare(sql);
    if (!prepared) {
        return RepositoryResult<std::vector<CapacityRow>>::failure(std::move(prepared).error());
    }
    auto after_bound = bind_after(query, after_run_id);
    if (!after_bound) {
        return RepositoryResult<std::vector<CapacityRow>>::failure(std::move(after_bound).error());
    }
    auto limit_bound = query.bind_value(":limit", static_cast<std::int64_t>(limit));
    if (!limit_bound) {
        return RepositoryResult<std::vector<CapacityRow>>::failure(std::move(limit_bound).error());
    }
    auto executed = query.exec();
    if (!executed) {
        return RepositoryResult<std::vector<CapacityRow>>::failure(std::move(executed).error());
    }

    auto rows = std::vector<CapacityRow>{};
    rows.reserve(limit);
    while (true) {
        auto next = query.next();
        if (!next) {
            return RepositoryResult<std::vector<CapacityRow>>::failure(std::move(next).error());
        }
        if (!*next) {
            break;
        }
        auto decoded = decode_scheduler_run(query.record(), _attributes);
        if (!decoded) {
            return RepositoryResult<std::vector<CapacityRow>>::failure(std::move(decoded).error());
        }

        auto usage = CapacityUsage{};
        if (decoded->run.state == RunState::Running) {
            if (decoded->active_attempts != 1 || decoded->running_attempts != 1 ||
                decoded->completed_attempts + 1 != decoded->total_attempts) {
                return RepositoryResult<std::vector<CapacityRow>>::failure(invariant("running_attempt_relationship"));
            }
            if (decoded->run.type == JobType::Cli) {
                usage.cli_running = 1;
            }
            else {
                usage.http_running = 1;
            }
            usage.queue_slots = 1;
        }
        else {
            if (decoded->run.state != RunState::RetryWait || decoded->active_attempts != 0 ||
                decoded->completed_attempts == 0 || decoded->completed_attempts != decoded->total_attempts ||
                decoded->failed_attempts != decoded->completed_attempts) {
                return RepositoryResult<std::vector<CapacityRow>>::failure(invariant("retry_attempt_relationship"));
            }
            auto policy = retry_policy_from_attributes(decoded->run.attributes);
            if (!policy) {
                return RepositoryResult<std::vector<CapacityRow>>::failure(
                    invariant("invalid_retry_policy", &policy.error()));
            }
            auto const job_permits_execution =
                decoded->job_state == JobState::Active || decoded->run.origin == RunOrigin::Manual;
            if (policy->mode == RetryMode::Blocking && job_permits_execution &&
                decoded->queue_state == QueueState::Active) {
                usage.queue_slots = 1;
            }
        }
        rows.push_back({.run_id = decoded->run.id, .queue_id = decoded->run.queue_id, .usage = usage});
    }
    return RepositoryResult<std::vector<CapacityRow>>::success(std::move(rows));
}

auto SchedulerRepository::list_manual_barriers(std::size_t limit, std::optional<jb::core::Uuid> after_run_id)
    -> jb::core::Result<std::vector<ManualBarrier>, jb::core::Error>
{
    if (!valid_limit(limit)) {
        return RepositoryResult<std::vector<ManualBarrier>>::failure(invalid_limit());
    }
    auto sql = "SELECT " + scheduler_run_columns() +
               ", (SELECT COUNT(*) FROM jobu_runs AS manual_siblings "
               "WHERE manual_siblings.job_id = jobu_runs.job_id AND manual_siblings.origin = 'manual' "
               "AND manual_siblings.state IN ('scheduled', 'running', 'retry_wait')) AS manual_sibling_count, "
               "(SELECT COUNT(*) FROM jobu_runs AS schedule_siblings "
               "WHERE schedule_siblings.job_id = jobu_runs.job_id AND schedule_siblings.origin = 'scheduled' "
               "AND schedule_siblings.schedule_owned = 1 "
               "AND schedule_siblings.state IN ('scheduled', 'running', 'retry_wait')) AS schedule_sibling_count" +
               std::string{scheduler_run_joins()} +
               "WHERE jobu_runs.origin = 'manual' "
               "AND jobu_runs.state IN ('scheduled', 'running', 'retry_wait')";
    add_after_clause(sql, after_run_id);
    sql += " ORDER BY jobu_runs.id ASC LIMIT :limit";

    jb::db::Query query{_database};
    auto          prepared = query.prepare(sql);
    if (!prepared) {
        return RepositoryResult<std::vector<ManualBarrier>>::failure(std::move(prepared).error());
    }
    auto after_bound = bind_after(query, after_run_id);
    if (!after_bound) {
        return RepositoryResult<std::vector<ManualBarrier>>::failure(std::move(after_bound).error());
    }
    auto limit_bound = query.bind_value(":limit", static_cast<std::int64_t>(limit));
    if (!limit_bound) {
        return RepositoryResult<std::vector<ManualBarrier>>::failure(std::move(limit_bound).error());
    }
    auto executed = query.exec();
    if (!executed) {
        return RepositoryResult<std::vector<ManualBarrier>>::failure(std::move(executed).error());
    }

    auto barriers = std::vector<ManualBarrier>{};
    barriers.reserve(limit);
    while (true) {
        auto next = query.next();
        if (!next) {
            return RepositoryResult<std::vector<ManualBarrier>>::failure(std::move(next).error());
        }
        if (!*next) {
            break;
        }
        auto decoded = decode_scheduler_run(query.record(), _attributes);
        if (!decoded) {
            return RepositoryResult<std::vector<ManualBarrier>>::failure(std::move(decoded).error());
        }
        auto manual_count   = read_count(query.record(), "manual_sibling_count");
        auto schedule_count = read_count(query.record(), "schedule_sibling_count");
        if (!manual_count || !schedule_count) {
            return RepositoryResult<std::vector<ManualBarrier>>::failure(
                !manual_count ? std::move(manual_count).error() : std::move(schedule_count).error());
        }
        if (decoded->run.schedule_owned || *manual_count != 1 || *schedule_count != 1) {
            return RepositoryResult<std::vector<ManualBarrier>>::failure(invariant("manual_barrier_relationship"));
        }
        barriers.push_back({.run_id = decoded->run.id, .job_id = decoded->run.job_id});
    }
    return RepositoryResult<std::vector<ManualBarrier>>::success(std::move(barriers));
}

auto SchedulerRepository::list_runnable(jb::core::Uuid const&  queue_id,
                                        JobType                type,
                                        jb::core::UtcTimePoint now,
                                        std::size_t            limit)
    -> jb::core::Result<std::vector<DispatchCandidate>, jb::core::Error>
{
    if (!valid_limit(limit)) {
        return RepositoryResult<std::vector<DispatchCandidate>>::failure(invalid_limit());
    }
    auto now_value = timestamp_to_storage(now);
    if (!now_value) {
        return RepositoryResult<std::vector<DispatchCandidate>>::failure(std::move(now_value).error());
    }
    auto sql = "SELECT " + scheduler_run_columns() + std::string{scheduler_run_joins()} + candidate_where("<=") +
               " AND jobu_runs.queue_id = :queue_id "
               "ORDER BY jobu_runs.priority DESC, jobu_runs.runnable_at_us ASC, "
               "jobu_runs.planned_at_us ASC, jobu_runs.id ASC LIMIT :limit";

    jb::db::Query query{_database};
    auto          prepared = query.prepare(sql);
    if (!prepared) {
        return RepositoryResult<std::vector<DispatchCandidate>>::failure(std::move(prepared).error());
    }
    auto bound = bind_all(query,
                          {
                              {":type",     jb::db::make_text(storage_text(type))},
                              {":now",      std::move(now_value).value()         },
                              {":queue_id", uuid_to_storage(queue_id)            },
                              {":limit",    static_cast<std::int64_t>(limit)     },
    });
    if (!bound) {
        return RepositoryResult<std::vector<DispatchCandidate>>::failure(std::move(bound).error());
    }
    auto executed = query.exec();
    if (!executed) {
        return RepositoryResult<std::vector<DispatchCandidate>>::failure(std::move(executed).error());
    }

    auto candidates = std::vector<DispatchCandidate>{};
    candidates.reserve(limit);
    while (true) {
        auto next = query.next();
        if (!next) {
            return RepositoryResult<std::vector<DispatchCandidate>>::failure(std::move(next).error());
        }
        if (!*next) {
            break;
        }
        auto decoded = decode_scheduler_run(query.record(), _attributes);
        if (!decoded) {
            return RepositoryResult<std::vector<DispatchCandidate>>::failure(std::move(decoded).error());
        }
        auto valid = validate_candidate(*decoded, type, now, false, queue_id);
        if (!valid) {
            return RepositoryResult<std::vector<DispatchCandidate>>::failure(std::move(valid).error());
        }
        candidates.push_back({
            .run         = std::move(decoded->run),
            .job_state   = decoded->job_state,
            .queue_state = decoded->queue_state,
        });
    }
    return RepositoryResult<std::vector<DispatchCandidate>>::success(std::move(candidates));
}

auto SchedulerRepository::earliest_future_runnable(JobType type, jb::core::UtcTimePoint now)
    -> jb::core::Result<std::optional<jb::core::UtcTimePoint>, jb::core::Error>
{
    auto now_value = timestamp_to_storage(now);
    if (!now_value) {
        return RepositoryResult<std::optional<jb::core::UtcTimePoint>>::failure(std::move(now_value).error());
    }
    auto sql = "SELECT " + scheduler_run_columns() + std::string{scheduler_run_joins()} + candidate_where(">") +
               " ORDER BY jobu_runs.runnable_at_us ASC, jobu_runs.priority DESC, "
               "jobu_runs.planned_at_us ASC, jobu_runs.id ASC LIMIT 1";

    jb::db::Query query{_database};
    auto          prepared = query.prepare(sql);
    if (!prepared) {
        return RepositoryResult<std::optional<jb::core::UtcTimePoint>>::failure(std::move(prepared).error());
    }
    auto bound = bind_all(query,
                          {
                              {":type", jb::db::make_text(storage_text(type))},
                              {":now",  std::move(now_value).value()         },
    });
    if (!bound) {
        return RepositoryResult<std::optional<jb::core::UtcTimePoint>>::failure(std::move(bound).error());
    }
    auto executed = query.exec();
    if (!executed) {
        return RepositoryResult<std::optional<jb::core::UtcTimePoint>>::failure(std::move(executed).error());
    }
    auto next = query.next();
    if (!next) {
        return RepositoryResult<std::optional<jb::core::UtcTimePoint>>::failure(std::move(next).error());
    }
    if (!*next) {
        return RepositoryResult<std::optional<jb::core::UtcTimePoint>>::success(std::nullopt);
    }
    auto decoded = decode_scheduler_run(query.record(), _attributes);
    if (!decoded) {
        return RepositoryResult<std::optional<jb::core::UtcTimePoint>>::failure(std::move(decoded).error());
    }
    auto valid = validate_candidate(*decoded, type, now, true, std::nullopt);
    if (!valid) {
        return RepositoryResult<std::optional<jb::core::UtcTimePoint>>::failure(std::move(valid).error());
    }
    return RepositoryResult<std::optional<jb::core::UtcTimePoint>>::success(decoded->run.runnable_at);
}

auto SchedulerRepository::has_any_running_state() -> jb::core::Result<bool, jb::core::Error>
{
    jb::db::Query query{_database};
    auto executed = query.exec("SELECT EXISTS(SELECT 1 FROM jobu_runs WHERE state = 'running') AS running_run_count, "
                               "EXISTS(SELECT 1 FROM jobu_attempts WHERE state = 'running') AS running_attempt_count");
    if (!executed) {
        return RepositoryResult<bool>::failure(std::move(executed).error());
    }
    auto next = query.next();
    if (!next) {
        return RepositoryResult<bool>::failure(std::move(next).error());
    }
    if (!*next) {
        return RepositoryResult<bool>::failure(invariant("missing_running_state_row"));
    }
    auto runs     = read_count(query.record(), "running_run_count");
    auto attempts = read_count(query.record(), "running_attempt_count");
    if (!runs || !attempts) {
        return RepositoryResult<bool>::failure(!runs ? std::move(runs).error() : std::move(attempts).error());
    }
    return RepositoryResult<bool>::success(*runs != 0 || *attempts != 0);
}

} // namespace jb::jobu::detail
