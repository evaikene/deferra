#include "queue_repository_priv.hpp"

#include "attribute_codec_priv.hpp"
#include "domain_storage_priv.hpp"
#include "query.hpp"
#include "queue_validation_priv.hpp"
#include "value.hpp"

#include <cstdint>
#include <string>
#include <utility>

namespace jb::jobu::detail {

namespace {

template <typename T>
using RepositoryResult = jb::core::Result<T, jb::core::Error>;

constexpr std::size_t kMaximumAttributeDocumentBytes = std::size_t{256} * 1024U;

constexpr auto kQueueSelection =
    "SELECT id AS queue_id, CASE WHEN state = 'deleted' THEN deleted_name ELSE name END AS queue_name, "
    "state AS queue_state, weight AS queue_weight, concurrency_limit AS queue_concurrency_limit, "
    "recovery_policy AS queue_recovery_policy, defaults_json AS queue_defaults_json, "
    "retention_seconds AS queue_retention_seconds, runnable_wait_warning_ms AS queue_runnable_wait_warning_ms, "
    "created_at_us AS queue_created_at_us, updated_at_us AS queue_updated_at_us, "
    "deleted_at_us AS queue_deleted_at_us FROM jobu_queues ";

auto name_conflict(jb::core::Error const& cause) -> jb::core::Error
{
    auto detail = "cause=" + cause.code;
    if (!cause.detail.empty()) {
        detail += " " + cause.detail;
    }
    return {
        .category = jb::core::ErrorCategory::Conflict,
        .code     = "jobu.queue.name_conflict",
        .message  = "A queue with that name already exists",
        .detail   = std::move(detail),
    };
}

auto ambiguous_deleted_name() -> jb::core::Error
{
    return {
        .category = jb::core::ErrorCategory::Conflict,
        .code     = "jobu.queue.ambiguous_deleted_name",
        .message  = "The deleted queue name is ambiguous; select the queue by ID",
    };
}

auto invalid_queue_row(std::string_view reason) -> jb::core::Error
{
    return {
        .category = jb::core::ErrorCategory::Internal,
        .code     = "jobu.storage.invalid_queue",
        .message  = "Persisted queue data is invalid",
        .detail   = "reason=" + std::string{reason},
    };
}

auto decode_queue(jb::db::Record const& record, AttributeRegistry const& attributes) -> RepositoryResult<Queue>
{
    auto id = read_uuid(record, "queue_id");
    if (!id) {
        return RepositoryResult<Queue>::failure(std::move(id).error());
    }
    auto name = read_text(record, "queue_name");
    if (!name) {
        return RepositoryResult<Queue>::failure(std::move(name).error());
    }
    if (!is_valid_queue_name(*name)) {
        return RepositoryResult<Queue>::failure(invalid_queue_row("invalid_name"));
    }
    auto state = read_queue_state(record, "queue_state");
    if (!state) {
        return RepositoryResult<Queue>::failure(std::move(state).error());
    }
    auto weight = read_positive_uint32(record, "queue_weight");
    if (!weight) {
        return RepositoryResult<Queue>::failure(std::move(weight).error());
    }
    auto concurrency = read_positive_uint32(record, "queue_concurrency_limit");
    if (!concurrency) {
        return RepositoryResult<Queue>::failure(std::move(concurrency).error());
    }
    auto recovery = read_recovery_policy(record, "queue_recovery_policy");
    if (!recovery) {
        return RepositoryResult<Queue>::failure(std::move(recovery).error());
    }
    auto defaults_json = read_json(record, "queue_defaults_json", true, kMaximumAttributeDocumentBytes);
    if (!defaults_json) {
        return RepositoryResult<Queue>::failure(std::move(defaults_json).error());
    }
    auto defaults = decode_attribute_document(attributes,
                                              *defaults_json,
                                              AttributeScope::QueueDefault,
                                              AttributeDocumentMode::Partial);
    if (!defaults) {
        return RepositoryResult<Queue>::failure(std::move(defaults).error());
    }
    auto retention = read_optional_nonnegative_seconds(record, "queue_retention_seconds");
    if (!retention) {
        return RepositoryResult<Queue>::failure(std::move(retention).error());
    }
    auto warning = read_nonnegative_milliseconds(record, "queue_runnable_wait_warning_ms");
    if (!warning) {
        return RepositoryResult<Queue>::failure(std::move(warning).error());
    }
    auto created = read_timestamp(record, "queue_created_at_us");
    if (!created) {
        return RepositoryResult<Queue>::failure(std::move(created).error());
    }
    auto updated = read_timestamp(record, "queue_updated_at_us");
    if (!updated) {
        return RepositoryResult<Queue>::failure(std::move(updated).error());
    }
    auto deleted = read_optional_timestamp(record, "queue_deleted_at_us");
    if (!deleted) {
        return RepositoryResult<Queue>::failure(std::move(deleted).error());
    }
    if ((*state == QueueState::Deleted) != deleted->has_value()) {
        return RepositoryResult<Queue>::failure(invalid_queue_row("deleted_state_mismatch"));
    }

    return RepositoryResult<Queue>::success(Queue{
        .id                    = *id,
        .name                  = std::move(name).value(),
        .state                 = *state,
        .weight                = *weight,
        .concurrency_limit     = *concurrency,
        .recovery_policy       = *recovery,
        .defaults              = std::move(defaults).value(),
        .history_retention     = *retention,
        .runnable_wait_warning = *warning,
        .created_at            = *created,
        .updated_at            = *updated,
        .deleted_at            = *deleted,
    });
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

} // anonymous namespace

QueueRepository::QueueRepository(jb::db::Database& database, AttributeRegistry const& attributes) noexcept
    : _database{database}
    , _attributes{attributes}
{}

auto QueueRepository::insert(Queue const&                       queue,
                             std::string_view                   internal_name,
                             SerializedAttributeDocument const& defaults) -> jb::core::Result<void, jb::core::Error>
{
    auto weight = positive_uint32_to_storage(queue.weight);
    if (!weight) {
        return RepositoryResult<void>::failure(std::move(weight).error());
    }
    auto concurrency = positive_uint32_to_storage(queue.concurrency_limit);
    if (!concurrency) {
        return RepositoryResult<void>::failure(std::move(concurrency).error());
    }
    auto retention = optional_nonnegative_seconds_to_storage(queue.history_retention);
    if (!retention) {
        return RepositoryResult<void>::failure(std::move(retention).error());
    }
    auto warning = nonnegative_milliseconds_to_storage(queue.runnable_wait_warning);
    if (!warning) {
        return RepositoryResult<void>::failure(std::move(warning).error());
    }
    auto created = timestamp_to_storage(queue.created_at);
    if (!created) {
        return RepositoryResult<void>::failure(std::move(created).error());
    }
    auto updated = timestamp_to_storage(queue.updated_at);
    if (!updated) {
        return RepositoryResult<void>::failure(std::move(updated).error());
    }

    jb::db::Query query{_database};
    auto          prepared = query.prepare(
        "INSERT INTO jobu_queues(id, name, deleted_name, state, weight, concurrency_limit, recovery_policy, "
        "defaults_json, retention_seconds, runnable_wait_warning_ms, created_at_us, updated_at_us, deleted_at_us) "
        "VALUES(:id, :name, :deleted_name, :state, :weight, :concurrency_limit, :recovery_policy, :defaults_json, "
        ":retention_seconds, :runnable_wait_warning_ms, :created_at_us, :updated_at_us, :deleted_at_us)");
    if (!prepared) {
        return RepositoryResult<void>::failure(std::move(prepared).error());
    }
    auto bound = bind_all(query,
                          {
                              {":id",                       uuid_to_storage(queue.id)                             },
                              {":name",                     jb::db::make_text(internal_name)                      },
                              {":deleted_name",             jb::db::Null{}                                        },
                              {":state",                    jb::db::make_text(storage_text(queue.state))          },
                              {":weight",                   std::move(weight).value()                             },
                              {":concurrency_limit",        std::move(concurrency).value()                        },
                              {":recovery_policy",          jb::db::make_text(storage_text(queue.recovery_policy))},
                              {":defaults_json",            jb::db::make_text(defaults.serialized())              },
                              {":retention_seconds",        std::move(retention).value()                          },
                              {":runnable_wait_warning_ms", std::move(warning).value()                            },
                              {":created_at_us",            std::move(created).value()                            },
                              {":updated_at_us",            std::move(updated).value()                            },
                              {":deleted_at_us",            jb::db::Null{}                                        },
    });
    if (!bound) {
        return bound;
    }
    auto executed = query.exec();
    if (!executed && executed.error().code == "db.constraint.unique") {
        return RepositoryResult<void>::failure(name_conflict(executed.error()));
    }
    return executed;
}

auto QueueRepository::find_by_id(jb::core::Uuid const& id, bool include_deleted)
    -> jb::core::Result<std::optional<Queue>, jb::core::Error>
{
    auto sql = std::string{kQueueSelection} + "WHERE id = :id";
    if (!include_deleted) {
        sql += " AND state <> 'deleted'";
    }
    jb::db::Query query{_database};
    auto          prepared = query.prepare(sql);
    if (!prepared) {
        return RepositoryResult<std::optional<Queue>>::failure(std::move(prepared).error());
    }
    auto bound = query.bind_value(":id", uuid_to_storage(id));
    if (!bound) {
        return RepositoryResult<std::optional<Queue>>::failure(std::move(bound).error());
    }
    auto executed = query.exec();
    if (!executed) {
        return RepositoryResult<std::optional<Queue>>::failure(std::move(executed).error());
    }
    auto next = query.next();
    if (!next) {
        return RepositoryResult<std::optional<Queue>>::failure(std::move(next).error());
    }
    if (!*next) {
        return RepositoryResult<std::optional<Queue>>::success(std::nullopt);
    }
    auto decoded = decode_queue(query.record(), _attributes);
    if (!decoded) {
        return RepositoryResult<std::optional<Queue>>::failure(std::move(decoded).error());
    }
    return RepositoryResult<std::optional<Queue>>::success(std::move(decoded).value());
}

auto QueueRepository::find_by_name(std::string_view name, bool include_deleted)
    -> jb::core::Result<std::optional<Queue>, jb::core::Error>
{
    jb::db::Query query{_database};
    auto          sql = std::string{kQueueSelection};
    if (include_deleted) {
        sql += "WHERE (state <> 'deleted' AND name = :name) OR (state = 'deleted' AND deleted_name = :name) "
               "ORDER BY CASE WHEN state = 'deleted' THEN 1 ELSE 0 END ASC, id ASC LIMIT 2";
    }
    else {
        sql += "WHERE state <> 'deleted' AND name = :name";
    }
    auto prepared = query.prepare(sql);
    if (!prepared) {
        return RepositoryResult<std::optional<Queue>>::failure(std::move(prepared).error());
    }
    auto bound = query.bind_value(":name", jb::db::make_text(name));
    if (!bound) {
        return RepositoryResult<std::optional<Queue>>::failure(std::move(bound).error());
    }
    auto executed = query.exec();
    if (!executed) {
        return RepositoryResult<std::optional<Queue>>::failure(std::move(executed).error());
    }
    auto next = query.next();
    if (!next) {
        return RepositoryResult<std::optional<Queue>>::failure(std::move(next).error());
    }
    if (!*next) {
        return RepositoryResult<std::optional<Queue>>::success(std::nullopt);
    }
    auto decoded = decode_queue(query.record(), _attributes);
    if (!decoded) {
        return RepositoryResult<std::optional<Queue>>::failure(std::move(decoded).error());
    }
    if (include_deleted && decoded->state == QueueState::Deleted) {
        auto second = query.next();
        if (!second) {
            return RepositoryResult<std::optional<Queue>>::failure(std::move(second).error());
        }
        if (*second) {
            return RepositoryResult<std::optional<Queue>>::failure(ambiguous_deleted_name());
        }
    }
    return RepositoryResult<std::optional<Queue>>::success(std::move(decoded).value());
}

auto QueueRepository::list(bool                          include_deleted,
                           std::optional<QueueState>     state,
                           std::size_t                   limit,
                           std::optional<jb::core::Uuid> after_id)
    -> jb::core::Result<std::vector<Queue>, jb::core::Error>
{
    auto sql        = std::string{kQueueSelection};
    auto has_where  = false;
    auto add_clause = [&sql, &has_where](std::string_view clause) {
        sql       += has_where ? " AND " : "WHERE ";
        sql       += clause;
        has_where  = true;
    };
    if (!include_deleted) {
        add_clause("state <> 'deleted'");
    }
    if (state) {
        add_clause("state = :state");
    }
    if (after_id) {
        add_clause("id > :after_id");
    }
    sql += " ORDER BY id ASC LIMIT :limit";

    jb::db::Query query{_database};
    auto          prepared = query.prepare(sql);
    if (!prepared) {
        return RepositoryResult<std::vector<Queue>>::failure(std::move(prepared).error());
    }
    if (state) {
        auto bound = query.bind_value(":state", jb::db::make_text(storage_text(*state)));
        if (!bound) {
            return RepositoryResult<std::vector<Queue>>::failure(std::move(bound).error());
        }
    }
    if (after_id) {
        auto bound = query.bind_value(":after_id", uuid_to_storage(*after_id));
        if (!bound) {
            return RepositoryResult<std::vector<Queue>>::failure(std::move(bound).error());
        }
    }
    auto bound = query.bind_value(":limit", static_cast<std::int64_t>(limit));
    if (!bound) {
        return RepositoryResult<std::vector<Queue>>::failure(std::move(bound).error());
    }
    auto executed = query.exec();
    if (!executed) {
        return RepositoryResult<std::vector<Queue>>::failure(std::move(executed).error());
    }

    auto queues = std::vector<Queue>{};
    queues.reserve(limit);
    while (true) {
        auto next = query.next();
        if (!next) {
            return RepositoryResult<std::vector<Queue>>::failure(std::move(next).error());
        }
        if (!*next) {
            break;
        }
        auto decoded = decode_queue(query.record(), _attributes);
        if (!decoded) {
            return RepositoryResult<std::vector<Queue>>::failure(std::move(decoded).error());
        }
        queues.push_back(std::move(decoded).value());
    }
    return RepositoryResult<std::vector<Queue>>::success(std::move(queues));
}

auto QueueRepository::replace_mutable_fields(Queue const& queue, SerializedAttributeDocument const* defaults)
    -> jb::core::Result<bool, jb::core::Error>
{
    auto weight = positive_uint32_to_storage(queue.weight);
    if (!weight) {
        return RepositoryResult<bool>::failure(std::move(weight).error());
    }
    auto concurrency = positive_uint32_to_storage(queue.concurrency_limit);
    if (!concurrency) {
        return RepositoryResult<bool>::failure(std::move(concurrency).error());
    }
    auto retention = optional_nonnegative_seconds_to_storage(queue.history_retention);
    if (!retention) {
        return RepositoryResult<bool>::failure(std::move(retention).error());
    }
    auto warning = nonnegative_milliseconds_to_storage(queue.runnable_wait_warning);
    if (!warning) {
        return RepositoryResult<bool>::failure(std::move(warning).error());
    }
    auto updated = timestamp_to_storage(queue.updated_at);
    if (!updated) {
        return RepositoryResult<bool>::failure(std::move(updated).error());
    }

    auto const* const sql = defaults == nullptr
                              ? "UPDATE jobu_queues SET name = :name, weight = :weight, "
                                "concurrency_limit = :concurrency_limit, recovery_policy = :recovery_policy, "
                                "retention_seconds = :retention_seconds, "
                                "runnable_wait_warning_ms = :runnable_wait_warning_ms, updated_at_us = :updated_at_us "
                                "WHERE id = :id AND state <> 'deleted'"
                              : "UPDATE jobu_queues SET name = :name, weight = :weight, "
                                "concurrency_limit = :concurrency_limit, recovery_policy = :recovery_policy, "
                                "defaults_json = :defaults_json, retention_seconds = :retention_seconds, "
                                "runnable_wait_warning_ms = :runnable_wait_warning_ms, updated_at_us = :updated_at_us "
                                "WHERE id = :id AND state <> 'deleted'";
    jb::db::Query     query{_database};
    auto              prepared = query.prepare(sql);
    if (!prepared) {
        return RepositoryResult<bool>::failure(std::move(prepared).error());
    }
    auto bound = bind_all(query,
                          {
                              {":name",                     jb::db::make_text(queue.name)                         },
                              {":weight",                   std::move(weight).value()                             },
                              {":concurrency_limit",        std::move(concurrency).value()                        },
                              {":recovery_policy",          jb::db::make_text(storage_text(queue.recovery_policy))},
                              {":retention_seconds",        std::move(retention).value()                          },
                              {":runnable_wait_warning_ms", std::move(warning).value()                            },
                              {":updated_at_us",            std::move(updated).value()                            },
                              {":id",                       uuid_to_storage(queue.id)                             },
    });
    if (!bound) {
        return RepositoryResult<bool>::failure(std::move(bound).error());
    }
    if (defaults != nullptr) {
        bound = query.bind_value(":defaults_json", jb::db::make_text(defaults->serialized()));
        if (!bound) {
            return RepositoryResult<bool>::failure(std::move(bound).error());
        }
    }
    auto executed = query.exec();
    if (!executed && executed.error().code == "db.constraint.unique") {
        return RepositoryResult<bool>::failure(name_conflict(executed.error()));
    }
    if (!executed) {
        return RepositoryResult<bool>::failure(std::move(executed).error());
    }
    return RepositoryResult<bool>::success(query.num_rows_affected() == 1);
}

auto QueueRepository::set_state(jb::core::Uuid const&  id,
                                QueueState             expected_state,
                                QueueState             next_state,
                                jb::core::UtcTimePoint updated_at) -> jb::core::Result<bool, jb::core::Error>
{
    if (expected_state == QueueState::Deleted || next_state == QueueState::Deleted || expected_state == next_state ||
        storage_text(expected_state).empty() || storage_text(next_state).empty()) {
        return RepositoryResult<bool>::failure(invalid_queue_row("invalid_state_transition"));
    }
    auto updated = timestamp_to_storage(updated_at);
    if (!updated) {
        return RepositoryResult<bool>::failure(std::move(updated).error());
    }

    jb::db::Query query{_database};
    auto          prepared = query.prepare("UPDATE jobu_queues SET state = :next_state, updated_at_us = :updated_at_us "
                                           "WHERE id = :id AND state = :expected_state AND deleted_at_us IS NULL");
    if (!prepared) {
        return RepositoryResult<bool>::failure(std::move(prepared).error());
    }
    auto bound = bind_all(query,
                          {
                              {":next_state",     jb::db::make_text(storage_text(next_state))    },
                              {":updated_at_us",  std::move(updated).value()                     },
                              {":id",             uuid_to_storage(id)                            },
                              {":expected_state", jb::db::make_text(storage_text(expected_state))},
    });
    if (!bound) {
        return RepositoryResult<bool>::failure(std::move(bound).error());
    }
    auto executed = query.exec();
    if (!executed) {
        return RepositoryResult<bool>::failure(std::move(executed).error());
    }
    return RepositoryResult<bool>::success(query.num_rows_affected() == 1);
}

auto QueueRepository::count_non_deleted_jobs(jb::core::Uuid const& queue_id)
    -> jb::core::Result<std::size_t, jb::core::Error>
{
    jb::db::Query query{_database};
    auto          prepared =
        query.prepare("SELECT COUNT(*) AS job_count FROM jobu_jobs WHERE queue_id = :queue_id AND state <> 'deleted'");
    if (!prepared) {
        return RepositoryResult<std::size_t>::failure(std::move(prepared).error());
    }
    auto bound = query.bind_value(":queue_id", uuid_to_storage(queue_id));
    if (!bound) {
        return RepositoryResult<std::size_t>::failure(std::move(bound).error());
    }
    auto executed = query.exec();
    if (!executed) {
        return RepositoryResult<std::size_t>::failure(std::move(executed).error());
    }
    auto next = query.next();
    if (!next) {
        return RepositoryResult<std::size_t>::failure(std::move(next).error());
    }
    if (!*next) {
        return RepositoryResult<std::size_t>::failure(invalid_queue_row("missing_job_count"));
    }
    auto const* value = query.record().value("job_count");
    auto const* count = value == nullptr ? nullptr : std::get_if<std::int64_t>(value);
    if (count == nullptr || *count < 0 || !std::in_range<std::size_t>(*count)) {
        return RepositoryResult<std::size_t>::failure(invalid_queue_row("invalid_job_count"));
    }
    return RepositoryResult<std::size_t>::success(static_cast<std::size_t>(*count));
}

auto QueueRepository::mark_deleted(jb::core::Uuid const&  id,
                                   std::string_view       internal_name,
                                   std::string_view       original_name,
                                   jb::core::UtcTimePoint deleted_at) -> jb::core::Result<bool, jb::core::Error>
{
    auto timestamp = timestamp_to_storage(deleted_at);
    if (!timestamp) {
        return RepositoryResult<bool>::failure(std::move(timestamp).error());
    }

    jb::db::Query query{_database};
    auto          prepared =
        query.prepare("UPDATE jobu_queues SET name = :internal_name, deleted_name = :original_name, state = 'deleted', "
                      "updated_at_us = :updated_at_us, deleted_at_us = :deleted_at_us "
                      "WHERE id = :id AND state = 'suspended' AND deleted_at_us IS NULL");
    if (!prepared) {
        return RepositoryResult<bool>::failure(std::move(prepared).error());
    }
    auto bound = bind_all(query,
                          {
                              {":internal_name", jb::db::make_text(internal_name)},
                              {":original_name", jb::db::make_text(original_name)},
                              {":updated_at_us", timestamp.value()               },
                              {":deleted_at_us", timestamp.value()               },
                              {":id",            uuid_to_storage(id)             },
    });
    if (!bound) {
        return RepositoryResult<bool>::failure(std::move(bound).error());
    }
    auto executed = query.exec();
    if (!executed) {
        return RepositoryResult<bool>::failure(std::move(executed).error());
    }
    return RepositoryResult<bool>::success(query.num_rows_affected() == 1);
}

} // namespace jb::jobu::detail
