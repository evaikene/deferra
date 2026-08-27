#include "run_repository_priv.hpp"

#include "attribute_codec_priv.hpp"
#include "domain_storage_priv.hpp"
#include "query.hpp"
#include "value.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace jb::jobu::detail {

namespace {

template <typename T>
using RepositoryResult = jb::core::Result<T, jb::core::Error>;

constexpr std::size_t kMaximumJsonDocumentBytes = std::size_t{256} * 1024U;
constexpr std::size_t kMaximumRetentionBatch    = 1000U;
// Keep each statement below conservative database parameter ceilings; the retention transaction makes chunks atomic.
constexpr std::size_t kMaximumRunIdsPerDelete   = 500U;

auto repository_error(jb::core::ErrorCategory category, std::string_view code, std::string_view message)
    -> jb::core::Error
{
    return {
        .category = category,
        .code     = std::string{code},
        .message  = std::string{message},
    };
}

auto invalid_run(std::string_view reason) -> jb::core::Error
{
    auto error   = repository_error(jb::core::ErrorCategory::Internal,
                                    "jobu.storage.invariant",
                                    "Persisted run data violates a JobU invariant");
    error.detail = "reason=" + std::string{reason};
    return error;
}

auto invalid_limit() -> jb::core::Error
{
    return repository_error(jb::core::ErrorCategory::InvalidArgument,
                            "jobu.storage.invalid_limit",
                            "Repository limit is outside its supported range");
}

auto invalid_json(std::string_view reason) -> jb::core::Error
{
    auto error   = repository_error(jb::core::ErrorCategory::Internal,
                                    "jobu.storage.invalid_json",
                                    "Persisted JSON document is invalid");
    error.detail = "reason=" + std::string{reason};
    return error;
}

auto schedule_conflict() -> jb::core::Error
{
    return {
        .category = jb::core::ErrorCategory::Conflict,
        .code     = "jobu.run.schedule_conflict",
        .message  = "The job already has a schedule-owned non-terminal run",
    };
}

auto schedule_conflict(jb::core::Error const& cause) -> jb::core::Error
{
    auto error  = schedule_conflict();
    auto detail = "cause=" + cause.code;
    if (!cause.detail.empty()) {
        detail += " " + cause.detail;
    }
    error.detail = std::move(detail);
    return error;
}

auto manual_conflict() -> jb::core::Error
{
    return {
        .category = jb::core::ErrorCategory::Conflict,
        .code     = "jobu.run.manual_conflict",
        .message  = "The job already has a non-terminal manual run",
    };
}

auto read_count(jb::db::Record const& record, std::string_view field) -> RepositoryResult<std::uint64_t>
{
    auto const* value = record.value(field);
    auto const* count = value == nullptr ? nullptr : std::get_if<std::int64_t>(value);
    if (count == nullptr || *count < 0) {
        return RepositoryResult<std::uint64_t>::failure(invalid_run("invalid_count"));
    }
    return RepositoryResult<std::uint64_t>::success(static_cast<std::uint64_t>(*count));
}

auto is_terminal(RunState state) noexcept -> bool
{
    return state == RunState::Succeeded || state == RunState::Failed || state == RunState::Interrupted ||
           state == RunState::Cancelled;
}

auto valid_started_at(RunState state, bool has_started_at) noexcept -> bool
{
    switch (state) {
        case RunState::Scheduled:
            return !has_started_at;
        case RunState::Running:
        case RunState::RetryWait:
        case RunState::Succeeded:
        case RunState::Failed:
        case RunState::Interrupted:
            return has_started_at;
        case RunState::Cancelled:
            return true;
    }
    return false;
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

auto optional_timestamp_to_storage(std::optional<jb::core::UtcTimePoint> value) -> RepositoryResult<jb::db::Value>
{
    if (!value) {
        return RepositoryResult<jb::db::Value>::success(jb::db::Null{});
    }
    return timestamp_to_storage(*value);
}

auto optional_json_to_storage(std::optional<jb::core::JsonValue> const& value) -> RepositoryResult<jb::db::Value>
{
    if (!value) {
        return RepositoryResult<jb::db::Value>::success(jb::db::Null{});
    }
    return json_to_storage(*value, true, kMaximumJsonDocumentBytes);
}

auto affected_rows(jb::db::Query const& query) -> RepositoryResult<std::size_t>
{
    auto const count = query.num_rows_affected();
    if (count < 0 || !std::in_range<std::size_t>(count)) {
        return RepositoryResult<std::size_t>::failure(invalid_run("invalid_affected_row_count"));
    }
    return RepositoryResult<std::size_t>::success(static_cast<std::size_t>(count));
}

auto cancellation_result(std::string_view reason) -> jb::core::JsonValue
{
    auto reason_value = jb::core::JsonValue{};
    reason_value.data = std::string{reason};
    auto result       = jb::core::JsonValue{};
    result.data       = jb::core::JsonValue::Object{
        {"reason", std::move(reason_value)}
    };
    return result;
}

auto cancel_pending(jb::db::Database&      database,
                    std::string_view       selector,
                    jb::core::Uuid const&  selector_id,
                    jb::core::UtcTimePoint completed_at,
                    std::string_view       reason) -> RepositoryResult<std::size_t>
{
    auto completed = timestamp_to_storage(completed_at);
    if (!completed) {
        return RepositoryResult<std::size_t>::failure(std::move(completed).error());
    }
    auto result = json_to_storage(cancellation_result(reason), true, kMaximumJsonDocumentBytes);
    if (!result) {
        return RepositoryResult<std::size_t>::failure(std::move(result).error());
    }

    auto sql  = std::string{"UPDATE jobu_runs SET state = 'cancelled', completed_at_us = :completed_at_us, "
                            "result_json = :result_json WHERE "};
    sql      += selector;
    sql      += " = :selector_id AND state IN ('scheduled', 'retry_wait')";

    jb::db::Query query{database};
    auto          prepared = query.prepare(sql);
    if (!prepared) {
        return RepositoryResult<std::size_t>::failure(std::move(prepared).error());
    }
    auto bound = bind_all(query,
                          {
                              {":completed_at_us", std::move(completed).value()},
                              {":result_json",     std::move(result).value()   },
                              {":selector_id",     uuid_to_storage(selector_id)},
    });
    if (!bound) {
        return RepositoryResult<std::size_t>::failure(std::move(bound).error());
    }
    auto executed = query.exec();
    if (!executed) {
        return RepositoryResult<std::size_t>::failure(std::move(executed).error());
    }
    return affected_rows(query);
}

auto count_running(jb::db::Database& database, std::string_view selector, jb::core::Uuid const& selector_id)
    -> RepositoryResult<std::uint64_t>
{
    auto sql  = std::string{"SELECT COUNT(*) AS running_count FROM jobu_runs WHERE "};
    sql      += selector;
    sql      += " = :selector_id AND state = 'running'";

    jb::db::Query query{database};
    auto          prepared = query.prepare(sql);
    if (!prepared) {
        return RepositoryResult<std::uint64_t>::failure(std::move(prepared).error());
    }
    auto bound = query.bind_value(":selector_id", uuid_to_storage(selector_id));
    if (!bound) {
        return RepositoryResult<std::uint64_t>::failure(std::move(bound).error());
    }
    auto executed = query.exec();
    if (!executed) {
        return RepositoryResult<std::uint64_t>::failure(std::move(executed).error());
    }
    auto next = query.next();
    if (!next) {
        return RepositoryResult<std::uint64_t>::failure(std::move(next).error());
    }
    if (!*next) {
        return RepositoryResult<std::uint64_t>::failure(invalid_run("missing_count_row"));
    }
    auto const* value = query.record().value("running_count");
    auto const* count = value == nullptr ? nullptr : std::get_if<std::int64_t>(value);
    if (count == nullptr || *count < 0) {
        return RepositoryResult<std::uint64_t>::failure(invalid_run("invalid_count"));
    }
    return RepositoryResult<std::uint64_t>::success(static_cast<std::uint64_t>(*count));
}

auto delete_terminal_chunk(jb::db::Database& database, std::span<jb::core::Uuid const> run_ids)
    -> RepositoryResult<std::size_t>
{
    auto sql = std::string{
        "DELETE FROM jobu_runs WHERE state IN ('succeeded', 'failed', 'interrupted', 'cancelled') AND id IN ("};
    for (std::size_t index = 0; index < run_ids.size(); ++index) {
        if (index != 0) {
            sql += ", ";
        }
        sql += ":id" + std::to_string(index);
    }
    sql += ")";

    jb::db::Query query{database};
    auto          prepared = query.prepare(sql);
    if (!prepared) {
        return RepositoryResult<std::size_t>::failure(std::move(prepared).error());
    }
    for (std::size_t index = 0; index < run_ids.size(); ++index) {
        auto placeholder = ":id" + std::to_string(index);
        auto bound       = query.bind_value(placeholder, uuid_to_storage(run_ids[index]));
        if (!bound) {
            return RepositoryResult<std::size_t>::failure(std::move(bound).error());
        }
    }
    auto executed = query.exec();
    if (!executed) {
        return RepositoryResult<std::size_t>::failure(std::move(executed).error());
    }
    return affected_rows(query);
}

} // anonymous namespace

RunRepository::RunRepository(jb::db::Database& database, AttributeRegistry const& attributes) noexcept
    : _database{database}
    , _attributes{attributes}
{}

auto RunRepository::insert_schedule_owned(JobRun const& run) -> jb::core::Result<void, jb::core::Error>
{
    if (!run.schedule_owned || run.origin != RunOrigin::Scheduled) {
        return RepositoryResult<void>::failure(invalid_run("insert_not_schedule_owned"));
    }
    if (is_terminal(run.state) != run.completed_at.has_value() || (!is_terminal(run.state) && run.result) ||
        !valid_started_at(run.state, run.started_at.has_value())) {
        return RepositoryResult<void>::failure(invalid_run("insert_state_mismatch"));
    }
    auto attributes = encode_and_serialize_attribute_document(_attributes,
                                                              run.attributes,
                                                              AttributeScope::Job,
                                                              AttributeDocumentMode::Materialized);
    if (!attributes) {
        return RepositoryResult<void>::failure(std::move(attributes).error());
    }
    if (attributes->serialized().size() > kMaximumJsonDocumentBytes) {
        return RepositoryResult<void>::failure(invalid_json("too_large"));
    }
    auto payload = json_to_storage(run.payload, true, kMaximumJsonDocumentBytes);
    if (!payload) {
        return RepositoryResult<void>::failure(std::move(payload).error());
    }

    if (run.state == RunState::Scheduled) {
        return insert_schedule_owned({
            .id              = run.id,
            .job_id          = run.job_id,
            .job_revision    = run.job_revision,
            .queue_id        = run.queue_id,
            .planned_at      = run.planned_at,
            .runnable_at     = run.runnable_at,
            .type            = run.type,
            .priority        = run.priority,
            .attributes_json = attributes->serialized(),
            .payload_json    = std::get<std::string>(*payload),
        });
    }

    if (!is_terminal(run.state)) {
        auto existing = find_schedule_owned(run.job_id);
        if (!existing) {
            return RepositoryResult<void>::failure(std::move(existing).error());
        }
        if (existing->has_value()) {
            return RepositoryResult<void>::failure(schedule_conflict());
        }
    }

    auto revision = revision_to_storage(run.job_revision);
    if (!revision) {
        return RepositoryResult<void>::failure(std::move(revision).error());
    }
    auto planned = timestamp_to_storage(run.planned_at);
    if (!planned) {
        return RepositoryResult<void>::failure(std::move(planned).error());
    }
    auto runnable = timestamp_to_storage(run.runnable_at);
    if (!runnable) {
        return RepositoryResult<void>::failure(std::move(runnable).error());
    }
    auto started = optional_timestamp_to_storage(run.started_at);
    if (!started) {
        return RepositoryResult<void>::failure(std::move(started).error());
    }
    auto completed = optional_timestamp_to_storage(run.completed_at);
    if (!completed) {
        return RepositoryResult<void>::failure(std::move(completed).error());
    }
    auto result = optional_json_to_storage(run.result);
    if (!result) {
        return RepositoryResult<void>::failure(std::move(result).error());
    }

    jb::db::Query query{_database};
    auto          prepared = query.prepare(
        "INSERT INTO jobu_runs(id, job_id, job_revision, queue_id, origin, schedule_owned, planned_at_us, "
        "runnable_at_us, started_at_us, completed_at_us, type, priority, attributes_json, payload_json, state, "
        "result_json) VALUES(:id, :job_id, :job_revision, :queue_id, :origin, :schedule_owned, :planned_at_us, "
        ":runnable_at_us, :started_at_us, :completed_at_us, :type, :priority, :attributes_json, :payload_json, "
        ":state, :result_json)");
    if (!prepared) {
        return RepositoryResult<void>::failure(std::move(prepared).error());
    }
    auto bound = bind_all(query,
                          {
                              {":id",              uuid_to_storage(run.id)                    },
                              {":job_id",          uuid_to_storage(run.job_id)                },
                              {":job_revision",    std::move(revision).value()                },
                              {":queue_id",        uuid_to_storage(run.queue_id)              },
                              {":origin",          jb::db::make_text(storage_text(run.origin))},
                              {":schedule_owned",  boolean_to_storage(run.schedule_owned)     },
                              {":planned_at_us",   std::move(planned).value()                 },
                              {":runnable_at_us",  std::move(runnable).value()                },
                              {":started_at_us",   std::move(started).value()                 },
                              {":completed_at_us", std::move(completed).value()               },
                              {":type",            jb::db::make_text(storage_text(run.type))  },
                              {":priority",        int32_to_storage(run.priority)             },
                              {":attributes_json", jb::db::make_text(attributes->serialized())},
                              {":payload_json",    std::move(payload).value()                 },
                              {":state",           jb::db::make_text(storage_text(run.state)) },
                              {":result_json",     std::move(result).value()                  },
    });
    if (!bound) {
        return bound;
    }
    auto executed = query.exec();
    if (!executed && executed.error().code == "db.constraint.unique" && !is_terminal(run.state)) {
        auto existing = find_schedule_owned(run.job_id);
        if (!existing) {
            return RepositoryResult<void>::failure(std::move(existing).error());
        }
        if (existing->has_value()) {
            return RepositoryResult<void>::failure(schedule_conflict(executed.error()));
        }
    }
    return executed;
}

auto RunRepository::insert_schedule_owned(ScheduleOwnedRunInsert const& run) -> jb::core::Result<void, jb::core::Error>
{
    if (run.attributes_json.size() > kMaximumJsonDocumentBytes || run.payload_json.size() > kMaximumJsonDocumentBytes) {
        return RepositoryResult<void>::failure(invalid_json("too_large"));
    }
    if (storage_text(run.type).empty()) {
        return RepositoryResult<void>::failure(invalid_run("invalid_type"));
    }
    auto revision = revision_to_storage(run.job_revision);
    if (!revision) {
        return RepositoryResult<void>::failure(std::move(revision).error());
    }
    auto planned = timestamp_to_storage(run.planned_at);
    if (!planned) {
        return RepositoryResult<void>::failure(std::move(planned).error());
    }
    auto runnable = timestamp_to_storage(run.runnable_at);
    if (!runnable) {
        return RepositoryResult<void>::failure(std::move(runnable).error());
    }
    auto existing = find_schedule_owned(run.job_id);
    if (!existing) {
        return RepositoryResult<void>::failure(std::move(existing).error());
    }
    if (existing->has_value()) {
        return RepositoryResult<void>::failure(schedule_conflict());
    }

    jb::db::Query query{_database};
    auto          prepared = query.prepare(
        "INSERT INTO jobu_runs(id, job_id, job_revision, queue_id, origin, schedule_owned, planned_at_us, "
        "runnable_at_us, started_at_us, completed_at_us, type, priority, attributes_json, payload_json, state, "
        "result_json) VALUES(:id, :job_id, :job_revision, :queue_id, 'scheduled', 1, :planned_at_us, "
        ":runnable_at_us, NULL, NULL, :type, :priority, :attributes_json, :payload_json, 'scheduled', NULL)");
    if (!prepared) {
        return RepositoryResult<void>::failure(std::move(prepared).error());
    }
    auto bound = bind_all(query,
                          {
                              {":id",              uuid_to_storage(run.id)                  },
                              {":job_id",          uuid_to_storage(run.job_id)              },
                              {":job_revision",    std::move(revision).value()              },
                              {":queue_id",        uuid_to_storage(run.queue_id)            },
                              {":planned_at_us",   std::move(planned).value()               },
                              {":runnable_at_us",  std::move(runnable).value()              },
                              {":type",            jb::db::make_text(storage_text(run.type))},
                              {":priority",        int32_to_storage(run.priority)           },
                              {":attributes_json", jb::db::make_text(run.attributes_json)   },
                              {":payload_json",    jb::db::make_text(run.payload_json)      },
    });
    if (!bound) {
        return bound;
    }
    auto executed = query.exec();
    if (!executed && executed.error().code == "db.constraint.unique") {
        existing = find_schedule_owned(run.job_id);
        if (!existing) {
            return RepositoryResult<void>::failure(std::move(existing).error());
        }
        if (existing->has_value()) {
            return RepositoryResult<void>::failure(schedule_conflict(executed.error()));
        }
    }
    return executed;
}

auto RunRepository::insert_manual(JobRun const& run) -> jb::core::Result<void, jb::core::Error>
{
    if (run.origin != RunOrigin::Manual || run.schedule_owned) {
        return RepositoryResult<void>::failure(invalid_run("insert_not_manual"));
    }
    if (run.state != RunState::Scheduled || run.planned_at != run.runnable_at || run.started_at || run.completed_at ||
        run.result) {
        return RepositoryResult<void>::failure(invalid_run("insert_state_mismatch"));
    }
    if (storage_text(run.type).empty()) {
        return RepositoryResult<void>::failure(invalid_run("invalid_type"));
    }
    auto attributes = encode_and_serialize_attribute_document(_attributes,
                                                              run.attributes,
                                                              AttributeScope::Job,
                                                              AttributeDocumentMode::Materialized);
    if (!attributes) {
        return RepositoryResult<void>::failure(std::move(attributes).error());
    }
    if (attributes->serialized().size() > kMaximumJsonDocumentBytes) {
        return RepositoryResult<void>::failure(invalid_json("too_large"));
    }
    auto payload = json_to_storage(run.payload, true, kMaximumJsonDocumentBytes);
    if (!payload) {
        return RepositoryResult<void>::failure(std::move(payload).error());
    }
    auto existing = has_non_terminal_manual_run(run.job_id);
    if (!existing) {
        return RepositoryResult<void>::failure(std::move(existing).error());
    }
    if (*existing) {
        return RepositoryResult<void>::failure(manual_conflict());
    }
    auto revision = revision_to_storage(run.job_revision);
    if (!revision) {
        return RepositoryResult<void>::failure(std::move(revision).error());
    }
    auto planned = timestamp_to_storage(run.planned_at);
    if (!planned) {
        return RepositoryResult<void>::failure(std::move(planned).error());
    }

    jb::db::Query query{_database};
    auto          prepared = query.prepare(
        "INSERT INTO jobu_runs(id, job_id, job_revision, queue_id, origin, schedule_owned, planned_at_us, "
        "runnable_at_us, started_at_us, completed_at_us, type, priority, attributes_json, payload_json, state, "
        "result_json) VALUES(:id, :job_id, :job_revision, :queue_id, 'manual', 0, :planned_at_us, "
        ":planned_at_us, NULL, NULL, :type, :priority, :attributes_json, :payload_json, 'scheduled', NULL)");
    if (!prepared) {
        return RepositoryResult<void>::failure(std::move(prepared).error());
    }
    auto bound = bind_all(query,
                          {
                              {":id",              uuid_to_storage(run.id)                    },
                              {":job_id",          uuid_to_storage(run.job_id)                },
                              {":job_revision",    std::move(revision).value()                },
                              {":queue_id",        uuid_to_storage(run.queue_id)              },
                              {":planned_at_us",   std::move(planned).value()                 },
                              {":type",            jb::db::make_text(storage_text(run.type))  },
                              {":priority",        int32_to_storage(run.priority)             },
                              {":attributes_json", jb::db::make_text(attributes->serialized())},
                              {":payload_json",    std::move(payload).value()                 },
    });
    if (!bound) {
        return bound;
    }
    return query.exec();
}

auto RunRepository::find_schedule_owned(jb::core::Uuid const& job_id)
    -> jb::core::Result<std::optional<JobRun>, jb::core::Error>
{
    jb::db::Query query{_database};
    auto          prepared = query.prepare("SELECT " + std::string{job_run_columns()} +
                                           " FROM jobu_runs "
                                           "WHERE job_id = :job_id AND schedule_owned = 1 "
                                           "AND state IN ('scheduled', 'running', 'retry_wait') LIMIT 2");
    if (!prepared) {
        return RepositoryResult<std::optional<JobRun>>::failure(std::move(prepared).error());
    }
    auto bound = query.bind_value(":job_id", uuid_to_storage(job_id));
    if (!bound) {
        return RepositoryResult<std::optional<JobRun>>::failure(std::move(bound).error());
    }
    auto executed = query.exec();
    if (!executed) {
        return RepositoryResult<std::optional<JobRun>>::failure(std::move(executed).error());
    }
    auto next = query.next();
    if (!next) {
        return RepositoryResult<std::optional<JobRun>>::failure(std::move(next).error());
    }
    if (!*next) {
        return RepositoryResult<std::optional<JobRun>>::success(std::nullopt);
    }
    auto decoded = read_job_run(query.record(), _attributes);
    if (!decoded) {
        return RepositoryResult<std::optional<JobRun>>::failure(std::move(decoded).error());
    }
    auto second = query.next();
    if (!second) {
        return RepositoryResult<std::optional<JobRun>>::failure(std::move(second).error());
    }
    if (*second) {
        return RepositoryResult<std::optional<JobRun>>::failure(invalid_run("multiple_schedule_owned_runs"));
    }
    return RepositoryResult<std::optional<JobRun>>::success(std::move(decoded).value());
}

auto RunRepository::find_by_id(jb::core::Uuid const& run_id) -> jb::core::Result<std::optional<JobRun>, jb::core::Error>
{
    jb::db::Query query{_database};
    auto prepared = query.prepare("SELECT " + std::string{job_run_columns()} + " FROM jobu_runs WHERE id = :run_id");
    if (!prepared) {
        return RepositoryResult<std::optional<JobRun>>::failure(std::move(prepared).error());
    }
    auto bound = query.bind_value(":run_id", uuid_to_storage(run_id));
    if (!bound) {
        return RepositoryResult<std::optional<JobRun>>::failure(std::move(bound).error());
    }
    auto executed = query.exec();
    if (!executed) {
        return RepositoryResult<std::optional<JobRun>>::failure(std::move(executed).error());
    }
    auto next = query.next();
    if (!next) {
        return RepositoryResult<std::optional<JobRun>>::failure(std::move(next).error());
    }
    if (!*next) {
        return RepositoryResult<std::optional<JobRun>>::success(std::nullopt);
    }
    auto decoded = read_job_run(query.record(), _attributes);
    if (!decoded) {
        return RepositoryResult<std::optional<JobRun>>::failure(std::move(decoded).error());
    }
    return RepositoryResult<std::optional<JobRun>>::success(std::move(decoded).value());
}

auto RunRepository::has_non_terminal_manual_run(jb::core::Uuid const& job_id) -> jb::core::Result<bool, jb::core::Error>
{
    jb::db::Query query{_database};
    auto          prepared =
        query.prepare("SELECT COUNT(*) AS manual_count, COALESCE(SUM(schedule_owned), 0) AS schedule_owned_count "
                      "FROM jobu_runs WHERE job_id = :job_id AND origin = 'manual' "
                      "AND state IN ('scheduled', 'running', 'retry_wait')");
    if (!prepared) {
        return RepositoryResult<bool>::failure(std::move(prepared).error());
    }
    auto bound = query.bind_value(":job_id", uuid_to_storage(job_id));
    if (!bound) {
        return RepositoryResult<bool>::failure(std::move(bound).error());
    }
    auto executed = query.exec();
    if (!executed) {
        return RepositoryResult<bool>::failure(std::move(executed).error());
    }
    auto next = query.next();
    if (!next) {
        return RepositoryResult<bool>::failure(std::move(next).error());
    }
    if (!*next) {
        return RepositoryResult<bool>::failure(invalid_run("missing_count_row"));
    }
    auto manual_count = read_count(query.record(), "manual_count");
    auto owned_count  = read_count(query.record(), "schedule_owned_count");
    if (!manual_count || !owned_count) {
        return RepositoryResult<bool>::failure(!manual_count ? std::move(manual_count).error()
                                                             : std::move(owned_count).error());
    }
    if (*manual_count > 1 || *owned_count != 0) {
        return RepositoryResult<bool>::failure(invalid_run("manual_run_relationship"));
    }
    return RepositoryResult<bool>::success(*manual_count == 1);
}

auto RunRepository::has_running_or_retrying_run(jb::core::Uuid const& job_id) -> jb::core::Result<bool, jb::core::Error>
{
    jb::db::Query query{_database};
    auto          prepared = query.prepare("SELECT COUNT(*) AS run_count FROM jobu_runs WHERE job_id = :job_id "
                                           "AND state IN ('running', 'retry_wait')");
    if (!prepared) {
        return RepositoryResult<bool>::failure(std::move(prepared).error());
    }
    auto bound = query.bind_value(":job_id", uuid_to_storage(job_id));
    if (!bound) {
        return RepositoryResult<bool>::failure(std::move(bound).error());
    }
    auto executed = query.exec();
    if (!executed) {
        return RepositoryResult<bool>::failure(std::move(executed).error());
    }
    auto next = query.next();
    if (!next) {
        return RepositoryResult<bool>::failure(std::move(next).error());
    }
    if (!*next) {
        return RepositoryResult<bool>::failure(invalid_run("missing_count_row"));
    }
    auto count = read_count(query.record(), "run_count");
    if (!count) {
        return RepositoryResult<bool>::failure(std::move(count).error());
    }
    return RepositoryResult<bool>::success(*count != 0);
}

auto RunRepository::refresh_unstarted_schedule_owned(jb::core::Uuid const& job_id, RunSnapshot const& snapshot)
    -> jb::core::Result<bool, jb::core::Error>
{
    auto attributes = encode_and_serialize_attribute_document(_attributes,
                                                              snapshot.attributes,
                                                              AttributeScope::Job,
                                                              AttributeDocumentMode::Materialized);
    if (!attributes) {
        return RepositoryResult<bool>::failure(std::move(attributes).error());
    }
    auto payload = json_to_storage(snapshot.payload, true, kMaximumJsonDocumentBytes);
    if (!payload) {
        return RepositoryResult<bool>::failure(std::move(payload).error());
    }
    return refresh_unstarted_schedule_owned(job_id,
                                            {
                                                .job_revision    = snapshot.job_revision,
                                                .queue_id        = snapshot.queue_id,
                                                .planned_at      = snapshot.planned_at,
                                                .runnable_at     = snapshot.runnable_at,
                                                .type            = snapshot.type,
                                                .priority        = snapshot.priority,
                                                .attributes_json = attributes->serialized(),
                                                .payload_json    = std::get<std::string>(*payload),
                                            });
}

auto RunRepository::refresh_unstarted_schedule_owned(jb::core::Uuid const&         job_id,
                                                     ScheduleOwnedRunUpdate const& snapshot)
    -> jb::core::Result<bool, jb::core::Error>
{
    if (snapshot.attributes_json.size() > kMaximumJsonDocumentBytes ||
        snapshot.payload_json.size() > kMaximumJsonDocumentBytes || storage_text(snapshot.type).empty()) {
        return RepositoryResult<bool>::failure(invalid_run("invalid_snapshot"));
    }
    auto revision = revision_to_storage(snapshot.job_revision);
    if (!revision) {
        return RepositoryResult<bool>::failure(std::move(revision).error());
    }
    auto planned = timestamp_to_storage(snapshot.planned_at);
    if (!planned) {
        return RepositoryResult<bool>::failure(std::move(planned).error());
    }
    auto runnable = timestamp_to_storage(snapshot.runnable_at);
    if (!runnable) {
        return RepositoryResult<bool>::failure(std::move(runnable).error());
    }
    jb::db::Query query{_database};
    auto          prepared = query.prepare(
        "UPDATE jobu_runs SET job_revision = :job_revision, queue_id = :queue_id, planned_at_us = :planned_at_us, "
        "runnable_at_us = :runnable_at_us, type = :type, priority = :priority, attributes_json = :attributes_json, "
        "payload_json = :payload_json WHERE job_id = :job_id AND schedule_owned = 1 AND state = 'scheduled' "
        "AND NOT EXISTS (SELECT 1 FROM jobu_attempts WHERE run_id = jobu_runs.id)");
    if (!prepared) {
        return RepositoryResult<bool>::failure(std::move(prepared).error());
    }
    auto bound = bind_all(query,
                          {
                              {":job_revision",    std::move(revision).value()                   },
                              {":queue_id",        uuid_to_storage(snapshot.queue_id)            },
                              {":planned_at_us",   std::move(planned).value()                    },
                              {":runnable_at_us",  std::move(runnable).value()                   },
                              {":type",            jb::db::make_text(storage_text(snapshot.type))},
                              {":priority",        int32_to_storage(snapshot.priority)           },
                              {":attributes_json", jb::db::make_text(snapshot.attributes_json)   },
                              {":payload_json",    jb::db::make_text(snapshot.payload_json)      },
                              {":job_id",          uuid_to_storage(job_id)                       },
    });
    if (!bound) {
        return RepositoryResult<bool>::failure(std::move(bound).error());
    }
    auto executed = query.exec();
    if (!executed) {
        return RepositoryResult<bool>::failure(std::move(executed).error());
    }
    auto affected = affected_rows(query);
    if (!affected) {
        return RepositoryResult<bool>::failure(std::move(affected).error());
    }
    return RepositoryResult<bool>::success(*affected == 1U);
}

auto RunRepository::move_non_terminal(jb::core::Uuid const& job_id,
                                      jb::core::Uuid const& target_queue_id,
                                      JobRevision next_revision) -> jb::core::Result<std::size_t, jb::core::Error>
{
    auto revision = revision_to_storage(next_revision);
    if (!revision) {
        return RepositoryResult<std::size_t>::failure(std::move(revision).error());
    }
    jb::db::Query query{_database};
    auto          prepared =
        query.prepare("UPDATE jobu_runs SET queue_id = :queue_id, job_revision = :job_revision WHERE job_id = :job_id "
                      "AND state IN ('scheduled', 'running', 'retry_wait')");
    if (!prepared) {
        return RepositoryResult<std::size_t>::failure(std::move(prepared).error());
    }
    auto bound = bind_all(query,
                          {
                              {":queue_id",     uuid_to_storage(target_queue_id)},
                              {":job_revision", std::move(revision).value()     },
                              {":job_id",       uuid_to_storage(job_id)         },
    });
    if (!bound) {
        return RepositoryResult<std::size_t>::failure(std::move(bound).error());
    }
    auto executed = query.exec();
    if (!executed) {
        return RepositoryResult<std::size_t>::failure(std::move(executed).error());
    }
    return affected_rows(query);
}

auto RunRepository::cancel_pending_for_job(jb::core::Uuid const&  job_id,
                                           jb::core::UtcTimePoint completed_at,
                                           std::string_view reason) -> jb::core::Result<std::size_t, jb::core::Error>
{
    return cancel_pending(_database, "job_id", job_id, completed_at, reason);
}

auto RunRepository::cancel_pending_for_queue(jb::core::Uuid const&  queue_id,
                                             jb::core::UtcTimePoint completed_at,
                                             std::string_view reason) -> jb::core::Result<std::size_t, jb::core::Error>
{
    return cancel_pending(_database, "queue_id", queue_id, completed_at, reason);
}

auto RunRepository::count_running_for_job(jb::core::Uuid const& job_id)
    -> jb::core::Result<std::uint64_t, jb::core::Error>
{
    return count_running(_database, "job_id", job_id);
}

auto RunRepository::count_running_for_queue(jb::core::Uuid const& queue_id)
    -> jb::core::Result<std::uint64_t, jb::core::Error>
{
    return count_running(_database, "queue_id", queue_id);
}

auto RunRepository::list_terminal_before(jb::core::UtcTimePoint cutoff, std::size_t limit)
    -> jb::core::Result<std::vector<jb::core::Uuid>, jb::core::Error>
{
    if (limit == 0 || limit > kMaximumRetentionBatch || !std::in_range<std::int64_t>(limit)) {
        return RepositoryResult<std::vector<jb::core::Uuid>>::failure(invalid_limit());
    }
    auto cutoff_value = timestamp_to_storage(cutoff);
    if (!cutoff_value) {
        return RepositoryResult<std::vector<jb::core::Uuid>>::failure(std::move(cutoff_value).error());
    }

    jb::db::Query query{_database};
    auto          prepared = query.prepare(
        "SELECT id AS run_id FROM jobu_runs WHERE state IN ('succeeded', 'failed', 'interrupted', 'cancelled') "
        "AND completed_at_us < :cutoff ORDER BY completed_at_us ASC, id ASC LIMIT :limit");
    if (!prepared) {
        return RepositoryResult<std::vector<jb::core::Uuid>>::failure(std::move(prepared).error());
    }
    auto bound = bind_all(query,
                          {
                              {":cutoff", std::move(cutoff_value).value() },
                              {":limit",  static_cast<std::int64_t>(limit)},
    });
    if (!bound) {
        return RepositoryResult<std::vector<jb::core::Uuid>>::failure(std::move(bound).error());
    }
    auto executed = query.exec();
    if (!executed) {
        return RepositoryResult<std::vector<jb::core::Uuid>>::failure(std::move(executed).error());
    }
    auto ids = std::vector<jb::core::Uuid>{};
    ids.reserve(limit);
    while (true) {
        auto next = query.next();
        if (!next) {
            return RepositoryResult<std::vector<jb::core::Uuid>>::failure(std::move(next).error());
        }
        if (!*next) {
            break;
        }
        auto id = read_uuid(query.record(), "run_id");
        if (!id) {
            return RepositoryResult<std::vector<jb::core::Uuid>>::failure(std::move(id).error());
        }
        ids.push_back(*id);
    }
    return RepositoryResult<std::vector<jb::core::Uuid>>::success(std::move(ids));
}

auto RunRepository::delete_selected_terminal(std::span<jb::core::Uuid const> run_ids)
    -> jb::core::Result<std::size_t, jb::core::Error>
{
    if (run_ids.empty()) {
        return RepositoryResult<std::size_t>::success(0U);
    }
    if (run_ids.size() > kMaximumRetentionBatch) {
        return RepositoryResult<std::size_t>::failure(invalid_limit());
    }

    auto deleted = std::size_t{0};
    for (std::size_t offset = 0; offset < run_ids.size(); offset += kMaximumRunIdsPerDelete) {
        auto const size  = std::min(kMaximumRunIdsPerDelete, run_ids.size() - offset);
        auto       chunk = delete_terminal_chunk(_database, run_ids.subspan(offset, size));
        if (!chunk) {
            return RepositoryResult<std::size_t>::failure(std::move(chunk).error());
        }
        deleted += *chunk;
    }
    return RepositoryResult<std::size_t>::success(deleted);
}

} // namespace jb::jobu::detail
