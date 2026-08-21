#include "idempotency_codec_priv.hpp"

#include "attribute_registry.hpp"
#include "job_validation_priv.hpp"
#include "json.hpp"
#include "queue_validation_priv.hpp"
#include "utc_timestamp.hpp"

#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace jb::jobu::detail {

namespace {

template <typename T>
using CodecResult = jb::core::Result<T, jb::core::Error>;

constexpr std::size_t kMaximumIdempotencyDocumentBytes = std::size_t{1024} * 1024U;

auto invalid_record(std::string_view reason, std::string_view cause = {}) -> jb::core::Error
{
    auto error = jb::core::Error{
        .category = jb::core::ErrorCategory::Internal,
        .code     = "jobu.idempotency.invalid_record",
        .message  = "Stored idempotency data is invalid",
        .detail   = "reason=" + std::string{reason},
    };
    if (!cause.empty()) {
        error.detail += " cause=" + std::string{cause};
    }
    return error;
}

auto json_null() -> jb::rpc::JsonValue
{
    return {};
}

auto json_string(std::string value) -> jb::rpc::JsonValue
{
    auto result = jb::rpc::JsonValue{};
    result.data = std::move(value);
    return result;
}

auto json_uint(std::uint64_t value) -> jb::rpc::JsonValue
{
    auto result = jb::rpc::JsonValue{};
    result.data = value;
    return result;
}

auto json_int(std::int64_t value) -> jb::rpc::JsonValue
{
    auto result = jb::rpc::JsonValue{};
    result.data = value;
    return result;
}

auto json_object(jb::rpc::JsonValue::Object value) -> jb::rpc::JsonValue
{
    auto result = jb::rpc::JsonValue{};
    result.data = std::move(value);
    return result;
}

auto serialize_document(jb::rpc::JsonValue const& value) -> CodecResult<std::string>
{
    auto serialized = jb::rpc::serialize_json(value);
    if (!serialized) {
        return CodecResult<std::string>::failure(invalid_record("encode_failed", serialized.error().code));
    }
    if (serialized->size() > kMaximumIdempotencyDocumentBytes) {
        return CodecResult<std::string>::failure(invalid_record("document_too_large"));
    }
    return CodecResult<std::string>::success(std::move(serialized).value());
}

auto parse_document(std::string_view text) -> CodecResult<jb::rpc::JsonValue>
{
    if (text.empty() || text.size() > kMaximumIdempotencyDocumentBytes) {
        return CodecResult<jb::rpc::JsonValue>::failure(invalid_record("document_size"));
    }
    auto parsed = jb::rpc::parse_json(text);
    if (!parsed) {
        return CodecResult<jb::rpc::JsonValue>::failure(invalid_record("invalid_json", parsed.error().code));
    }
    auto canonical = jb::rpc::serialize_json(*parsed);
    if (!canonical || *canonical != text) {
        return CodecResult<jb::rpc::JsonValue>::failure(invalid_record("noncanonical_json"));
    }
    return CodecResult<jb::rpc::JsonValue>::success(std::move(parsed).value());
}

auto object_with_members(jb::rpc::JsonValue const& value, std::initializer_list<std::string_view> names)
    -> CodecResult<jb::rpc::JsonValue::Object const*>
{
    if (!value.is_object()) {
        return CodecResult<jb::rpc::JsonValue::Object const*>::failure(invalid_record("expected_object"));
    }
    auto const& object = value.as_object();
    if (object.size() != names.size()) {
        return CodecResult<jb::rpc::JsonValue::Object const*>::failure(invalid_record("unexpected_members"));
    }
    for (auto const name : names) {
        if (!object.contains(name)) {
            return CodecResult<jb::rpc::JsonValue::Object const*>::failure(invalid_record("missing_member"));
        }
    }
    return CodecResult<jb::rpc::JsonValue::Object const*>::success(&object);
}

auto value_member(jb::rpc::JsonValue::Object const& object, std::string_view name) -> jb::rpc::JsonValue const&
{
    return object.find(name)->second;
}

auto text_member(jb::rpc::JsonValue::Object const& object, std::string_view name) -> CodecResult<std::string>
{
    auto const& value = value_member(object, name);
    if (!value.is_string()) {
        return CodecResult<std::string>::failure(invalid_record("expected_string"));
    }
    return CodecResult<std::string>::success(value.as_string());
}

auto unsigned_member(jb::rpc::JsonValue::Object const& object, std::string_view name) -> CodecResult<std::uint64_t>
{
    auto const& value = value_member(object, name);
    if (!value.is_uint()) {
        return CodecResult<std::uint64_t>::failure(invalid_record("expected_unsigned_integer"));
    }
    return CodecResult<std::uint64_t>::success(value.as_uint());
}

auto signed_member(jb::rpc::JsonValue::Object const& object, std::string_view name) -> CodecResult<std::int64_t>
{
    auto const& value = value_member(object, name);
    if (value.is_int()) {
        return CodecResult<std::int64_t>::success(value.as_int());
    }
    if (value.is_uint() && value.as_uint() <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return CodecResult<std::int64_t>::success(static_cast<std::int64_t>(value.as_uint()));
    }
    return CodecResult<std::int64_t>::failure(invalid_record("expected_signed_integer"));
}

auto decode_uuid(std::string_view text) -> CodecResult<jb::core::Uuid>
{
    auto parsed = jb::core::Uuid::parse(text);
    if (!parsed || parsed->to_string() != text) {
        return CodecResult<jb::core::Uuid>::failure(invalid_record("invalid_uuid"));
    }
    return CodecResult<jb::core::Uuid>::success(*parsed);
}

auto uuid_member(jb::rpc::JsonValue::Object const& object, std::string_view name) -> CodecResult<jb::core::Uuid>
{
    auto text = text_member(object, name);
    if (!text) {
        return CodecResult<jb::core::Uuid>::failure(std::move(text).error());
    }
    return decode_uuid(*text);
}

auto encode_time(jb::core::UtcTimePoint value) -> CodecResult<jb::rpc::JsonValue>
{
    auto formatted = format_utc_timestamp(value);
    if (!formatted) {
        return CodecResult<jb::rpc::JsonValue>::failure(invalid_record("invalid_time", formatted.error().code));
    }
    return CodecResult<jb::rpc::JsonValue>::success(json_string(std::move(formatted).value()));
}

auto time_member(jb::rpc::JsonValue::Object const& object, std::string_view name) -> CodecResult<jb::core::UtcTimePoint>
{
    auto text = text_member(object, name);
    if (!text) {
        return CodecResult<jb::core::UtcTimePoint>::failure(std::move(text).error());
    }
    auto parsed = parse_utc_timestamp(*text);
    if (!parsed) {
        return CodecResult<jb::core::UtcTimePoint>::failure(invalid_record("invalid_time", parsed.error().code));
    }
    auto formatted = format_utc_timestamp(*parsed);
    if (!formatted || *formatted != *text) {
        return CodecResult<jb::core::UtcTimePoint>::failure(invalid_record("noncanonical_time"));
    }
    return CodecResult<jb::core::UtcTimePoint>::success(*parsed);
}

auto nullable_time(jb::rpc::JsonValue::Object const& object, std::string_view name)
    -> CodecResult<std::optional<jb::core::UtcTimePoint>>
{
    if (value_member(object, name).is_null()) {
        return CodecResult<std::optional<jb::core::UtcTimePoint>>::success(std::nullopt);
    }
    auto value = time_member(object, name);
    if (!value) {
        return CodecResult<std::optional<jb::core::UtcTimePoint>>::failure(std::move(value).error());
    }
    return CodecResult<std::optional<jb::core::UtcTimePoint>>::success(*value);
}

auto recovery_policy_text(RecoveryPolicy value) -> std::string_view
{
    switch (value) {
        case RecoveryPolicy::FailInterrupted:
            return "fail_interrupted";
        case RecoveryPolicy::RetryInterrupted:
            return "retry_interrupted";
    }
    return {};
}

auto decode_recovery_policy(std::string_view text) -> CodecResult<RecoveryPolicy>
{
    if (text == "fail_interrupted") {
        return CodecResult<RecoveryPolicy>::success(RecoveryPolicy::FailInterrupted);
    }
    if (text == "retry_interrupted") {
        return CodecResult<RecoveryPolicy>::success(RecoveryPolicy::RetryInterrupted);
    }
    return CodecResult<RecoveryPolicy>::failure(invalid_record("invalid_recovery_policy"));
}

auto job_type_text(JobType value) -> std::string_view
{
    switch (value) {
        case JobType::Cli:
            return "cli";
        case JobType::Http:
            return "http";
    }
    return {};
}

auto decode_job_type(std::string_view text) -> CodecResult<JobType>
{
    if (text == "cli") {
        return CodecResult<JobType>::success(JobType::Cli);
    }
    if (text == "http") {
        return CodecResult<JobType>::success(JobType::Http);
    }
    return CodecResult<JobType>::failure(invalid_record("invalid_job_type"));
}

auto encode_schedule(JobSchedule const& schedule) -> CodecResult<jb::rpc::JsonValue>
{
    if (auto const* once = std::get_if<OnceSchedule>(&schedule)) {
        auto at = encode_time(once->planned_at);
        if (!at) {
            return CodecResult<jb::rpc::JsonValue>::failure(std::move(at).error());
        }
        return CodecResult<jb::rpc::JsonValue>::success(json_object({
            {"at",   std::move(at).value()},
            {"kind", json_string("once")  },
        }));
    }
    if (auto const* cron = std::get_if<CronSchedule>(&schedule)) {
        return CodecResult<jb::rpc::JsonValue>::success(json_object({
            {"expression", json_string(cron->expression)},
            {"kind",       json_string("cron")          },
            {"timezone",   json_string(cron->timezone)  },
        }));
    }
    return CodecResult<jb::rpc::JsonValue>::failure(invalid_record("invalid_schedule_kind"));
}

auto decode_schedule(jb::rpc::JsonValue const& value) -> CodecResult<JobSchedule>
{
    if (!value.is_object()) {
        return CodecResult<JobSchedule>::failure(invalid_record("invalid_schedule"));
    }
    auto kind = text_member(value.as_object(), "kind");
    if (!kind) {
        return CodecResult<JobSchedule>::failure(invalid_record("invalid_schedule_kind"));
    }
    if (*kind == "once") {
        auto object = object_with_members(value, {"at", "kind"});
        if (!object) {
            return CodecResult<JobSchedule>::failure(std::move(object).error());
        }
        auto at = time_member(**object, "at");
        if (!at) {
            return CodecResult<JobSchedule>::failure(std::move(at).error());
        }
        return CodecResult<JobSchedule>::success(OnceSchedule{.planned_at = *at});
    }
    if (*kind == "cron") {
        auto object = object_with_members(value, {"expression", "kind", "timezone"});
        if (!object) {
            return CodecResult<JobSchedule>::failure(std::move(object).error());
        }
        auto expression = text_member(**object, "expression");
        auto timezone   = text_member(**object, "timezone");
        if (!expression || !timezone) {
            return CodecResult<JobSchedule>::failure(invalid_record("invalid_cron_schedule"));
        }
        return CodecResult<JobSchedule>::success(CronSchedule{
            .expression = std::move(expression).value(),
            .timezone   = std::move(timezone).value(),
        });
    }
    return CodecResult<JobSchedule>::failure(invalid_record("invalid_schedule_kind"));
}

auto encode_attributes(AttributeSet const& values, AttributeRegistry const& attributes, AttributeScope scope)
    -> CodecResult<jb::rpc::JsonValue>
{
    auto encoded = attribute_set_to_json(values, attributes, scope);
    if (!encoded) {
        return CodecResult<jb::rpc::JsonValue>::failure(invalid_record("invalid_attributes", encoded.error().code));
    }
    return CodecResult<jb::rpc::JsonValue>::success(std::move(encoded).value());
}

auto decode_attributes(jb::rpc::JsonValue const& value, AttributeRegistry const& attributes, AttributeScope scope)
    -> CodecResult<AttributeSet>
{
    auto decoded = attribute_set_from_json(value, attributes, scope);
    if (!decoded) {
        return CodecResult<AttributeSet>::failure(invalid_record("invalid_attributes", decoded.error().code));
    }
    return CodecResult<AttributeSet>::success(std::move(decoded).value());
}

auto require_materialized(AttributeSet const& values, AttributeRegistry const& attributes) -> CodecResult<void>
{
    for (auto const& definition : attributes.definitions()) {
        if (definition.scopes.test(AttributeScope::Job) && !values.contains(definition.name)) {
            return CodecResult<void>::failure(invalid_record("incomplete_attributes"));
        }
    }
    return CodecResult<void>::success();
}

auto valid_payload(JobType type, jb::rpc::JsonValue const& payload) -> bool
{
    return static_cast<bool>(validate_and_serialize_job_payload(type, payload));
}

auto encode_optional_name(std::optional<std::string> const& name) -> jb::rpc::JsonValue
{
    return name ? json_string(*name) : json_null();
}

auto decode_optional_name(jb::rpc::JsonValue const& value) -> CodecResult<std::optional<std::string>>
{
    if (value.is_null()) {
        return CodecResult<std::optional<std::string>>::success(std::nullopt);
    }
    if (!value.is_string() || !is_valid_job_name(value.as_string())) {
        return CodecResult<std::optional<std::string>>::failure(invalid_record("invalid_job_name"));
    }
    return CodecResult<std::optional<std::string>>::success(value.as_string());
}

auto encode_retention(std::optional<std::chrono::seconds> value) -> CodecResult<jb::rpc::JsonValue>
{
    if (!value) {
        return CodecResult<jb::rpc::JsonValue>::success(json_null());
    }
    if (value->count() < 0) {
        return CodecResult<jb::rpc::JsonValue>::failure(invalid_record("negative_retention"));
    }
    return CodecResult<jb::rpc::JsonValue>::success(json_uint(static_cast<std::uint64_t>(value->count())));
}

auto decode_retention(jb::rpc::JsonValue const& value) -> CodecResult<std::optional<std::chrono::seconds>>
{
    if (value.is_null()) {
        return CodecResult<std::optional<std::chrono::seconds>>::success(std::nullopt);
    }
    if (!value.is_uint() || value.as_uint() > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return CodecResult<std::optional<std::chrono::seconds>>::failure(invalid_record("invalid_retention"));
    }
    return CodecResult<std::optional<std::chrono::seconds>>::success(
        std::chrono::seconds{static_cast<std::int64_t>(value.as_uint())});
}

auto validate_queue_fields(jb::rpc::JsonValue::Object const& object, AttributeRegistry const& attributes)
    -> CodecResult<void>
{
    auto name = text_member(object, "name");
    if (!name || !is_valid_queue_name(*name)) {
        return CodecResult<void>::failure(invalid_record("invalid_queue_name"));
    }
    auto weight      = unsigned_member(object, "weight");
    auto concurrency = unsigned_member(object, "concurrency_limit");
    if (!weight || *weight == 0 || *weight > std::numeric_limits<std::uint32_t>::max() || !concurrency ||
        *concurrency == 0 || *concurrency > std::numeric_limits<std::uint32_t>::max()) {
        return CodecResult<void>::failure(invalid_record("invalid_queue_limits"));
    }
    auto recovery = text_member(object, "recovery_policy");
    if (!recovery || !decode_recovery_policy(*recovery)) {
        return CodecResult<void>::failure(invalid_record("invalid_recovery_policy"));
    }
    auto defaults = decode_attributes(object.at("defaults"), attributes, AttributeScope::QueueDefault);
    if (!defaults) {
        return CodecResult<void>::failure(std::move(defaults).error());
    }
    auto retention = decode_retention(object.at("history_retention_seconds"));
    if (!retention) {
        return CodecResult<void>::failure(std::move(retention).error());
    }
    auto warning = unsigned_member(object, "runnable_wait_warning_ms");
    if (!warning || *warning > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return CodecResult<void>::failure(invalid_record("invalid_warning_duration"));
    }
    return CodecResult<void>::success();
}

auto queue_state_text(QueueState value) -> std::string_view
{
    switch (value) {
        case QueueState::Active:
            return "active";
        case QueueState::Suspending:
            return "suspending";
        case QueueState::Suspended:
            return "suspended";
        case QueueState::Deleted:
            return "deleted";
    }
    return {};
}

auto job_state_text(JobState value) -> std::string_view
{
    switch (value) {
        case JobState::Active:
            return "active";
        case JobState::Suspending:
            return "suspending";
        case JobState::Suspended:
            return "suspended";
        case JobState::Deleted:
            return "deleted";
    }
    return {};
}

} // anonymous namespace

