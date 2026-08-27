#include "management.hpp"

#include "attempt_repository_priv.hpp"
#include "attribute_codec_priv.hpp"
#include "attribute_registry.hpp"
#include "cron.hpp"
#include "idempotency_codec_priv.hpp"
#include "idempotency_repository_priv.hpp"
#include "job_repository_priv.hpp"
#include "job_validation_priv.hpp"
#include "json.hpp"
#include "queue_repository_priv.hpp"
#include "queue_validation_priv.hpp"
#include "run_repository_priv.hpp"
#include "secret_repository_priv.hpp"
#include "transaction.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>

namespace jb::jobu {

namespace {

template <typename T>
using ServiceResult = jb::core::Result<T, jb::core::Error>;

constexpr std::size_t kMaximumAttributeDocumentBytes = std::size_t{256} * 1024U;
constexpr std::size_t kMaximumPageSize               = 200;
// JobRevision is unsigned publicly, but schema version 1 stores revisions in a positive signed 64-bit INTEGER.
constexpr JobRevision kMaximumPersistedJobRevision = static_cast<JobRevision>(std::numeric_limits<std::int64_t>::max());

auto service_error(jb::core::ErrorCategory category, std::string_view code, std::string_view message) -> jb::core::Error
{
    return {
        .category = category,
        .code     = std::string{code},
        .message  = std::string{message},
    };
}

auto invalid_name() -> jb::core::Error
{
    return service_error(jb::core::ErrorCategory::InvalidArgument,
                         "jobu.queue.invalid_name",
                         "Queue name must contain 1 through 128 bytes of valid UTF-8, no ASCII control characters, "
                         "and no reserved deletion suffix");
}

auto invalid_configuration(std::string_view reason) -> jb::core::Error
{
    auto error   = service_error(jb::core::ErrorCategory::InvalidArgument,
                                 "jobu.queue.invalid_configuration",
                                 "Queue configuration is invalid");
    error.detail = "reason=" + std::string{reason};
    return error;
}

auto queue_not_found() -> jb::core::Error
{
    return service_error(jb::core::ErrorCategory::NotFound, "jobu.queue.not_found", "Queue was not found");
}

auto queue_name_conflict() -> jb::core::Error
{
    return service_error(jb::core::ErrorCategory::Conflict,
                         "jobu.queue.name_conflict",
                         "A queue with that name already exists");
}

auto queue_state_conflict() -> jb::core::Error
{
    return service_error(jb::core::ErrorCategory::Conflict,
                         "jobu.queue.state_conflict",
                         "Queue state changed incompatibly during the operation");
}

auto queue_not_suspended() -> jb::core::Error
{
    return service_error(jb::core::ErrorCategory::Conflict,
                         "jobu.queue.not_suspended",
                         "Queue must be fully suspended for this operation");
}

auto queue_has_running_attempt() -> jb::core::Error
{
    return service_error(jb::core::ErrorCategory::Conflict,
                         "jobu.queue.has_running_attempt",
                         "Queue cannot be deleted while work is running");
}

auto invalid_job_name() -> jb::core::Error
{
    return service_error(jb::core::ErrorCategory::InvalidArgument,
                         "jobu.job.invalid_name",
                         "Job name must contain at most 256 valid UTF-8 bytes and no ASCII control characters");
}

auto invalid_job_configuration(std::string_view reason) -> jb::core::Error
{
    auto error   = service_error(jb::core::ErrorCategory::InvalidArgument,
                                 "jobu.job.invalid_configuration",
                                 "Job configuration is invalid");
    error.detail = "reason=" + std::string{reason};
    return error;
}

auto invalid_job_payload(detail::JobPayloadIssue issue) -> jb::core::Error
{
    if (issue == detail::JobPayloadIssue::TooLarge) {
        auto error   = service_error(jb::core::ErrorCategory::ResourceExhausted,
                                     "jobu.protocol.value_too_large",
                                     "Job payload exceeds its size limit");
        error.detail = "reason=" + std::string{detail::job_payload_issue_text(issue)};
        return error;
    }
    auto error   = service_error(jb::core::ErrorCategory::InvalidArgument,
                                 "jobu.job.invalid_payload",
                                 "Job runner type or payload is structurally invalid");
    error.detail = "reason=" + std::string{detail::job_payload_issue_text(issue)};
    return error;
}

auto job_not_found() -> jb::core::Error
{
    return service_error(jb::core::ErrorCategory::NotFound, "jobu.job.not_found", "Job was not found");
}

auto job_deleted() -> jb::core::Error
{
    return service_error(jb::core::ErrorCategory::Conflict, "jobu.job.deleted", "Job is deleted");
}

auto job_revision_conflict() -> jb::core::Error
{
    return service_error(jb::core::ErrorCategory::Conflict,
                         "jobu.job.revision_conflict",
                         "Job revision does not match the expected revision");
}

auto job_revision_exhausted() -> jb::core::Error
{
    return service_error(jb::core::ErrorCategory::ResourceExhausted,
                         "jobu.job.revision_exhausted",
                         "Job revision cannot be incremented");
}

auto job_state_conflict() -> jb::core::Error
{
    return service_error(jb::core::ErrorCategory::Conflict,
                         "jobu.job.state_conflict",
                         "Job state changed incompatibly during the operation");
}

auto job_not_suspended() -> jb::core::Error
{
    return service_error(jb::core::ErrorCategory::Conflict,
                         "jobu.job.not_suspended",
                         "Job must be fully suspended for this operation");
}

auto job_has_running_attempt() -> jb::core::Error
{
    return service_error(jb::core::ErrorCategory::Conflict,
                         "jobu.job.has_running_attempt",
                         "Job cannot be deleted while work is running");
}

auto storage_invariant(std::string_view reason) -> jb::core::Error
{
    auto error   = service_error(jb::core::ErrorCategory::Internal,
                                 "jobu.storage.invariant",
                                 "Persisted JobU data violates a cross-table invariant");
    error.detail = "reason=" + std::string{reason};
    return error;
}

auto job_immutable() -> jb::core::Error
{
    return service_error(jb::core::ErrorCategory::Conflict,
                         "jobu.job.immutable",
                         "A one-time job cannot be changed after an attempt starts");
}

auto schedule_refresh_conflict() -> jb::core::Error
{
    return service_error(jb::core::ErrorCategory::Conflict,
                         "jobu.run.schedule_conflict",
                         "The pending schedule-owned run could not be refreshed");
}

auto manual_run_conflict() -> jb::core::Error
{
    return service_error(jb::core::ErrorCategory::Conflict,
                         "jobu.run.manual_conflict",
                         "Run Now preconditions are not satisfied");
}

auto invalid_idempotency_key() -> jb::core::Error
{
    return service_error(jb::core::ErrorCategory::InvalidArgument,
                         "jobu.idempotency.invalid_key",
                         "Idempotency key must contain 1 through 128 valid UTF-8 bytes");
}

auto idempotency_conflict() -> jb::core::Error
{
    return service_error(jb::core::ErrorCategory::Conflict,
                         "jobu.idempotency.conflict",
                         "The idempotency key was already used for a different request");
}

auto invalid_idempotency_record(std::string_view reason) -> jb::core::Error
{
    auto error   = service_error(jb::core::ErrorCategory::Internal,
                                 "jobu.idempotency.invalid_record",
                                 "Stored idempotency data is invalid");
    error.detail = "reason=" + std::string{reason};
    return error;
}

auto validate_name(std::string_view name) -> ServiceResult<void>
{
    if (!detail::is_valid_queue_name(name)) {
        return ServiceResult<void>::failure(invalid_name());
    }
    return ServiceResult<void>::success();
}

auto validate_idempotency_key(std::optional<std::string> const& key) -> ServiceResult<void>
{
    if (key && !detail::is_valid_idempotency_key(*key)) {
        return ServiceResult<void>::failure(invalid_idempotency_key());
    }
    return ServiceResult<void>::success();
}

auto validate_job_name(std::optional<std::string> const& name) -> ServiceResult<void>
{
    if (name && !detail::is_valid_job_name(*name)) {
        return ServiceResult<void>::failure(invalid_job_name());
    }
    return ServiceResult<void>::success();
}

auto validate_job_payload(JobType type, jb::core::JsonValue const& payload)
    -> ServiceResult<detail::ValidatedJobPayload>
{
    auto validated = detail::validate_and_serialize_job_payload(type, payload);
    if (!validated) {
        return ServiceResult<detail::ValidatedJobPayload>::failure(invalid_job_payload(validated.error()));
    }
    return ServiceResult<detail::ValidatedJobPayload>::success(std::move(validated).value());
}

auto attribute_document_size_message(AttributeScope scope) noexcept -> std::string_view
{
    switch (scope) {
        case AttributeScope::DaemonDefault:
            return "Daemon default attribute document exceeds its size limit";
        case AttributeScope::QueueDefault:
            return "Queue attribute document exceeds its size limit";
        case AttributeScope::Job:
            return "Job attribute document exceeds its size limit";
    }
    return "Attribute document exceeds its size limit";
}

auto serialize_attributes(AttributeRegistry const&      attributes,
                          AttributeSet const&           values,
                          AttributeScope                scope,
                          detail::AttributeDocumentMode mode) -> ServiceResult<detail::SerializedAttributeDocument>
{
    auto serialized = detail::encode_and_serialize_attribute_document(attributes, values, scope, mode);
    if (!serialized) {
        return ServiceResult<detail::SerializedAttributeDocument>::failure(std::move(serialized).error());
    }
    if (serialized->serialized().size() > kMaximumAttributeDocumentBytes) {
        return ServiceResult<detail::SerializedAttributeDocument>::failure(
            service_error(jb::core::ErrorCategory::ResourceExhausted,
                          "jobu.protocol.value_too_large",
                          attribute_document_size_message(scope)));
    }
    return ServiceResult<detail::SerializedAttributeDocument>::success(std::move(serialized).value());
}

auto validate_attributes(AttributeRegistry const& attributes, AttributeSet const& values, AttributeScope scope)
    -> ServiceResult<detail::SerializedAttributeDocument>
{
    return serialize_attributes(attributes, values, scope, detail::AttributeDocumentMode::Partial);
}

auto valid_recovery_policy(RecoveryPolicy value) noexcept -> bool
{
    return value == RecoveryPolicy::FailInterrupted || value == RecoveryPolicy::RetryInterrupted;
}

auto valid_queue_state(QueueState value) noexcept -> bool
{
    return value == QueueState::Active || value == QueueState::Suspending || value == QueueState::Suspended ||
           value == QueueState::Deleted;
}

auto valid_job_state(JobState value) noexcept -> bool
{
    return value == JobState::Active || value == JobState::Suspending || value == JobState::Suspended ||
           value == JobState::Deleted;
}

auto valid_job_type(JobType value) noexcept -> bool
{
    return value == JobType::Cli || value == JobType::Http;
}

auto validate_configuration(std::uint32_t                       weight,
                            std::uint32_t                       concurrency_limit,
                            RecoveryPolicy                      recovery_policy,
                            std::optional<std::chrono::seconds> history_retention,
                            std::chrono::milliseconds           runnable_wait_warning) -> ServiceResult<void>
{
    if (weight == 0) {
        return ServiceResult<void>::failure(invalid_configuration("weight_not_positive"));
    }
    if (concurrency_limit == 0) {
        return ServiceResult<void>::failure(invalid_configuration("concurrency_limit_not_positive"));
    }
    if (!valid_recovery_policy(recovery_policy)) {
        return ServiceResult<void>::failure(invalid_configuration("unknown_recovery_policy"));
    }
    if (history_retention && history_retention->count() < 0) {
        return ServiceResult<void>::failure(invalid_configuration("negative_history_retention"));
    }
    if (runnable_wait_warning.count() < 0) {
        return ServiceResult<void>::failure(invalid_configuration("negative_runnable_wait_warning"));
    }
    return ServiceResult<void>::success();
}

auto validate_selector(QueueSelector const& selector) -> ServiceResult<void>
{
    if (auto const* name = std::get_if<std::string>(&selector)) {
        return validate_name(*name);
    }
    return ServiceResult<void>::success();
}

auto find_queue(detail::QueueRepository& repository, QueueSelector const& selector, bool include_deleted)
    -> ServiceResult<std::optional<Queue>>
{
    if (auto const* id = std::get_if<jb::core::Uuid>(&selector)) {
        return repository.find_by_id(*id, include_deleted);
    }
    return repository.find_by_name(std::get<std::string>(selector), include_deleted);
}

auto is_empty_update(UpdateQueueRequest const& request) noexcept -> bool
{
    return !request.name && !request.weight && !request.concurrency_limit && !request.recovery_policy &&
           !request.defaults && !request.history_retention && !request.runnable_wait_warning;
}

auto is_empty_update(UpdateJobRequest const& request) noexcept -> bool
{
    return !request.name && !request.type && !request.schedule && !request.priority &&
           request.attribute_changes.empty() && !request.payload;
}

} // anonymous namespace

struct ManagementService::Private {
    Private(jb::db::Database&        database_value,
            AttributeRegistry const& attributes_value,
            CronEngine const&        cron_value,
            jb::core::UuidGenerator& uuid_generator_value,
            jb::core::TimeSource&    time_source_value,
            AttributeSet             daemon_defaults_value)
        : database{database_value}
        , attributes{attributes_value}
        , cron{cron_value}
        , uuid_generator{uuid_generator_value}
        , time_source{time_source_value}
        , daemon_defaults{std::move(daemon_defaults_value)}
        , queues{database_value, attributes_value}
        , jobs{database_value, attributes_value}
        , runs{database_value, attributes_value}
        , attempts{database_value}
        , idempotency{database_value}
        , secrets{database_value}
    {
        auto validated = validate_attributes(attributes, daemon_defaults, AttributeScope::DaemonDefault);
        if (!validated) {
            initialization_error = std::move(validated).error();
        }
    }

