#include "job_repository_priv.hpp"

#include "attribute_codec_priv.hpp"
#include "domain_storage_priv.hpp"
#include "job_validation_priv.hpp"
#include "query.hpp"
#include "value.hpp"

#include <cstdint>
#include <string>
#include <utility>

namespace jb::jobu::detail {

namespace {

template <typename T>
using RepositoryResult = jb::core::Result<T, jb::core::Error>;

constexpr std::size_t kMaximumListRows = 201;

constexpr auto kJobSelection =
    "SELECT id AS job_id, queue_id AS job_queue_id, revision AS job_revision, name AS job_name, "
    "state AS job_state, type AS job_type, schedule_kind AS job_schedule_kind, "
    "scheduled_at_us AS job_scheduled_at_us, cron_expression AS job_cron_expression, "
    "cron_timezone AS job_cron_timezone, priority AS job_priority, attributes_json AS job_attributes_json, "
    "payload_json AS job_payload_json, created_at_us AS job_created_at_us, updated_at_us AS job_updated_at_us, "
    "deleted_at_us AS job_deleted_at_us FROM jobu_jobs ";

auto repository_error(jb::core::ErrorCategory category, std::string_view code, std::string_view message)
    -> jb::core::Error
{
    return {
        .category = category,
        .code     = std::string{code},
        .message  = std::string{message},
    };
}

auto invalid_job(std::string_view reason) -> jb::core::Error
{
    auto error   = repository_error(jb::core::ErrorCategory::Internal,
                                    "jobu.storage.invariant",
                                    "Persisted job data violates a JobU invariant");
    error.detail = "reason=" + std::string{reason};
    return error;
}

auto invalid_limit() -> jb::core::Error
{
    return repository_error(jb::core::ErrorCategory::InvalidArgument,
                            "jobu.storage.invalid_limit",
                            "Repository limit is outside its supported range");
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

auto attributes_to_storage(AttributeRegistry const& attributes, AttributeSet const& values)
    -> RepositoryResult<jb::db::Value>
{
    auto encoded =
        encode_attribute_document(attributes, values, AttributeScope::Job, AttributeDocumentMode::Materialized);
    if (!encoded) {
        return RepositoryResult<jb::db::Value>::failure(std::move(encoded).error());
    }
    return json_to_storage(*encoded, true, maximum_job_document_bytes);
}

struct StoredSchedule {
    jb::db::Value kind;
    jb::db::Value scheduled_at;
    jb::db::Value cron_expression;
    jb::db::Value cron_timezone;
};

auto schedule_to_storage(JobSchedule const& schedule) -> RepositoryResult<StoredSchedule>
{
    if (auto const* once = std::get_if<OnceSchedule>(&schedule)) {
        auto planned = timestamp_to_storage(once->planned_at);
        if (!planned) {
            return RepositoryResult<StoredSchedule>::failure(std::move(planned).error());
        }
        return RepositoryResult<StoredSchedule>::success({
            .kind            = jb::db::make_text("once"),
            .scheduled_at    = std::move(planned).value(),
            .cron_expression = jb::db::Null{},
            .cron_timezone   = jb::db::Null{},
        });
    }
    auto const& cron = std::get<CronSchedule>(schedule);
    return RepositoryResult<StoredSchedule>::success({
        .kind            = jb::db::make_text("cron"),
        .scheduled_at    = jb::db::Null{},
        .cron_expression = jb::db::make_text(cron.expression),
        .cron_timezone   = jb::db::make_text(cron.timezone),
    });
}

auto decode_schedule(jb::db::Record const& record) -> RepositoryResult<JobSchedule>
{
    auto kind = read_text(record, "job_schedule_kind");
    if (!kind) {
        return RepositoryResult<JobSchedule>::failure(std::move(kind).error());
    }
    auto scheduled_at = read_optional_timestamp(record, "job_scheduled_at_us");
    if (!scheduled_at) {
        return RepositoryResult<JobSchedule>::failure(std::move(scheduled_at).error());
    }
    auto expression = read_optional_text(record, "job_cron_expression");
    if (!expression) {
        return RepositoryResult<JobSchedule>::failure(std::move(expression).error());
    }
    auto timezone = read_optional_text(record, "job_cron_timezone");
    if (!timezone) {
        return RepositoryResult<JobSchedule>::failure(std::move(timezone).error());
    }

    if (*kind == "once" && scheduled_at->has_value() && !expression->has_value() && !timezone->has_value()) {
        return RepositoryResult<JobSchedule>::success(OnceSchedule{.planned_at = **scheduled_at});
    }
    if (*kind == "cron" && !scheduled_at->has_value() && expression->has_value() && timezone->has_value()) {
        return RepositoryResult<JobSchedule>::success(CronSchedule{
            .expression = std::move(**expression),
            .timezone   = std::move(**timezone),
        });
    }
    return RepositoryResult<JobSchedule>::failure(invalid_job("schedule_columns"));
}

auto decode_job(jb::db::Record const& record, AttributeRegistry const& attributes) -> RepositoryResult<JobDefinition>
{
    auto id = read_uuid(record, "job_id");
    if (!id) {
        return RepositoryResult<JobDefinition>::failure(std::move(id).error());
    }
    auto queue_id = read_uuid(record, "job_queue_id");
    if (!queue_id) {
        return RepositoryResult<JobDefinition>::failure(std::move(queue_id).error());
    }
    auto revision = read_revision(record, "job_revision");
    if (!revision) {
        return RepositoryResult<JobDefinition>::failure(std::move(revision).error());
    }
    auto name = read_optional_text(record, "job_name");
    if (!name) {
        return RepositoryResult<JobDefinition>::failure(std::move(name).error());
    }
    if (name->has_value() && !is_valid_job_name(**name)) {
        return RepositoryResult<JobDefinition>::failure(invalid_job("invalid_name"));
    }
    auto state = read_job_state(record, "job_state");
    if (!state) {
        return RepositoryResult<JobDefinition>::failure(std::move(state).error());
    }
    auto type = read_job_type(record, "job_type");
    if (!type) {
        return RepositoryResult<JobDefinition>::failure(std::move(type).error());
    }
    auto schedule = decode_schedule(record);
    if (!schedule) {
        return RepositoryResult<JobDefinition>::failure(std::move(schedule).error());
    }
    auto priority = read_int32(record, "job_priority");
    if (!priority) {
        return RepositoryResult<JobDefinition>::failure(std::move(priority).error());
    }
    auto attributes_json = read_json(record, "job_attributes_json", true, maximum_job_document_bytes);
    if (!attributes_json) {
        return RepositoryResult<JobDefinition>::failure(std::move(attributes_json).error());
    }
    auto decoded_attributes = decode_attribute_document(attributes,
                                                        *attributes_json,
                                                        AttributeScope::Job,
                                                        AttributeDocumentMode::Materialized);
    if (!decoded_attributes) {
        return RepositoryResult<JobDefinition>::failure(std::move(decoded_attributes).error());
    }
    auto payload = read_json(record, "job_payload_json", true, maximum_job_document_bytes);
    if (!payload) {
        return RepositoryResult<JobDefinition>::failure(std::move(payload).error());
    }
    auto payload_issue = job_payload_issue(*type, *payload);
    if (payload_issue != JobPayloadIssue::None) {
        return RepositoryResult<JobDefinition>::failure(invalid_job(job_payload_issue_text(payload_issue)));
    }
    auto created = read_timestamp(record, "job_created_at_us");
    if (!created) {
        return RepositoryResult<JobDefinition>::failure(std::move(created).error());
    }
    auto updated = read_timestamp(record, "job_updated_at_us");
    if (!updated) {
        return RepositoryResult<JobDefinition>::failure(std::move(updated).error());
    }
    auto deleted = read_optional_timestamp(record, "job_deleted_at_us");
    if (!deleted) {
        return RepositoryResult<JobDefinition>::failure(std::move(deleted).error());
    }
    if ((*state == JobState::Deleted) != deleted->has_value()) {
        return RepositoryResult<JobDefinition>::failure(invalid_job("deleted_state_mismatch"));
    }

    return RepositoryResult<JobDefinition>::success(JobDefinition{
        .id         = *id,
        .queue_id   = *queue_id,
        .revision   = *revision,
        .name       = std::move(name).value(),
        .state      = *state,
        .type       = *type,
        .schedule   = std::move(schedule).value(),
        .priority   = *priority,
        .attributes = std::move(decoded_attributes).value(),
        .payload    = std::move(payload).value(),
        .created_at = *created,
        .updated_at = *updated,
        .deleted_at = *deleted,
    });
}

} // anonymous namespace

JobRepository::JobRepository(jb::db::Database& database, AttributeRegistry const& attributes) noexcept
    : _database{database}
    , _attributes{attributes}
{}

auto JobRepository::insert(JobDefinition const& job) -> jb::core::Result<void, jb::core::Error>
{
    if (job.name && !is_valid_job_name(*job.name)) {
        return RepositoryResult<void>::failure(invalid_job("invalid_name"));
    }
    if ((job.state == JobState::Deleted) != job.deleted_at.has_value()) {
        return RepositoryResult<void>::failure(invalid_job("deleted_state_mismatch"));
    }
    auto payload_issue = job_payload_issue(job.type, job.payload);
    if (payload_issue != JobPayloadIssue::None) {
        return RepositoryResult<void>::failure(invalid_job(job_payload_issue_text(payload_issue)));
    }

    auto revision = revision_to_storage(job.revision);
    if (!revision) {
        return RepositoryResult<void>::failure(std::move(revision).error());
    }
    auto schedule = schedule_to_storage(job.schedule);
    if (!schedule) {
        return RepositoryResult<void>::failure(std::move(schedule).error());
    }
    auto attributes = attributes_to_storage(_attributes, job.attributes);
    if (!attributes) {
        return RepositoryResult<void>::failure(std::move(attributes).error());
    }
    auto payload = json_to_storage(job.payload, true, maximum_job_document_bytes);
    if (!payload) {
        return RepositoryResult<void>::failure(std::move(payload).error());
    }
    auto created = timestamp_to_storage(job.created_at);
    if (!created) {
        return RepositoryResult<void>::failure(std::move(created).error());
    }
    auto updated = timestamp_to_storage(job.updated_at);
    if (!updated) {
        return RepositoryResult<void>::failure(std::move(updated).error());
    }

    jb::db::Query query{_database};
    auto          prepared = query.prepare(
        "INSERT INTO jobu_jobs(id, queue_id, revision, name, state, type, schedule_kind, scheduled_at_us, "
        "cron_expression, cron_timezone, priority, attributes_json, payload_json, created_at_us, updated_at_us, "
        "deleted_at_us) VALUES(:id, :queue_id, :revision, :name, :state, :type, :schedule_kind, :scheduled_at_us, "
        ":cron_expression, :cron_timezone, :priority, :attributes_json, :payload_json, :created_at_us, "
        ":updated_at_us, :deleted_at_us)");
    if (!prepared) {
        return RepositoryResult<void>::failure(std::move(prepared).error());
    }
    auto name       = job.name ? jb::db::make_text(*job.name) : jb::db::Value{jb::db::Null{}};
    auto deleted_at = jb::db::Value{jb::db::Null{}};
    if (job.deleted_at) {
        auto stored = timestamp_to_storage(*job.deleted_at);
        if (!stored) {
            return RepositoryResult<void>::failure(std::move(stored).error());
        }
        deleted_at = std::move(stored).value();
    }
    auto bound = bind_all(query,
                          {
                              {":id",              uuid_to_storage(job.id)                   },
                              {":queue_id",        uuid_to_storage(job.queue_id)             },
                              {":revision",        std::move(revision).value()               },
                              {":name",            std::move(name)                           },
                              {":state",           jb::db::make_text(storage_text(job.state))},
                              {":type",            jb::db::make_text(storage_text(job.type)) },
                              {":schedule_kind",   std::move(schedule->kind)                 },
                              {":scheduled_at_us", std::move(schedule->scheduled_at)         },
                              {":cron_expression", std::move(schedule->cron_expression)      },
                              {":cron_timezone",   std::move(schedule->cron_timezone)        },
                              {":priority",        int32_to_storage(job.priority)            },
                              {":attributes_json", std::move(attributes).value()             },
                              {":payload_json",    std::move(payload).value()                },
                              {":created_at_us",   std::move(created).value()                },
                              {":updated_at_us",   std::move(updated).value()                },
                              {":deleted_at_us",   std::move(deleted_at)                     },
    });
    if (!bound) {
        return bound;
    }
    return query.exec();
}

auto JobRepository::find_by_id(jb::core::Uuid const& id, bool include_deleted)
    -> jb::core::Result<std::optional<JobDefinition>, jb::core::Error>
{
    auto sql = std::string{kJobSelection} + "WHERE id = :id";
    if (!include_deleted) {
        sql += " AND state <> 'deleted'";
    }
    jb::db::Query query{_database};
    auto          prepared = query.prepare(sql);
    if (!prepared) {
        return RepositoryResult<std::optional<JobDefinition>>::failure(std::move(prepared).error());
    }
    auto bound = query.bind_value(":id", uuid_to_storage(id));
    if (!bound) {
        return RepositoryResult<std::optional<JobDefinition>>::failure(std::move(bound).error());
    }
    auto executed = query.exec();
    if (!executed) {
        return RepositoryResult<std::optional<JobDefinition>>::failure(std::move(executed).error());
    }
    auto next = query.next();
    if (!next) {
        return RepositoryResult<std::optional<JobDefinition>>::failure(std::move(next).error());
    }
    if (!*next) {
        return RepositoryResult<std::optional<JobDefinition>>::success(std::nullopt);
    }
    auto decoded = decode_job(query.record(), _attributes);
    if (!decoded) {
        return RepositoryResult<std::optional<JobDefinition>>::failure(std::move(decoded).error());
    }
    return RepositoryResult<std::optional<JobDefinition>>::success(std::move(decoded).value());
}

auto JobRepository::list(std::optional<jb::core::Uuid> queue_id,
                         bool                          include_deleted,
                         std::optional<JobState>       state,
                         std::optional<JobType>        type,
                         std::size_t                   limit,
                         std::optional<jb::core::Uuid> after_id)
    -> jb::core::Result<std::vector<JobDefinition>, jb::core::Error>
{
    if (limit == 0 || limit > kMaximumListRows) {
        return RepositoryResult<std::vector<JobDefinition>>::failure(invalid_limit());
    }

    auto sql        = std::string{kJobSelection};
    auto has_where  = false;
    auto add_clause = [&sql, &has_where](std::string_view clause) {
        sql       += has_where ? " AND " : "WHERE ";
        sql       += clause;
        has_where  = true;
    };
    if (queue_id) {
        add_clause("queue_id = :queue_id");
    }
    if (!include_deleted) {
        add_clause("state <> 'deleted'");
    }
    if (state) {
        add_clause("state = :state");
    }
    if (type) {
        add_clause("type = :type");
    }
    if (after_id) {
        add_clause("id > :after_id");
    }
    sql += " ORDER BY id ASC LIMIT :limit";

    jb::db::Query query{_database};
    auto          prepared = query.prepare(sql);
    if (!prepared) {
        return RepositoryResult<std::vector<JobDefinition>>::failure(std::move(prepared).error());
    }
    if (queue_id) {
        auto bound = query.bind_value(":queue_id", uuid_to_storage(*queue_id));
        if (!bound) {
            return RepositoryResult<std::vector<JobDefinition>>::failure(std::move(bound).error());
        }
    }
    if (state) {
        auto bound = query.bind_value(":state", jb::db::make_text(storage_text(*state)));
        if (!bound) {
            return RepositoryResult<std::vector<JobDefinition>>::failure(std::move(bound).error());
        }
    }
    if (type) {
        auto bound = query.bind_value(":type", jb::db::make_text(storage_text(*type)));
        if (!bound) {
            return RepositoryResult<std::vector<JobDefinition>>::failure(std::move(bound).error());
        }
    }
    if (after_id) {
        auto bound = query.bind_value(":after_id", uuid_to_storage(*after_id));
        if (!bound) {
            return RepositoryResult<std::vector<JobDefinition>>::failure(std::move(bound).error());
        }
    }
    auto bound = query.bind_value(":limit", static_cast<std::int64_t>(limit));
    if (!bound) {
        return RepositoryResult<std::vector<JobDefinition>>::failure(std::move(bound).error());
    }
    auto executed = query.exec();
    if (!executed) {
        return RepositoryResult<std::vector<JobDefinition>>::failure(std::move(executed).error());
    }

    auto jobs = std::vector<JobDefinition>{};
    jobs.reserve(limit);
    while (true) {
        auto next = query.next();
        if (!next) {
            return RepositoryResult<std::vector<JobDefinition>>::failure(std::move(next).error());
        }
        if (!*next) {
            break;
        }
        auto decoded = decode_job(query.record(), _attributes);
        if (!decoded) {
            return RepositoryResult<std::vector<JobDefinition>>::failure(std::move(decoded).error());
        }
        jobs.push_back(std::move(decoded).value());
    }
    return RepositoryResult<std::vector<JobDefinition>>::success(std::move(jobs));
}

} // namespace jb::jobu::detail