auto encode_queue_create_idempotency_request(CreateQueueRequest const& request, AttributeRegistry const& attributes)
    -> jb::core::Result<std::string, jb::core::Error>
{
    auto defaults = encode_attributes(request.defaults, attributes, AttributeScope::QueueDefault);
    if (!defaults) {
        return CodecResult<std::string>::failure(std::move(defaults).error());
    }
    auto retention = encode_retention(request.history_retention);
    if (!retention || request.runnable_wait_warning.count() < 0) {
        return CodecResult<std::string>::failure(invalid_record("invalid_queue_request"));
    }
    return serialize_document(json_object({
        {"concurrency_limit",         json_uint(request.concurrency_limit)                                        },
        {"defaults",                  std::move(defaults).value()                                                 },
        {"history_retention_seconds", std::move(retention).value()                                                },
        {"name",                      json_string(request.name)                                                   },
        {"recovery_policy",           json_string(std::string{recovery_policy_text(request.recovery_policy)})     },
        {"runnable_wait_warning_ms",  json_uint(static_cast<std::uint64_t>(request.runnable_wait_warning.count()))},
        {"weight",                    json_uint(request.weight)                                                   },
    }));
}

auto validate_queue_create_idempotency_request(std::string_view request_json, AttributeRegistry const& attributes)
    -> jb::core::Result<void, jb::core::Error>
{
    auto parsed = parse_document(request_json);
    if (!parsed) {
        return CodecResult<void>::failure(std::move(parsed).error());
    }
    auto object = object_with_members(*parsed,
                                      {"concurrency_limit",
                                       "defaults",
                                       "history_retention_seconds",
                                       "name",
                                       "recovery_policy",
                                       "runnable_wait_warning_ms",
                                       "weight"});
    if (!object) {
        return CodecResult<void>::failure(std::move(object).error());
    }
    return validate_queue_fields(**object, attributes);
}