    jb::db::Database&              database;
    AttributeRegistry const&       attributes;
    CronEngine const&              cron;
    jb::core::UuidGenerator&       uuid_generator;
    jb::core::TimeSource&          time_source;
    AttributeSet                   daemon_defaults;
    detail::QueueRepository        queues;
    detail::JobRepository          jobs;
    detail::RunRepository          runs;
    detail::AttemptRepository      attempts;
    detail::IdempotencyRepository  idempotency;
    detail::SecretRepository       secrets;
    std::optional<jb::core::Error> initialization_error;
};

ManagementService::ManagementService(jb::db::Database&        database,
                                     AttributeRegistry const& attributes,
                                     CronEngine const&        cron,
                                     jb::core::UuidGenerator& uuid_generator,
                                     jb::core::TimeSource&    time_source,
                                     AttributeSet             daemon_defaults)
    : _data{std::make_unique<Private>(database,
                                      attributes,
                                      cron,
                                      uuid_generator,
                                      time_source,
                                      std::move(daemon_defaults))}
{}

ManagementService::~ManagementService() = default;

auto ManagementService::create_queue(CreateQueueRequest request) -> jb::core::Result<Queue, jb::core::Error>
{
    if (_data->initialization_error) {
        return ServiceResult<Queue>::failure(*_data->initialization_error);
    }
    auto name = validate_name(request.name);
    if (!name) {
        return ServiceResult<Queue>::failure(std::move(name).error());
    }
    auto configuration = validate_configuration(request.weight,
                                                request.concurrency_limit,
                                                request.recovery_policy,
                                                request.history_retention,
                                                request.runnable_wait_warning);
    if (!configuration) {
        return ServiceResult<Queue>::failure(std::move(configuration).error());
    }
    auto defaults = validate_attributes(_data->attributes, request.defaults, AttributeScope::QueueDefault);
    if (!defaults) {
        return ServiceResult<Queue>::failure(std::move(defaults).error());
    }
    auto idempotency = validate_idempotency_key(request.idempotency_key);
    if (!idempotency) {
        return ServiceResult<Queue>::failure(std::move(idempotency).error());
    }
    auto canonical_request = std::optional<std::string>{};
    if (request.idempotency_key) {
        auto encoded = detail::encode_queue_create_idempotency_request(request, _data->attributes);
        if (!encoded) {
            return ServiceResult<Queue>::failure(std::move(encoded).error());
        }
        canonical_request = std::move(encoded).value();
    }

    auto transaction = std::optional<jb::db::Transaction>{};
    if (request.idempotency_key) {
        auto begun = jb::db::Transaction::begin(_data->database);
        if (!begun) {
            return ServiceResult<Queue>::failure(std::move(begun).error());
        }
        transaction.emplace(std::move(begun).value());

        auto record = _data->idempotency.find("queue.create", jb::core::Uuid{}, *request.idempotency_key);
        if (!record) {
            return ServiceResult<Queue>::failure(std::move(record).error());
        }
        if (record->has_value()) {
            auto valid = detail::validate_queue_create_idempotency_request((**record).request_json, _data->attributes);
            if (!valid) {
                return ServiceResult<Queue>::failure(std::move(valid).error());
            }
            if ((**record).request_json != *canonical_request) {
                return ServiceResult<Queue>::failure(idempotency_conflict());
            }
            auto replay = detail::decode_queue_idempotency_result((**record).result_json, _data->attributes);
            if (!replay) {
                return ServiceResult<Queue>::failure(std::move(replay).error());
            }
            if (replay->id != (**record).resource_id) {
                return ServiceResult<Queue>::failure(invalid_idempotency_record("queue_resource_id_mismatch"));
            }
            auto committed = transaction->commit();
            if (!committed) {
                return ServiceResult<Queue>::failure(std::move(committed).error());
            }
            return ServiceResult<Queue>::success(std::move(replay).value());
        }
    }

    auto id = _data->uuid_generator.generate();
    if (!id) {
        return ServiceResult<Queue>::failure(std::move(id).error());
    }
    auto const now   = _data->time_source.utc_now();
    auto       queue = Queue{
        .id                    = *id,
        .name                  = std::move(request.name),
        .state                 = QueueState::Active,
        .weight                = request.weight,
        .concurrency_limit     = request.concurrency_limit,
        .recovery_policy       = request.recovery_policy,
        .defaults              = std::move(request.defaults),
        .history_retention     = request.history_retention,
        .runnable_wait_warning = request.runnable_wait_warning,
        .created_at            = now,
        .updated_at            = now,
        .deleted_at            = std::nullopt,
    };

    if (!transaction) {
        auto begun = jb::db::Transaction::begin(_data->database);
        if (!begun) {
            return ServiceResult<Queue>::failure(std::move(begun).error());
        }
        transaction.emplace(std::move(begun).value());
    }
    auto existing = _data->queues.find_by_name(queue.name, false);
    if (!existing) {
        return ServiceResult<Queue>::failure(std::move(existing).error());
    }
    if (existing->has_value()) {
        return ServiceResult<Queue>::failure(queue_name_conflict());
    }
    auto inserted = _data->queues.insert(queue, queue.name, *defaults);
    if (!inserted) {
        return ServiceResult<Queue>::failure(std::move(inserted).error());
    }
    if (request.idempotency_key) {
        auto result_json = detail::encode_queue_idempotency_result(queue, _data->attributes);
        if (!result_json) {
            return ServiceResult<Queue>::failure(std::move(result_json).error());
        }
        auto recorded = _data->idempotency.insert({
            .method       = "queue.create",
            .scope_id     = jb::core::Uuid{},
            .key          = *request.idempotency_key,
            .request_json = *canonical_request,
            .result_json  = std::move(result_json).value(),
            .resource_id  = queue.id,
            .created_at   = now,
            .expires_at   = std::nullopt,
        });
        if (!recorded) {
            return ServiceResult<Queue>::failure(std::move(recorded).error());
        }
    }
    auto committed = transaction->commit();
    if (!committed) {
        return ServiceResult<Queue>::failure(std::move(committed).error());
    }
    return ServiceResult<Queue>::success(std::move(queue));
}

auto ManagementService::get_queue(QueueSelector const& selector, bool include_deleted)
    -> jb::core::Result<Queue, jb::core::Error>
{
    if (_data->initialization_error) {
        return ServiceResult<Queue>::failure(*_data->initialization_error);
    }
    auto validated = validate_selector(selector);
    if (!validated) {
        return ServiceResult<Queue>::failure(std::move(validated).error());
    }
    auto found = find_queue(_data->queues, selector, include_deleted);
    if (!found) {
        return ServiceResult<Queue>::failure(std::move(found).error());
    }
    if (!found->has_value()) {
        return ServiceResult<Queue>::failure(queue_not_found());
    }
    return ServiceResult<Queue>::success(std::move(**found));
}

auto ManagementService::list_queues(QueueListRequest const& request) -> jb::core::Result<QueuePage, jb::core::Error>
{
    if (_data->initialization_error) {
        return ServiceResult<QueuePage>::failure(*_data->initialization_error);
    }
    if (request.page.limit == 0 || request.page.limit > kMaximumPageSize) {
        return ServiceResult<QueuePage>::failure(invalid_configuration("page_limit_out_of_range"));
    }
    if (request.state && !valid_queue_state(*request.state)) {
        return ServiceResult<QueuePage>::failure(invalid_configuration("unknown_queue_state"));
    }
    auto listed =
        _data->queues.list(request.include_deleted, request.state, request.page.limit + 1U, request.page.after_id);
    if (!listed) {
        return ServiceResult<QueuePage>::failure(std::move(listed).error());
    }

    auto page = QueuePage{.items = std::move(listed).value()};
    if (page.items.size() > request.page.limit) {
        page.items.resize(request.page.limit);
        page.next_after_id = page.items.back().id;
    }
    return ServiceResult<QueuePage>::success(std::move(page));
}

auto ManagementService::update_queue(UpdateQueueRequest request) -> jb::core::Result<Queue, jb::core::Error>
{
    if (_data->initialization_error) {
        return ServiceResult<Queue>::failure(*_data->initialization_error);
    }
    auto selector = validate_selector(request.queue);
    if (!selector) {
        return ServiceResult<Queue>::failure(std::move(selector).error());
    }
    if (is_empty_update(request)) {
        return ServiceResult<Queue>::failure(invalid_configuration("empty_update"));
    }
    if (request.name) {
        auto name = validate_name(*request.name);
        if (!name) {
            return ServiceResult<Queue>::failure(std::move(name).error());
        }
    }
    if (request.weight && *request.weight == 0) {
        return ServiceResult<Queue>::failure(invalid_configuration("weight_not_positive"));
    }
    if (request.concurrency_limit && *request.concurrency_limit == 0) {
        return ServiceResult<Queue>::failure(invalid_configuration("concurrency_limit_not_positive"));
    }
    if (request.recovery_policy && !valid_recovery_policy(*request.recovery_policy)) {
        return ServiceResult<Queue>::failure(invalid_configuration("unknown_recovery_policy"));
    }
    auto serialized_defaults = std::optional<detail::SerializedAttributeDocument>{};
    if (request.defaults) {
        auto validated = validate_attributes(_data->attributes, *request.defaults, AttributeScope::QueueDefault);
        if (!validated) {
            return ServiceResult<Queue>::failure(std::move(validated).error());
        }
        serialized_defaults.emplace(std::move(validated).value());
    }
    if (request.history_retention && *request.history_retention && (*request.history_retention)->count() < 0) {
        return ServiceResult<Queue>::failure(invalid_configuration("negative_history_retention"));
    }
    if (request.runnable_wait_warning && request.runnable_wait_warning->count() < 0) {
        return ServiceResult<Queue>::failure(invalid_configuration("negative_runnable_wait_warning"));
    }
    auto const now = _data->time_source.utc_now();

    auto begun = jb::db::Transaction::begin(_data->database);
    if (!begun) {
        return ServiceResult<Queue>::failure(std::move(begun).error());
    }
    auto transaction = std::move(begun).value();
    auto found       = find_queue(_data->queues, request.queue, false);
    if (!found) {
        return ServiceResult<Queue>::failure(std::move(found).error());
    }
    if (!found->has_value()) {
        return ServiceResult<Queue>::failure(queue_not_found());
    }
    auto replacement = std::move(**found);
    if (request.name && *request.name != replacement.name) {
        auto conflict = _data->queues.find_by_name(*request.name, false);
        if (!conflict) {
            return ServiceResult<Queue>::failure(std::move(conflict).error());
        }
        if (conflict->has_value() && (**conflict).id != replacement.id) {
            return ServiceResult<Queue>::failure(queue_name_conflict());
        }
        replacement.name = std::move(*request.name);
    }
    if (request.weight) {
        replacement.weight = *request.weight;
    }
    if (request.concurrency_limit) {
        replacement.concurrency_limit = *request.concurrency_limit;
    }
    if (request.recovery_policy) {
        replacement.recovery_policy = *request.recovery_policy;
    }
    if (request.defaults) {
        replacement.defaults = std::move(*request.defaults);
    }
    if (request.history_retention) {
        replacement.history_retention = *request.history_retention;
    }
    if (request.runnable_wait_warning) {
        replacement.runnable_wait_warning = *request.runnable_wait_warning;
    }
    replacement.updated_at = now;

    auto replaced =
        _data->queues.replace_mutable_fields(replacement, serialized_defaults ? &*serialized_defaults : nullptr);
    if (!replaced) {
        return ServiceResult<Queue>::failure(std::move(replaced).error());
    }
    if (!*replaced) {
        return ServiceResult<Queue>::failure(queue_state_conflict());
    }
    auto committed = transaction.commit();
    if (!committed) {
        return ServiceResult<Queue>::failure(std::move(committed).error());
    }
    return ServiceResult<Queue>::success(std::move(replacement));
}

auto ManagementService::suspend_queue(QueueSelector const& selector) -> jb::core::Result<Queue, jb::core::Error>
{
    if (_data->initialization_error) {
        return ServiceResult<Queue>::failure(*_data->initialization_error);
    }
    auto validated = validate_selector(selector);
    if (!validated) {
        return ServiceResult<Queue>::failure(std::move(validated).error());
    }
    auto const now = _data->time_source.utc_now();

    auto begun = jb::db::Transaction::begin(_data->database);
    if (!begun) {
        return ServiceResult<Queue>::failure(std::move(begun).error());
    }
    auto transaction = std::move(begun).value();
    auto found       = find_queue(_data->queues, selector, true);
    if (!found) {
        return ServiceResult<Queue>::failure(std::move(found).error());
    }
    if (!found->has_value()) {
        return ServiceResult<Queue>::failure(queue_not_found());
    }
    auto queue = std::move(**found);
    if (queue.state == QueueState::Deleted) {
        return ServiceResult<Queue>::failure(queue_state_conflict());
    }
    if (queue.state == QueueState::Suspended) {
        auto committed = transaction.commit();
        if (!committed) {
            return ServiceResult<Queue>::failure(std::move(committed).error());
        }
        return ServiceResult<Queue>::success(std::move(queue));
    }
    if (queue.state == QueueState::Active) {
        // Establish the suspension gate before consulting durable occupancy.
        // Completion processing finishes the transition later when work remains.
        auto transitioned = _data->queues.set_state(queue.id, QueueState::Active, QueueState::Suspending, now);
        if (!transitioned) {
            return ServiceResult<Queue>::failure(std::move(transitioned).error());
        }
        if (!*transitioned) {
            return ServiceResult<Queue>::failure(queue_state_conflict());
        }
        queue.state      = QueueState::Suspending;
        queue.updated_at = now;
    }

    auto running = _data->runs.count_running_for_queue(queue.id);
    if (!running) {
        return ServiceResult<Queue>::failure(std::move(running).error());
    }
    if (*running == 0) {
        auto transitioned = _data->queues.set_state(queue.id, QueueState::Suspending, QueueState::Suspended, now);
        if (!transitioned) {
            return ServiceResult<Queue>::failure(std::move(transitioned).error());
        }
        if (!*transitioned) {
            return ServiceResult<Queue>::failure(queue_state_conflict());
        }
        queue.state      = QueueState::Suspended;
        queue.updated_at = now;
    }

    auto committed = transaction.commit();
    if (!committed) {
        return ServiceResult<Queue>::failure(std::move(committed).error());
    }
    return ServiceResult<Queue>::success(std::move(queue));
}

auto ManagementService::resume_queue(QueueSelector const& selector) -> jb::core::Result<Queue, jb::core::Error>
{
    if (_data->initialization_error) {
        return ServiceResult<Queue>::failure(*_data->initialization_error);
    }
    auto validated = validate_selector(selector);
    if (!validated) {
        return ServiceResult<Queue>::failure(std::move(validated).error());
    }
    auto const now = _data->time_source.utc_now();

    auto begun = jb::db::Transaction::begin(_data->database);
    if (!begun) {
        return ServiceResult<Queue>::failure(std::move(begun).error());
    }
    auto transaction = std::move(begun).value();
    auto found       = find_queue(_data->queues, selector, true);
    if (!found) {
        return ServiceResult<Queue>::failure(std::move(found).error());
    }
    if (!found->has_value()) {
        return ServiceResult<Queue>::failure(queue_not_found());
    }
    auto queue = std::move(**found);
    if (queue.state == QueueState::Deleted) {
        return ServiceResult<Queue>::failure(queue_state_conflict());
    }
    if (queue.state != QueueState::Active) {
        auto const expected_state = queue.state;
        auto       transitioned   = _data->queues.set_state(queue.id, expected_state, QueueState::Active, now);
        if (!transitioned) {
            return ServiceResult<Queue>::failure(std::move(transitioned).error());
        }
        if (!*transitioned) {
            return ServiceResult<Queue>::failure(queue_state_conflict());
        }
        queue.state      = QueueState::Active;
        queue.updated_at = now;
    }

    auto committed = transaction.commit();
    if (!committed) {
        return ServiceResult<Queue>::failure(std::move(committed).error());
    }
    return ServiceResult<Queue>::success(std::move(queue));
}

auto ManagementService::delete_queue(QueueSelector const& selector) -> jb::core::Result<void, jb::core::Error>
{
    if (_data->initialization_error) {
        return ServiceResult<void>::failure(*_data->initialization_error);
    }
    auto validated = validate_selector(selector);
    if (!validated) {
        return ServiceResult<void>::failure(std::move(validated).error());
    }
    auto const now = _data->time_source.utc_now();

    auto begun = jb::db::Transaction::begin(_data->database);
    if (!begun) {
        return ServiceResult<void>::failure(std::move(begun).error());
    }
    auto transaction = std::move(begun).value();
    auto found       = find_queue(_data->queues, selector, true);
    if (!found) {
        return ServiceResult<void>::failure(std::move(found).error());
    }
    if (!found->has_value()) {
        return ServiceResult<void>::failure(queue_not_found());
    }
    auto queue = std::move(**found);
    if (queue.state == QueueState::Deleted) {
        return ServiceResult<void>::failure(queue_state_conflict());
    }
    if (queue.state != QueueState::Suspended) {
        return ServiceResult<void>::failure(queue_not_suspended());
    }

    // Preflight every destructive condition before changing related tables,
    // including revision capacity for all job tombstones in this queue.
    auto running = _data->runs.count_running_for_queue(queue.id);
    if (!running) {
        return ServiceResult<void>::failure(std::move(running).error());
    }
    if (*running != 0) {
        return ServiceResult<void>::failure(queue_has_running_attempt());
    }
    auto job_count = _data->queues.count_non_deleted_jobs(queue.id);
    if (!job_count) {
        return ServiceResult<void>::failure(std::move(job_count).error());
    }
    auto exhausted = _data->jobs.has_exhausted_revision_in_queue(queue.id, kMaximumPersistedJobRevision);
    if (!exhausted) {
        return ServiceResult<void>::failure(std::move(exhausted).error());
    }
    if (*exhausted) {
        return ServiceResult<void>::failure(job_revision_exhausted());
    }

    // Reference cleanup, job tombstones, pending-run cancellation, and the
    // queue tombstone commit together to preserve all cross-table relationships.
    auto references = _data->secrets.erase_references_for_queue(queue.id);
    if (!references) {
        return ServiceResult<void>::failure(std::move(references).error());
    }
    auto deleted_jobs = _data->jobs.mark_all_in_queue_deleted(queue.id, now);
    if (!deleted_jobs) {
        return ServiceResult<void>::failure(std::move(deleted_jobs).error());
    }
    if (*deleted_jobs != *job_count) {
        return ServiceResult<void>::failure(storage_invariant("queue_delete_job_count"));
    }
    auto cancelled = _data->runs.cancel_pending_for_queue(queue.id, now, "queue_deleted");
    if (!cancelled) {
        return ServiceResult<void>::failure(std::move(cancelled).error());
    }

    auto internal_name = queue.name + "-deleted#" + queue.id.to_string();
    auto deleted_queue = _data->queues.mark_deleted(queue.id, internal_name, queue.name, now);
    if (!deleted_queue) {
        return ServiceResult<void>::failure(std::move(deleted_queue).error());
    }
    if (!*deleted_queue) {
        return ServiceResult<void>::failure(queue_state_conflict());
    }
    auto committed = transaction.commit();
    if (!committed) {
        return ServiceResult<void>::failure(std::move(committed).error());
    }
    return ServiceResult<void>::success();
}

auto ManagementService::create_job(CreateJobRequest request) -> jb::core::Result<JobDefinition, jb::core::Error>
{
    if (_data->initialization_error) {
        return ServiceResult<JobDefinition>::failure(*_data->initialization_error);
    }
    // Reject and serialize caller-owned input before opening a transaction so
    // validation failures cannot hold the database write lock.
    auto selector = validate_selector(request.queue);
    if (!selector) {
        return ServiceResult<JobDefinition>::failure(std::move(selector).error());
    }
    auto name = validate_job_name(request.name);
    if (!name) {
        return ServiceResult<JobDefinition>::failure(std::move(name).error());
    }
    auto payload = validate_job_payload(request.type, request.payload);
    if (!payload) {
        return ServiceResult<JobDefinition>::failure(std::move(payload).error());
    }
    auto partial_attributes = validate_attributes(_data->attributes, request.attributes, AttributeScope::Job);
    if (!partial_attributes) {
        return ServiceResult<JobDefinition>::failure(std::move(partial_attributes).error());
    }
    auto idempotency = validate_idempotency_key(request.idempotency_key);
    if (!idempotency) {
        return ServiceResult<JobDefinition>::failure(std::move(idempotency).error());
    }

    // Non-idempotent calls can reserve identities immediately. Idempotent calls
    // defer allocation until replay has been ruled out to avoid consuming IDs.
    auto job_id            = std::optional<jb::core::Uuid>{};
    auto run_id            = std::optional<jb::core::Uuid>{};
    auto generate_identity = [&]() -> ServiceResult<void> {
        auto generated_job_id = _data->uuid_generator.generate();
        if (!generated_job_id) {
            return ServiceResult<void>::failure(std::move(generated_job_id).error());
        }
        auto generated_run_id = _data->uuid_generator.generate();
        if (!generated_run_id) {
            return ServiceResult<void>::failure(std::move(generated_run_id).error());
        }
        job_id = *generated_job_id;
        run_id = *generated_run_id;
        return ServiceResult<void>::success();
    };
    if (!request.idempotency_key) {
        auto generated = generate_identity();
        if (!generated) {
            return ServiceResult<JobDefinition>::failure(std::move(generated).error());
        }
    }

    auto begun = jb::db::Transaction::begin(_data->database);
    if (!begun) {
        return ServiceResult<JobDefinition>::failure(std::move(begun).error());
    }
    auto transaction = std::move(begun).value();
    auto found_queue = find_queue(_data->queues, request.queue, false);
    if (!found_queue) {
        return ServiceResult<JobDefinition>::failure(std::move(found_queue).error());
    }
    if (!found_queue->has_value()) {
        return ServiceResult<JobDefinition>::failure(queue_not_found());
    }
    auto const& queue = **found_queue;
    if (queue.state != QueueState::Active && queue.state != QueueState::Suspended) {
        return ServiceResult<JobDefinition>::failure(queue_state_conflict());
    }

    // Resolve a canonical replay before any durable mutation and, for an
    // idempotent first call, before generating its job and run identities.
    auto canonical_request = std::optional<std::string>{};
    if (request.idempotency_key) {
        auto encoded = detail::encode_job_create_idempotency_request(request, queue.id, _data->attributes);
        if (!encoded) {
            return ServiceResult<JobDefinition>::failure(std::move(encoded).error());
        }
        canonical_request = std::move(encoded).value();
        auto record       = _data->idempotency.find("job.create", queue.id, *request.idempotency_key);
        if (!record) {
            return ServiceResult<JobDefinition>::failure(std::move(record).error());
        }
        if (record->has_value()) {
            auto valid = detail::validate_job_create_idempotency_request((**record).request_json, _data->attributes);
            if (!valid) {
                return ServiceResult<JobDefinition>::failure(std::move(valid).error());
            }
            if ((**record).request_json != *canonical_request) {
                return ServiceResult<JobDefinition>::failure(idempotency_conflict());
            }
            auto replay = detail::decode_job_idempotency_result((**record).result_json, _data->attributes);
            if (!replay) {
                return ServiceResult<JobDefinition>::failure(std::move(replay).error());
            }
            if (replay->id != (**record).resource_id || replay->queue_id != queue.id) {
                return ServiceResult<JobDefinition>::failure(invalid_idempotency_record("job_resource_id_mismatch"));
            }
            auto committed = transaction.commit();
            if (!committed) {
                return ServiceResult<JobDefinition>::failure(std::move(committed).error());
            }
            return ServiceResult<JobDefinition>::success(std::move(replay).value());
        }

        auto generated = generate_identity();
        if (!generated) {
            return ServiceResult<JobDefinition>::failure(std::move(generated).error());
        }
    }

    // Resolve one planned instant and materialize the effective attributes so
    // the initial run owns an immutable snapshot of revision one.
    auto const now        = _data->time_source.utc_now();
    auto       planned_at = jb::core::UtcTimePoint{};
    if (auto const* once = std::get_if<OnceSchedule>(&request.schedule)) {
        planned_at = once->planned_at;
    }
    else {
        auto const& cron_schedule = std::get<CronSchedule>(request.schedule);
        auto        valid         = _data->cron.validate(cron_schedule);
        if (!valid) {
            return ServiceResult<JobDefinition>::failure(std::move(valid).error());
        }
        auto next = _data->cron.next_after(cron_schedule, now);
        if (!next) {
            return ServiceResult<JobDefinition>::failure(std::move(next).error());
        }
        planned_at = *next;
    }

    auto materialized =
        materialize_attributes(_data->attributes, _data->daemon_defaults, queue.defaults, request.attributes);
    if (!materialized) {
        return ServiceResult<JobDefinition>::failure(std::move(materialized).error());
    }
    auto serialized_attributes = serialize_attributes(_data->attributes,
                                                      *materialized,
                                                      AttributeScope::Job,
                                                      detail::AttributeDocumentMode::Materialized);
    if (!serialized_attributes) {
        return ServiceResult<JobDefinition>::failure(std::move(serialized_attributes).error());
    }

    auto job = JobDefinition{
        .id         = *job_id,
        .queue_id   = queue.id,
        .revision   = 1,
        .name       = std::move(request.name),
        .state      = JobState::Active,
        .type       = request.type,
        .schedule   = std::move(request.schedule),
        .priority   = request.priority,
        .attributes = std::move(materialized).value(),
        .payload    = std::move(request.payload),
        .created_at = now,
        .updated_at = now,
        .deleted_at = std::nullopt,
    };
    auto run = detail::ScheduleOwnedRunInsert{
        .id              = *run_id,
        .job_id          = job.id,
        .job_revision    = job.revision,
        .queue_id        = job.queue_id,
        .planned_at      = planned_at,
        .runnable_at     = planned_at,
        .type            = job.type,
        .priority        = job.priority,
        .attributes_json = serialized_attributes->serialized(),
        .payload_json    = payload->serialized(),
    };

    // Commit the definition, first schedule-owned run, and optional replay
    // result atomically so none can exist without the others.
    auto inserted_job = _data->jobs.insert(job, *serialized_attributes, *payload);
    if (!inserted_job) {
        return ServiceResult<JobDefinition>::failure(std::move(inserted_job).error());
    }
    auto inserted_run = _data->runs.insert_schedule_owned(run);
    if (!inserted_run) {
        return ServiceResult<JobDefinition>::failure(std::move(inserted_run).error());
    }
    if (request.idempotency_key) {
        auto result_json = detail::encode_job_idempotency_result(job, _data->attributes);
        if (!result_json) {
            return ServiceResult<JobDefinition>::failure(std::move(result_json).error());
        }
        auto recorded = _data->idempotency.insert({
            .method       = "job.create",
            .scope_id     = queue.id,
            .key          = *request.idempotency_key,
            .request_json = *canonical_request,
            .result_json  = std::move(result_json).value(),
            .resource_id  = job.id,
            .created_at   = now,
            .expires_at   = std::nullopt,
        });
        if (!recorded) {
            return ServiceResult<JobDefinition>::failure(std::move(recorded).error());
        }
    }
    auto committed = transaction.commit();
    if (!committed) {
        return ServiceResult<JobDefinition>::failure(std::move(committed).error());
    }
    return ServiceResult<JobDefinition>::success(std::move(job));
}

auto ManagementService::run_now(RunNowRequest request) -> jb::core::Result<JobRun, jb::core::Error>
{
    if (_data->initialization_error) {
        return ServiceResult<JobRun>::failure(*_data->initialization_error);
    }
    auto idempotency = validate_idempotency_key(request.idempotency_key);
    if (!idempotency) {
        return ServiceResult<JobRun>::failure(std::move(idempotency).error());
    }

    auto canonical_request = std::optional<std::string>{};
    if (request.idempotency_key) {
        auto encoded = detail::encode_run_now_idempotency_request(request);
        if (!encoded) {
            return ServiceResult<JobRun>::failure(std::move(encoded).error());
        }
        canonical_request = std::move(encoded).value();
    }

    // Check replay before eligibility or identifier generation so retries
    // return the original durable snapshot without consuming another run ID.
    auto begun = jb::db::Transaction::begin(_data->database);
    if (!begun) {
        return ServiceResult<JobRun>::failure(std::move(begun).error());
    }
    auto transaction = std::move(begun).value();
    if (request.idempotency_key) {
        auto record = _data->idempotency.find("job.run_now", request.job_id, *request.idempotency_key);
        if (!record) {
            return ServiceResult<JobRun>::failure(std::move(record).error());
        }
        if (record->has_value()) {
            auto const& stored = **record;
            if (stored.method != "job.run_now" || stored.scope_id != request.job_id ||
                stored.key != *request.idempotency_key) {
                return ServiceResult<JobRun>::failure(invalid_idempotency_record("run_now_record_identity"));
            }
            auto valid = detail::validate_run_now_idempotency_request(stored.request_json);
            if (!valid) {
                return ServiceResult<JobRun>::failure(std::move(valid).error());
            }
            if (stored.request_json != *canonical_request) {
                return ServiceResult<JobRun>::failure(invalid_idempotency_record("run_now_request_scope_mismatch"));
            }
            auto replay = detail::decode_run_now_idempotency_result(stored.result_json, _data->attributes);
            if (!replay) {
                return ServiceResult<JobRun>::failure(std::move(replay).error());
            }
            if (replay->id != stored.resource_id || replay->job_id != request.job_id) {
                return ServiceResult<JobRun>::failure(invalid_idempotency_record("run_now_resource_id_mismatch"));
            }
            auto committed = transaction.commit();
            if (!committed) {
                return ServiceResult<JobRun>::failure(std::move(committed).error());
            }
            return ServiceResult<JobRun>::success(std::move(replay).value());
        }
    }

    // Establish the job, queue, and future schedule-owned relationship, then
    // require an idle job with no existing non-terminal manual barrier.
    auto found_job = _data->jobs.find_by_id(request.job_id, true);
    if (!found_job) {
        return ServiceResult<JobRun>::failure(std::move(found_job).error());
    }
    if (!found_job->has_value()) {
        return ServiceResult<JobRun>::failure(job_not_found());
    }
    auto const& job = **found_job;
    if (job.state == JobState::Deleted) {
        return ServiceResult<JobRun>::failure(job_deleted());
    }
    auto found_queue = _data->queues.find_by_id(job.queue_id, true);
    if (!found_queue) {
        return ServiceResult<JobRun>::failure(std::move(found_queue).error());
    }
    if (!found_queue->has_value()) {
        return ServiceResult<JobRun>::failure(storage_invariant("run_now_missing_queue"));
    }
    if ((**found_queue).state == QueueState::Deleted) {
        return ServiceResult<JobRun>::failure(manual_run_conflict());
    }
    auto schedule_owned = _data->runs.find_schedule_owned(job.id);
    if (!schedule_owned) {
        return ServiceResult<JobRun>::failure(std::move(schedule_owned).error());
    }
    if (!schedule_owned->has_value()) {
        return ServiceResult<JobRun>::failure(manual_run_conflict());
    }
    auto const& scheduled = **schedule_owned;
    if (scheduled.job_id != job.id || scheduled.queue_id != job.queue_id || scheduled.origin != RunOrigin::Scheduled ||
        !scheduled.schedule_owned) {
        return ServiceResult<JobRun>::failure(storage_invariant("run_now_schedule_relationship"));
    }

    auto const now = _data->time_source.utc_now();
    if (scheduled.state != RunState::Scheduled || scheduled.planned_at <= now) {
        return ServiceResult<JobRun>::failure(manual_run_conflict());
    }
    auto busy = _data->runs.has_running_or_retrying_run(job.id);
    if (!busy) {
        return ServiceResult<JobRun>::failure(std::move(busy).error());
    }
    if (*busy) {
        return ServiceResult<JobRun>::failure(manual_run_conflict());
    }
    auto manual = _data->runs.has_non_terminal_manual_run(job.id);
    if (!manual) {
        return ServiceResult<JobRun>::failure(std::move(manual).error());
    }
    if (*manual) {
        return ServiceResult<JobRun>::failure(manual_run_conflict());
    }

    // The manual row snapshots the current definition and itself blocks the
    // recurring schedule-owned successor until the manual run is terminal.
    auto run_id = _data->uuid_generator.generate();
    if (!run_id) {
        return ServiceResult<JobRun>::failure(std::move(run_id).error());
    }
    auto run = JobRun{
        .id             = *run_id,
        .job_id         = job.id,
        .job_revision   = job.revision,
        .queue_id       = job.queue_id,
        .origin         = RunOrigin::Manual,
        .schedule_owned = false,
        .planned_at     = now,
        .runnable_at    = now,
        .started_at     = std::nullopt,
        .completed_at   = std::nullopt,
        .type           = job.type,
        .priority       = job.priority,
        .attributes     = job.attributes,
        .payload        = job.payload,
        .state          = RunState::Scheduled,
        .result         = std::nullopt,
    };
    // Store the manual snapshot and its optional idempotent result in one
    // transaction so a replay can never name a run that was rolled back.
    auto inserted = _data->runs.insert_manual(run);
    if (!inserted) {
        return ServiceResult<JobRun>::failure(std::move(inserted).error());
    }
    if (request.idempotency_key) {
        auto result_json = detail::encode_run_now_idempotency_result(run, _data->attributes);
        if (!result_json) {
            return ServiceResult<JobRun>::failure(std::move(result_json).error());
        }
        auto recorded = _data->idempotency.insert({
            .method       = "job.run_now",
            .scope_id     = job.id,
            .key          = *request.idempotency_key,
            .request_json = *canonical_request,
            .result_json  = std::move(result_json).value(),
            .resource_id  = run.id,
            .created_at   = now,
            .expires_at   = std::nullopt,
        });
        if (!recorded) {
            return ServiceResult<JobRun>::failure(std::move(recorded).error());
        }
    }
    auto committed = transaction.commit();
    if (!committed) {
        return ServiceResult<JobRun>::failure(std::move(committed).error());
    }
    return ServiceResult<JobRun>::success(std::move(run));
}

auto ManagementService::update_job(UpdateJobRequest request) -> jb::core::Result<JobDefinition, jb::core::Error>
{
    if (_data->initialization_error) {
        return ServiceResult<JobDefinition>::failure(*_data->initialization_error);
    }
    if (request.expected_revision == 0) {
        return ServiceResult<JobDefinition>::failure(invalid_job_configuration("expected_revision_not_positive"));
    }
    if (is_empty_update(request)) {
        return ServiceResult<JobDefinition>::failure(invalid_job_configuration("empty_update"));
    }
    if (request.name) {
        auto name = validate_job_name(*request.name);
        if (!name) {
            return ServiceResult<JobDefinition>::failure(std::move(name).error());
        }
    }
    if (request.schedule && std::holds_alternative<CronSchedule>(*request.schedule)) {
        auto valid = _data->cron.validate(std::get<CronSchedule>(*request.schedule));
        if (!valid) {
            return ServiceResult<JobDefinition>::failure(std::move(valid).error());
        }
    }
    auto attribute_changes = validate_attributes(_data->attributes, request.attribute_changes, AttributeScope::Job);
    if (!attribute_changes) {
        return ServiceResult<JobDefinition>::failure(std::move(attribute_changes).error());
    }

    auto begun = jb::db::Transaction::begin(_data->database);
    if (!begun) {
        return ServiceResult<JobDefinition>::failure(std::move(begun).error());
    }
    auto       transaction = std::move(begun).value();
    auto const now         = _data->time_source.utc_now();
    auto       found       = _data->jobs.find_by_id(request.job_id, true);
    if (!found) {
        return ServiceResult<JobDefinition>::failure(std::move(found).error());
    }
    if (!found->has_value()) {
        return ServiceResult<JobDefinition>::failure(job_not_found());
    }
    auto replacement = std::move(**found);
    if (replacement.state == JobState::Deleted) {
        return ServiceResult<JobDefinition>::failure(job_deleted());
    }
    if (replacement.revision != request.expected_revision) {
        return ServiceResult<JobDefinition>::failure(job_revision_conflict());
    }
    if (replacement.state != JobState::Active && replacement.state != JobState::Suspended) {
        return ServiceResult<JobDefinition>::failure(job_state_conflict());
    }

    // A once definition becomes immutable after execution starts. Recurring
    // definitions remain mutable because every run owns a revisioned snapshot.
    auto const recurring = std::holds_alternative<CronSchedule>(replacement.schedule);
    if (!recurring) {
        auto started = _data->attempts.has_started_for_job(replacement.id);
        if (!started) {
            return ServiceResult<JobDefinition>::failure(std::move(started).error());
        }
        if (*started) {
            return ServiceResult<JobDefinition>::failure(job_immutable());
        }
    }
    auto schedule_owned = _data->runs.find_schedule_owned(replacement.id);
    if (!schedule_owned) {
        return ServiceResult<JobDefinition>::failure(std::move(schedule_owned).error());
    }
    if (!schedule_owned->has_value()) {
        return ServiceResult<JobDefinition>::failure(schedule_refresh_conflict());
    }

    // Refresh only an untouched scheduled successor. Once recurring work has
    // been claimed, preserve its snapshot and defer edits to the next successor;
    // conversion to a once schedule cannot be deferred safely.
    auto refresh_snapshot = false;
    if ((**schedule_owned).state == RunState::Scheduled) {
        auto has_attempt = _data->attempts.has_any_for_run((**schedule_owned).id);
        if (!has_attempt) {
            return ServiceResult<JobDefinition>::failure(std::move(has_attempt).error());
        }
        if (*has_attempt) {
            return ServiceResult<JobDefinition>::failure(schedule_refresh_conflict());
        }
        refresh_snapshot = true;
    }
    else if (!recurring || (request.schedule && std::holds_alternative<OnceSchedule>(*request.schedule))) {
        return ServiceResult<JobDefinition>::failure(schedule_refresh_conflict());
    }

    if (request.name) {
        replacement.name = std::move(*request.name);
    }
    if (request.type) {
        replacement.type = *request.type;
    }
    if (request.schedule) {
        replacement.schedule = std::move(*request.schedule);
    }
    if (request.priority) {
        replacement.priority = *request.priority;
    }
    for (auto& [name, value] : request.attribute_changes) {
        replacement.attributes.insert_or_assign(name, std::move(value));
    }
    if (request.payload) {
        replacement.payload = std::move(*request.payload);
    }
    auto payload = validate_job_payload(replacement.type, replacement.payload);
    if (!payload) {
        return ServiceResult<JobDefinition>::failure(std::move(payload).error());
    }
    auto serialized_attributes = serialize_attributes(_data->attributes,
                                                      replacement.attributes,
                                                      AttributeScope::Job,
                                                      detail::AttributeDocumentMode::Materialized);
    if (!serialized_attributes) {
        return ServiceResult<JobDefinition>::failure(std::move(serialized_attributes).error());
    }
    if (replacement.revision >= kMaximumPersistedJobRevision) {
        return ServiceResult<JobDefinition>::failure(job_revision_exhausted());
    }
    ++replacement.revision;
    replacement.updated_at = now;

    // The definition revision and any eligible successor refresh share this
    // transaction, so a lost refresh rolls back rather than leaving disagreement.
    auto replaced =
        _data->jobs.update_definition(replacement, request.expected_revision, *serialized_attributes, *payload);
    if (!replaced) {
        return ServiceResult<JobDefinition>::failure(std::move(replaced).error());
    }
    if (!*replaced) {
        auto diagnosed = _data->jobs.find_by_id(replacement.id, true);
        if (!diagnosed) {
            return ServiceResult<JobDefinition>::failure(std::move(diagnosed).error());
        }
        if (!diagnosed->has_value()) {
            return ServiceResult<JobDefinition>::failure(job_not_found());
        }
        if ((**diagnosed).state == JobState::Deleted) {
            return ServiceResult<JobDefinition>::failure(job_deleted());
        }
        if ((**diagnosed).revision != request.expected_revision) {
            return ServiceResult<JobDefinition>::failure(job_revision_conflict());
        }
        return ServiceResult<JobDefinition>::failure(job_state_conflict());
    }

    if (refresh_snapshot) {
        auto planned_at = jb::core::UtcTimePoint{};
        if (auto const* once = std::get_if<OnceSchedule>(&replacement.schedule)) {
            planned_at = once->planned_at;
        }
        else {
            auto next = _data->cron.next_after(std::get<CronSchedule>(replacement.schedule), now);
            if (!next) {
                return ServiceResult<JobDefinition>::failure(std::move(next).error());
            }
            planned_at = *next;
        }
        auto refreshed =
            _data->runs.refresh_unstarted_schedule_owned(replacement.id,
                                                         {
                                                             .job_revision    = replacement.revision,
                                                             .queue_id        = replacement.queue_id,
                                                             .planned_at      = planned_at,
                                                             .runnable_at     = planned_at,
                                                             .type            = replacement.type,
                                                             .priority        = replacement.priority,
                                                             .attributes_json = serialized_attributes->serialized(),
                                                             .payload_json    = payload->serialized(),
                                                         });
        if (!refreshed) {
            return ServiceResult<JobDefinition>::failure(std::move(refreshed).error());
        }
        if (!*refreshed) {
            return ServiceResult<JobDefinition>::failure(schedule_refresh_conflict());
        }
    }
    auto committed = transaction.commit();
    if (!committed) {
        return ServiceResult<JobDefinition>::failure(std::move(committed).error());
    }
    return ServiceResult<JobDefinition>::success(std::move(replacement));
}

auto ManagementService::suspend_job(jb::core::Uuid const& id) -> jb::core::Result<JobDefinition, jb::core::Error>
{
    if (_data->initialization_error) {
        return ServiceResult<JobDefinition>::failure(*_data->initialization_error);
    }
    auto const now = _data->time_source.utc_now();

    auto begun = jb::db::Transaction::begin(_data->database);
    if (!begun) {
        return ServiceResult<JobDefinition>::failure(std::move(begun).error());
    }
    auto transaction = std::move(begun).value();
    auto found       = _data->jobs.find_by_id(id, true);
    if (!found) {
        return ServiceResult<JobDefinition>::failure(std::move(found).error());
    }
    if (!found->has_value()) {
        return ServiceResult<JobDefinition>::failure(job_not_found());
    }
    auto job = std::move(**found);
    if (job.state == JobState::Deleted) {
        return ServiceResult<JobDefinition>::failure(job_deleted());
    }
    if (job.state == JobState::Suspended) {
        auto committed = transaction.commit();
        if (!committed) {
            return ServiceResult<JobDefinition>::failure(std::move(committed).error());
        }
        return ServiceResult<JobDefinition>::success(std::move(job));
    }
    if (job.state == JobState::Active) {
        // Each durable job-state transition advances the revision. If running
        // work remains, completion processing advances it again when drained.
        if (job.revision >= kMaximumPersistedJobRevision) {
            return ServiceResult<JobDefinition>::failure(job_revision_exhausted());
        }
        auto const expected_revision = job.revision;
        auto const next_revision     = expected_revision + 1;
        auto       transitioned      = _data->jobs.set_state(job.id,
                                                             JobState::Active,
                                                             JobState::Suspending,
                                                             expected_revision,
                                                             next_revision,
                                                             now);
        if (!transitioned) {
            return ServiceResult<JobDefinition>::failure(std::move(transitioned).error());
        }
        if (!*transitioned) {
            return ServiceResult<JobDefinition>::failure(job_state_conflict());
        }
        job.state      = JobState::Suspending;
        job.revision   = next_revision;
        job.updated_at = now;
    }

    auto running = _data->runs.count_running_for_job(job.id);
    if (!running) {
        return ServiceResult<JobDefinition>::failure(std::move(running).error());
    }
    if (*running == 0) {
        if (job.revision >= kMaximumPersistedJobRevision) {
            return ServiceResult<JobDefinition>::failure(job_revision_exhausted());
        }
        auto const expected_revision = job.revision;
        auto const next_revision     = expected_revision + 1;
        auto       transitioned      = _data->jobs.set_state(job.id,
                                                             JobState::Suspending,
                                                             JobState::Suspended,
                                                             expected_revision,
                                                             next_revision,
                                                             now);
        if (!transitioned) {
            return ServiceResult<JobDefinition>::failure(std::move(transitioned).error());
        }
        if (!*transitioned) {
            return ServiceResult<JobDefinition>::failure(job_state_conflict());
        }
        job.state      = JobState::Suspended;
        job.revision   = next_revision;
        job.updated_at = now;
    }

    auto committed = transaction.commit();
    if (!committed) {
        return ServiceResult<JobDefinition>::failure(std::move(committed).error());
    }
    return ServiceResult<JobDefinition>::success(std::move(job));
}

auto ManagementService::resume_job(jb::core::Uuid const& id) -> jb::core::Result<JobDefinition, jb::core::Error>
{
    if (_data->initialization_error) {
        return ServiceResult<JobDefinition>::failure(*_data->initialization_error);
    }
    auto const now = _data->time_source.utc_now();

    auto begun = jb::db::Transaction::begin(_data->database);
    if (!begun) {
        return ServiceResult<JobDefinition>::failure(std::move(begun).error());
    }
    auto transaction = std::move(begun).value();
    auto found       = _data->jobs.find_by_id(id, true);
    if (!found) {
        return ServiceResult<JobDefinition>::failure(std::move(found).error());
    }
    if (!found->has_value()) {
        return ServiceResult<JobDefinition>::failure(job_not_found());
    }
    auto job = std::move(**found);
    if (job.state == JobState::Deleted) {
        return ServiceResult<JobDefinition>::failure(job_deleted());
    }
    if (job.state != JobState::Active) {
        if (job.revision >= kMaximumPersistedJobRevision) {
            return ServiceResult<JobDefinition>::failure(job_revision_exhausted());
        }
        auto const expected_state    = job.state;
        auto const expected_revision = job.revision;
        auto const next_revision     = expected_revision + 1;
        auto       transitioned =
            _data->jobs.set_state(job.id, expected_state, JobState::Active, expected_revision, next_revision, now);
        if (!transitioned) {
            return ServiceResult<JobDefinition>::failure(std::move(transitioned).error());
        }
        if (!*transitioned) {
            return ServiceResult<JobDefinition>::failure(job_state_conflict());
        }
        job.state      = JobState::Active;
        job.revision   = next_revision;
        job.updated_at = now;
    }

    auto committed = transaction.commit();
    if (!committed) {
        return ServiceResult<JobDefinition>::failure(std::move(committed).error());
    }
    return ServiceResult<JobDefinition>::success(std::move(job));
}

auto ManagementService::move_job(MoveJobRequest const& request) -> jb::core::Result<JobDefinition, jb::core::Error>
{
    if (_data->initialization_error) {
        return ServiceResult<JobDefinition>::failure(*_data->initialization_error);
    }
    if (request.expected_revision == 0) {
        return ServiceResult<JobDefinition>::failure(invalid_job_configuration("expected_revision_not_positive"));
    }
    auto selector = validate_selector(request.target_queue);
    if (!selector) {
        return ServiceResult<JobDefinition>::failure(std::move(selector).error());
    }
    auto const now = _data->time_source.utc_now();

    auto begun = jb::db::Transaction::begin(_data->database);
    if (!begun) {
        return ServiceResult<JobDefinition>::failure(std::move(begun).error());
    }
    auto transaction = std::move(begun).value();
    auto found       = _data->jobs.find_by_id(request.job_id, true);
    if (!found) {
        return ServiceResult<JobDefinition>::failure(std::move(found).error());
    }
    if (!found->has_value()) {
        return ServiceResult<JobDefinition>::failure(job_not_found());
    }
    auto job = std::move(**found);
    if (job.state == JobState::Deleted) {
        return ServiceResult<JobDefinition>::failure(job_deleted());
    }
    if (job.revision != request.expected_revision) {
        return ServiceResult<JobDefinition>::failure(job_revision_conflict());
    }
    if (job.state != JobState::Suspended) {
        return ServiceResult<JobDefinition>::failure(job_not_suspended());
    }

    auto found_queue = find_queue(_data->queues, request.target_queue, false);
    if (!found_queue) {
        return ServiceResult<JobDefinition>::failure(std::move(found_queue).error());
    }
    if (!found_queue->has_value()) {
        return ServiceResult<JobDefinition>::failure(queue_not_found());
    }
    auto const& target_queue = **found_queue;
    if (target_queue.id == job.queue_id ||
        (target_queue.state != QueueState::Active && target_queue.state != QueueState::Suspended)) {
        return ServiceResult<JobDefinition>::failure(queue_state_conflict());
    }
    if (job.revision >= kMaximumPersistedJobRevision) {
        return ServiceResult<JobDefinition>::failure(job_revision_exhausted());
    }
    // Move the definition and every non-terminal run snapshot in one transaction
    // so their queue and revision references cannot disagree.
    auto const next_revision = job.revision + 1;
    auto       moved_job     = _data->jobs.move(job.id, job.revision, target_queue.id, next_revision, now);
    if (!moved_job) {
        return ServiceResult<JobDefinition>::failure(std::move(moved_job).error());
    }
    if (!*moved_job) {
        auto diagnosed = _data->jobs.find_by_id(job.id, true);
        if (!diagnosed) {
            return ServiceResult<JobDefinition>::failure(std::move(diagnosed).error());
        }
        if (!diagnosed->has_value()) {
            return ServiceResult<JobDefinition>::failure(job_not_found());
        }
        if ((**diagnosed).state == JobState::Deleted) {
            return ServiceResult<JobDefinition>::failure(job_deleted());
        }
        if ((**diagnosed).revision != request.expected_revision) {
            return ServiceResult<JobDefinition>::failure(job_revision_conflict());
        }
        if ((**diagnosed).state != JobState::Suspended) {
            return ServiceResult<JobDefinition>::failure(job_not_suspended());
        }
        return ServiceResult<JobDefinition>::failure(job_state_conflict());
    }
    auto moved_runs = _data->runs.move_non_terminal(job.id, target_queue.id, next_revision);
    if (!moved_runs) {
        return ServiceResult<JobDefinition>::failure(std::move(moved_runs).error());
    }

    job.queue_id   = target_queue.id;
    job.revision   = next_revision;
    job.updated_at = now;
    auto committed = transaction.commit();
    if (!committed) {
        return ServiceResult<JobDefinition>::failure(std::move(committed).error());
    }
    return ServiceResult<JobDefinition>::success(std::move(job));
}

auto ManagementService::delete_job(DeleteJobRequest const& request) -> jb::core::Result<void, jb::core::Error>
{
    if (_data->initialization_error) {
        return ServiceResult<void>::failure(*_data->initialization_error);
    }
    if (request.expected_revision == 0) {
        return ServiceResult<void>::failure(invalid_job_configuration("expected_revision_not_positive"));
    }
    auto const now = _data->time_source.utc_now();

    auto begun = jb::db::Transaction::begin(_data->database);
    if (!begun) {
        return ServiceResult<void>::failure(std::move(begun).error());
    }
    auto transaction = std::move(begun).value();
    auto found       = _data->jobs.find_by_id(request.job_id, true);
    if (!found) {
        return ServiceResult<void>::failure(std::move(found).error());
    }
    if (!found->has_value()) {
        return ServiceResult<void>::failure(job_not_found());
    }
    auto job = std::move(**found);
    if (job.state == JobState::Deleted) {
        return ServiceResult<void>::failure(job_deleted());
    }
    if (job.revision != request.expected_revision) {
        return ServiceResult<void>::failure(job_revision_conflict());
    }
    if (job.state != JobState::Suspended) {
        return ServiceResult<void>::failure(job_not_suspended());
    }
    // Deletion requires a suspended, drained job before atomically tombstoning
    // it, cancelling pending runs, and removing its secret references.
    auto running = _data->runs.count_running_for_job(job.id);
    if (!running) {
        return ServiceResult<void>::failure(std::move(running).error());
    }
    if (*running != 0) {
        return ServiceResult<void>::failure(job_has_running_attempt());
    }
    if (job.revision >= kMaximumPersistedJobRevision) {
        return ServiceResult<void>::failure(job_revision_exhausted());
    }
    auto const next_revision = job.revision + 1;
    auto       deleted       = _data->jobs.mark_deleted(job.id, job.revision, next_revision, now);
    if (!deleted) {
        return ServiceResult<void>::failure(std::move(deleted).error());
    }
    if (!*deleted) {
        auto diagnosed = _data->jobs.find_by_id(job.id, true);
        if (!diagnosed) {
            return ServiceResult<void>::failure(std::move(diagnosed).error());
        }
        if (!diagnosed->has_value()) {
            return ServiceResult<void>::failure(job_not_found());
        }
        if ((**diagnosed).state == JobState::Deleted) {
            return ServiceResult<void>::failure(job_deleted());
        }
        if ((**diagnosed).revision != request.expected_revision) {
            return ServiceResult<void>::failure(job_revision_conflict());
        }
        if ((**diagnosed).state != JobState::Suspended) {
            return ServiceResult<void>::failure(job_not_suspended());
        }
        return ServiceResult<void>::failure(job_state_conflict());
    }
    auto cancelled = _data->runs.cancel_pending_for_job(job.id, now, "job_deleted");
    if (!cancelled) {
        return ServiceResult<void>::failure(std::move(cancelled).error());
    }
    auto references = _data->secrets.replace_references_for_job(job.id, {});
    if (!references) {
        return ServiceResult<void>::failure(std::move(references).error());
    }
    auto committed = transaction.commit();
    if (!committed) {
        return ServiceResult<void>::failure(std::move(committed).error());
    }
    return ServiceResult<void>::success();
}

auto ManagementService::get_job(jb::core::Uuid const& id, bool include_deleted)
    -> jb::core::Result<JobDefinition, jb::core::Error>
{
    if (_data->initialization_error) {
        return ServiceResult<JobDefinition>::failure(*_data->initialization_error);
    }
    auto found = _data->jobs.find_by_id(id, include_deleted);
    if (!found) {
        return ServiceResult<JobDefinition>::failure(std::move(found).error());
    }
    if (!found->has_value()) {
        return ServiceResult<JobDefinition>::failure(job_not_found());
    }
    return ServiceResult<JobDefinition>::success(std::move(**found));
}

auto ManagementService::list_jobs(JobListRequest const& request) -> jb::core::Result<JobPage, jb::core::Error>
{
    if (_data->initialization_error) {
        return ServiceResult<JobPage>::failure(*_data->initialization_error);
    }
    if (request.page.limit == 0 || request.page.limit > kMaximumPageSize) {
        return ServiceResult<JobPage>::failure(invalid_job_configuration("page_limit_out_of_range"));
    }
    if (request.state && !valid_job_state(*request.state)) {
        return ServiceResult<JobPage>::failure(invalid_job_configuration("unknown_job_state"));
    }
    if (request.type && !valid_job_type(*request.type)) {
        return ServiceResult<JobPage>::failure(invalid_job_configuration("unknown_job_type"));
    }

    auto queue_id = std::optional<jb::core::Uuid>{};
    if (request.queue) {
        auto selector = validate_selector(*request.queue);
        if (!selector) {
            return ServiceResult<JobPage>::failure(std::move(selector).error());
        }
        auto found_queue = find_queue(_data->queues, *request.queue, request.include_deleted);
        if (!found_queue) {
            return ServiceResult<JobPage>::failure(std::move(found_queue).error());
        }
        if (!found_queue->has_value()) {
            return ServiceResult<JobPage>::failure(queue_not_found());
        }
        queue_id = (**found_queue).id;
    }

    auto listed = _data->jobs.list(queue_id,
                                   request.include_deleted,
                                   request.state,
                                   request.type,
                                   request.page.limit + 1U,
                                   request.page.after_id);
    if (!listed) {
        return ServiceResult<JobPage>::failure(std::move(listed).error());
    }

    auto page = JobPage{.items = std::move(listed).value()};
    if (page.items.size() > request.page.limit) {
        page.items.resize(request.page.limit);
        page.next_after_id = page.items.back().id;
    }
    return ServiceResult<JobPage>::success(std::move(page));
}

} // namespace jb::jobu
