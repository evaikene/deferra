#include "management.hpp"

#include "attempt_repository_priv.hpp"
#include "attribute_codec_priv.hpp"
#include "attribute_registry.hpp"
#include "job_repository_priv.hpp"
#include "job_validation_priv.hpp"
#include "json.hpp"
#include "queue_repository_priv.hpp"
#include "queue_validation_priv.hpp"
#include "run_repository_priv.hpp"
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

constexpr std::size_t kMaximumAttributeDocumentBytes = 256U * 1024U;
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

auto cron_unavailable() -> jb::core::Error
{
    return service_error(jb::core::ErrorCategory::Unsupported,
                         "jobu.schedule.cron_unavailable",
                         "Recurring schedules are unavailable in Phase 3");
}

auto invalid_idempotency_key() -> jb::core::Error
{
    return service_error(jb::core::ErrorCategory::InvalidArgument,
                         "jobu.idempotency.invalid_key",
                         "Idempotency key must contain 1 through 128 valid UTF-8 bytes");
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

auto validate_job_payload(JobType type, jb::rpc::JsonValue const& payload) -> ServiceResult<detail::ValidatedJobPayload>
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
            jb::core::UuidGenerator& uuid_generator_value,
            jb::core::TimeSource&    time_source_value,
            AttributeSet             daemon_defaults_value)
        : database{database_value}
        , attributes{attributes_value}
        , uuid_generator{uuid_generator_value}
        , time_source{time_source_value}
        , daemon_defaults{std::move(daemon_defaults_value)}
        , queues{database_value, attributes_value}
        , jobs{database_value, attributes_value}
        , runs{database_value, attributes_value}
        , attempts{database_value}
    {
        auto validated = validate_attributes(attributes, daemon_defaults, AttributeScope::DaemonDefault);
        if (!validated) {
            initialization_error = std::move(validated).error();
        }
    }

    jb::db::Database&              database;
    AttributeRegistry const&       attributes;
    jb::core::UuidGenerator&       uuid_generator;
    jb::core::TimeSource&          time_source;
    AttributeSet                   daemon_defaults;
    detail::QueueRepository        queues;
    detail::JobRepository          jobs;
    detail::RunRepository          runs;
    detail::AttemptRepository      attempts;
    std::optional<jb::core::Error> initialization_error;
};

ManagementService::ManagementService(jb::db::Database&        database,
                                     AttributeRegistry const& attributes,
                                     jb::core::UuidGenerator& uuid_generator,
                                     jb::core::TimeSource&    time_source,
                                     AttributeSet             daemon_defaults)
    : _data{std::make_unique<Private>(database, attributes, uuid_generator, time_source, std::move(daemon_defaults))}
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

    auto begun = jb::db::Transaction::begin(_data->database);
    if (!begun) {
        return ServiceResult<Queue>::failure(std::move(begun).error());
    }
    auto transaction = std::move(begun).value();
    auto existing    = _data->queues.find_by_name(queue.name, false);
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
    auto committed = transaction.commit();
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

auto ManagementService::create_job(CreateJobRequest request) -> jb::core::Result<JobDefinition, jb::core::Error>
{
    if (_data->initialization_error) {
        return ServiceResult<JobDefinition>::failure(*_data->initialization_error);
    }
    auto selector = validate_selector(request.queue);
    if (!selector) {
        return ServiceResult<JobDefinition>::failure(std::move(selector).error());
    }
    auto name = validate_job_name(request.name);
    if (!name) {
        return ServiceResult<JobDefinition>::failure(std::move(name).error());
    }
    if (std::holds_alternative<CronSchedule>(request.schedule)) {
        return ServiceResult<JobDefinition>::failure(cron_unavailable());
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
    auto job_id = _data->uuid_generator.generate();
    if (!job_id) {
        return ServiceResult<JobDefinition>::failure(std::move(job_id).error());
    }
    auto run_id = _data->uuid_generator.generate();
    if (!run_id) {
        return ServiceResult<JobDefinition>::failure(std::move(run_id).error());
    }
    auto const now = _data->time_source.utc_now();

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
    auto const planned_at = std::get<OnceSchedule>(job.schedule).planned_at;
    auto       run        = detail::ScheduleOwnedRunInsert{
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

    auto inserted_job = _data->jobs.insert(job, *serialized_attributes, *payload);
    if (!inserted_job) {
        return ServiceResult<JobDefinition>::failure(std::move(inserted_job).error());
    }
    auto inserted_run = _data->runs.insert_schedule_owned(run);
    if (!inserted_run) {
        return ServiceResult<JobDefinition>::failure(std::move(inserted_run).error());
    }
    auto committed = transaction.commit();
    if (!committed) {
        return ServiceResult<JobDefinition>::failure(std::move(committed).error());
    }
    return ServiceResult<JobDefinition>::success(std::move(job));
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
        return ServiceResult<JobDefinition>::failure(cron_unavailable());
    }
    auto attribute_changes = validate_attributes(_data->attributes, request.attribute_changes, AttributeScope::Job);
    if (!attribute_changes) {
        return ServiceResult<JobDefinition>::failure(std::move(attribute_changes).error());
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
    auto started = _data->attempts.has_started_for_job(replacement.id);
    if (!started) {
        return ServiceResult<JobDefinition>::failure(std::move(started).error());
    }
    if (*started) {
        return ServiceResult<JobDefinition>::failure(job_immutable());
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
        replacement.attributes.insert_or_assign(std::move(name), std::move(value));
    }
    if (request.payload) {
        replacement.payload = std::move(*request.payload);
    }
    if (std::holds_alternative<CronSchedule>(replacement.schedule)) {
        return ServiceResult<JobDefinition>::failure(cron_unavailable());
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

    auto const planned_at = std::get<OnceSchedule>(replacement.schedule).planned_at;
    auto       refreshed =
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