auto encode_queue_idempotency_result(Queue const& queue, AttributeRegistry const& attributes)
    -> jb::core::Result<std::string, jb::core::Error>
{
    auto defaults  = encode_attributes(queue.defaults, attributes, AttributeScope::QueueDefault);
    auto retention = encode_retention(queue.history_retention);
    auto created   = encode_time(queue.created_at);
    auto updated   = encode_time(queue.updated_at);
    if (!defaults || !retention || !created || !updated || queue.runnable_wait_warning.count() < 0) {
        return CodecResult<std::string>::failure(invalid_record("invalid_queue_result"));
    }
    auto deleted =
        queue.deleted_at ? encode_time(*queue.deleted_at) : CodecResult<jb::rpc::JsonValue>::success(json_null());
    if (!deleted) {
        return CodecResult<std::string>::failure(std::move(deleted).error());
    }
    return serialize_document(json_object({
        {"concurrency_limit",         json_uint(queue.concurrency_limit)                                        },
        {"created_at",                std::move(created).value()                                                },
        {"defaults",                  std::move(defaults).value()                                               },
        {"deleted_at",                std::move(deleted).value()                                                },
        {"history_retention_seconds", std::move(retention).value()                                              },
        {"id",                        json_string(queue.id.to_string())                                         },
        {"name",                      json_string(queue.name)                                                   },
        {"recovery_policy",           json_string(std::string{recovery_policy_text(queue.recovery_policy)})     },
        {"runnable_wait_warning_ms",  json_uint(static_cast<std::uint64_t>(queue.runnable_wait_warning.count()))},
        {"state",                     json_string(std::string{queue_state_text(queue.state)})                   },
        {"updated_at",                std::move(updated).value()                                                },
        {"weight",                    json_uint(queue.weight)                                                   },
    }));
}

auto decode_queue_idempotency_result(std::string_view result_json, AttributeRegistry const& attributes)
    -> jb::core::Result<Queue, jb::core::Error>
{
    auto parsed = parse_document(result_json);
    if (!parsed) {
        return CodecResult<Queue>::failure(std::move(parsed).error());
    }
    auto object = object_with_members(*parsed,
                                      {"concurrency_limit",
                                       "created_at",
                                       "defaults",
                                       "deleted_at",
                                       "history_retention_seconds",
                                       "id",
                                       "name",
                                       "recovery_policy",
                                       "runnable_wait_warning_ms",
                                       "state",
                                       "updated_at",
                                       "weight"});
    if (!object) {
        return CodecResult<Queue>::failure(std::move(object).error());
    }
    auto valid = validate_queue_fields(**object, attributes);
    if (!valid) {
        return CodecResult<Queue>::failure(std::move(valid).error());
    }
    auto id            = uuid_member(**object, "id");
    auto name          = text_member(**object, "name");
    auto weight        = unsigned_member(**object, "weight");
    auto concurrency   = unsigned_member(**object, "concurrency_limit");
    auto recovery_text = text_member(**object, "recovery_policy");
    auto recovery  = recovery_text ? decode_recovery_policy(*recovery_text)
                                   : CodecResult<RecoveryPolicy>::failure(invalid_record("invalid_recovery_policy"));
    auto defaults  = decode_attributes((**object).at("defaults"), attributes, AttributeScope::QueueDefault);
    auto retention = decode_retention((**object).at("history_retention_seconds"));
    auto warning   = unsigned_member(**object, "runnable_wait_warning_ms");
    auto state     = text_member(**object, "state");
    auto created   = time_member(**object, "created_at");
    auto updated   = time_member(**object, "updated_at");
    auto deleted   = nullable_time(**object, "deleted_at");
    if (!id || !name || !weight || !concurrency || !recovery || !defaults || !retention || !warning || !state ||
        !created || !updated || !deleted || *state != "active" || deleted->has_value() || *created != *updated) {
        return CodecResult<Queue>::failure(invalid_record("invalid_queue_result"));
    }
    return CodecResult<Queue>::success({
        .id                    = *id,
        .name                  = std::move(name).value(),
        .state                 = QueueState::Active,
        .weight                = static_cast<std::uint32_t>(*weight),
        .concurrency_limit     = static_cast<std::uint32_t>(*concurrency),
        .recovery_policy       = *recovery,
        .defaults              = std::move(defaults).value(),
        .history_retention     = *retention,
        .runnable_wait_warning = std::chrono::milliseconds{static_cast<std::int64_t>(*warning)},
        .created_at            = *created,
        .updated_at            = *updated,
        .deleted_at            = std::nullopt,
    });
}

auto encode_job_create_idempotency_request(CreateJobRequest const&  request,
                                           jb::core::Uuid const&    resolved_queue_id,
                                           AttributeRegistry const& attributes)
    -> jb::core::Result<std::string, jb::core::Error>
{
    auto schedule           = encode_schedule(request.schedule);
    auto encoded_attributes = encode_attributes(request.attributes, attributes, AttributeScope::Job);
    if (!schedule || !encoded_attributes || job_type_text(request.type).empty()) {
        return CodecResult<std::string>::failure(invalid_record("invalid_job_request"));
    }
    return serialize_document(json_object({
        {"attributes", std::move(encoded_attributes).value()                },
        {"name",       encode_optional_name(request.name)                   },
        {"payload",    request.payload                                      },
        {"priority",   json_int(request.priority)                           },
        {"queue_id",   json_string(resolved_queue_id.to_string())           },
        {"schedule",   std::move(schedule).value()                          },
        {"type",       json_string(std::string{job_type_text(request.type)})},
    }));
}

auto validate_job_create_idempotency_request(std::string_view request_json, AttributeRegistry const& attributes)
    -> jb::core::Result<void, jb::core::Error>
{
    auto parsed = parse_document(request_json);
    if (!parsed) {
        return CodecResult<void>::failure(std::move(parsed).error());
    }
    auto object =
        object_with_members(*parsed, {"attributes", "name", "payload", "priority", "queue_id", "schedule", "type"});
    if (!object) {
        return CodecResult<void>::failure(std::move(object).error());
    }
    auto queue_id  = uuid_member(**object, "queue_id");
    auto name      = decode_optional_name((**object).at("name"));
    auto type_text = text_member(**object, "type");
    auto type =
        type_text ? decode_job_type(*type_text) : CodecResult<JobType>::failure(invalid_record("invalid_job_type"));
    auto schedule         = decode_schedule((**object).at("schedule"));
    auto priority         = signed_member(**object, "priority");
    auto attributes_value = decode_attributes((**object).at("attributes"), attributes, AttributeScope::Job);
    if (!queue_id || !name || !type || !schedule || !priority || *priority < std::numeric_limits<std::int32_t>::min() ||
        *priority > std::numeric_limits<std::int32_t>::max() || !attributes_value ||
        !valid_payload(type ? *type : JobType::Cli, (**object).at("payload"))) {
        return CodecResult<void>::failure(invalid_record("invalid_job_request"));
    }
    return CodecResult<void>::success();
}

auto encode_job_idempotency_result(JobDefinition const& job, AttributeRegistry const& attributes)
    -> jb::core::Result<std::string, jb::core::Error>
{
    auto schedule           = encode_schedule(job.schedule);
    auto encoded_attributes = encode_attributes(job.attributes, attributes, AttributeScope::Job);
    auto created            = encode_time(job.created_at);
    auto updated            = encode_time(job.updated_at);
    if (!schedule || !encoded_attributes || !created || !updated || job_type_text(job.type).empty()) {
        return CodecResult<std::string>::failure(invalid_record("invalid_job_result"));
    }
    auto deleted =
        job.deleted_at ? encode_time(*job.deleted_at) : CodecResult<jb::rpc::JsonValue>::success(json_null());
    if (!deleted) {
        return CodecResult<std::string>::failure(std::move(deleted).error());
    }
    return serialize_document(json_object({
        {"attributes", std::move(encoded_attributes).value()              },
        {"created_at", std::move(created).value()                         },
        {"deleted_at", std::move(deleted).value()                         },
        {"id",         json_string(job.id.to_string())                    },
        {"name",       encode_optional_name(job.name)                     },
        {"payload",    job.payload                                        },
        {"priority",   json_int(job.priority)                             },
        {"queue_id",   json_string(job.queue_id.to_string())              },
        {"revision",   json_uint(job.revision)                            },
        {"schedule",   std::move(schedule).value()                        },
        {"state",      json_string(std::string{job_state_text(job.state)})},
        {"type",       json_string(std::string{job_type_text(job.type)})  },
        {"updated_at", std::move(updated).value()                         },
    }));
}

auto decode_job_idempotency_result(std::string_view result_json, AttributeRegistry const& attributes)
    -> jb::core::Result<JobDefinition, jb::core::Error>
{
    auto parsed = parse_document(result_json);
    if (!parsed) {
        return CodecResult<JobDefinition>::failure(std::move(parsed).error());
    }
    auto object = object_with_members(*parsed,
                                      {"attributes",
                                       "created_at",
                                       "deleted_at",
                                       "id",
                                       "name",
                                       "payload",
                                       "priority",
                                       "queue_id",
                                       "revision",
                                       "schedule",
                                       "state",
                                       "type",
                                       "updated_at"});
    if (!object) {
        return CodecResult<JobDefinition>::failure(std::move(object).error());
    }
    auto id        = uuid_member(**object, "id");
    auto queue_id  = uuid_member(**object, "queue_id");
    auto revision  = unsigned_member(**object, "revision");
    auto name      = decode_optional_name((**object).at("name"));
    auto state     = text_member(**object, "state");
    auto type_text = text_member(**object, "type");
    auto type =
        type_text ? decode_job_type(*type_text) : CodecResult<JobType>::failure(invalid_record("invalid_job_type"));
    auto schedule           = decode_schedule((**object).at("schedule"));
    auto priority           = signed_member(**object, "priority");
    auto decoded_attributes = decode_attributes((**object).at("attributes"), attributes, AttributeScope::Job);
    auto created            = time_member(**object, "created_at");
    auto updated            = time_member(**object, "updated_at");
    auto deleted            = nullable_time(**object, "deleted_at");
    if (!id || !queue_id || !revision || *revision != 1 || !name || !state || *state != "active" || !type ||
        !schedule || !priority || *priority < std::numeric_limits<std::int32_t>::min() ||
        *priority > std::numeric_limits<std::int32_t>::max() || !decoded_attributes || !created || !updated ||
        !deleted || deleted->has_value() || *created != *updated || !valid_payload(*type, (**object).at("payload"))) {
        return CodecResult<JobDefinition>::failure(invalid_record("invalid_job_result"));
    }
    auto complete = require_materialized(*decoded_attributes, attributes);
    if (!complete) {
        return CodecResult<JobDefinition>::failure(std::move(complete).error());
    }
    return CodecResult<JobDefinition>::success({
        .id         = *id,
        .queue_id   = *queue_id,
        .revision   = 1,
        .name       = std::move(name).value(),
        .state      = JobState::Active,
        .type       = *type,
        .schedule   = std::move(schedule).value(),
        .priority   = static_cast<std::int32_t>(*priority),
        .attributes = std::move(decoded_attributes).value(),
        .payload    = (**object).at("payload"),
        .created_at = *created,
        .updated_at = *updated,
        .deleted_at = std::nullopt,
    });
}

} // namespace jb::jobu::detail
