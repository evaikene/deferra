#include "domain_storage_priv.hpp"

#include "attribute_codec_priv.hpp"
#include "json.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace jb::jobu::detail {

namespace {

template <typename T>
using StorageResult = jb::core::Result<T, jb::core::Error>;

constexpr std::size_t kMaximumJsonDocumentBytes = std::size_t{256} * 1024U;

constexpr auto kJobRunColumns =
    "jobu_runs.id AS run_id, jobu_runs.job_id AS run_job_id, "
    "jobu_runs.job_revision AS run_job_revision, jobu_runs.queue_id AS run_queue_id, "
    "jobu_runs.origin AS run_origin, jobu_runs.schedule_owned AS run_schedule_owned, "
    "jobu_runs.planned_at_us AS run_planned_at_us, jobu_runs.runnable_at_us AS run_runnable_at_us, "
    "jobu_runs.started_at_us AS run_started_at_us, jobu_runs.completed_at_us AS run_completed_at_us, "
    "jobu_runs.type AS run_type, jobu_runs.priority AS run_priority, "
    "jobu_runs.attributes_json AS run_attributes_json, jobu_runs.payload_json AS run_payload_json, "
    "jobu_runs.state AS run_state, jobu_runs.result_json AS run_result_json";

auto storage_error(std::string_view code, std::string_view message, std::string_view field, std::string_view reason)
    -> jb::core::Error
{
    auto detail = std::string{};
    if (!field.empty()) {
        detail = "field=" + std::string{field};
    }
    if (!reason.empty()) {
        if (!detail.empty()) {
            detail.push_back(' ');
        }
        detail += "reason=" + std::string{reason};
    }
    return {
        .category = jb::core::ErrorCategory::Internal,
        .code     = std::string{code},
        .message  = std::string{message},
        .detail   = std::move(detail),
    };
}

auto required_value(jb::db::Record const& record, std::string_view field, std::string_view code)
    -> StorageResult<jb::db::Value const*>
{
    auto const* value = record.value(field);
    if (value == nullptr) {
        return StorageResult<jb::db::Value const*>::failure(
            storage_error(code, "Persisted JobU data is invalid", field, "missing"));
    }
    return StorageResult<jb::db::Value const*>::success(value);
}

auto decode_timestamp(jb::db::Value const& value, std::string_view field) -> StorageResult<jb::core::UtcTimePoint>
{
    auto const* integer = std::get_if<std::int64_t>(&value);
    if (integer == nullptr) {
        return StorageResult<jb::core::UtcTimePoint>::failure(
            storage_error("jobu.storage.invalid_time", "Persisted UTC time is invalid", field, "wrong_type"));
    }

    using Microseconds = std::chrono::microseconds;
    auto const minimum = std::chrono::ceil<Microseconds>(jb::core::UtcTimePoint::min().time_since_epoch()).count();
    auto const maximum = std::chrono::floor<Microseconds>(jb::core::UtcTimePoint::max().time_since_epoch()).count();
    if (*integer < minimum || *integer > maximum) {
        return StorageResult<jb::core::UtcTimePoint>::failure(
            storage_error("jobu.storage.invalid_time", "Persisted UTC time is invalid", field, "out_of_range"));
    }
    return StorageResult<jb::core::UtcTimePoint>::success(
        jb::core::UtcTimePoint{std::chrono::duration_cast<jb::core::UtcClock::duration>(Microseconds{*integer})});
}

template <typename Unsigned>
auto positive_unsigned_to_storage(Unsigned value) -> StorageResult<jb::db::Value>
{
    static_assert(std::is_unsigned_v<Unsigned>);
    if (value == 0 || value > static_cast<Unsigned>(std::numeric_limits<std::int64_t>::max())) {
        return StorageResult<jb::db::Value>::failure(
            storage_error("jobu.storage.invalid_integer", "JobU integer cannot be persisted", {}, "out_of_range"));
    }
    return StorageResult<jb::db::Value>::success(static_cast<std::int64_t>(value));
}

template <typename Unsigned>
auto read_positive_unsigned(jb::db::Record const& record, std::string_view field) -> StorageResult<Unsigned>
{
    auto value = required_value(record, field, "jobu.storage.invalid_integer");
    if (!value) {
        return StorageResult<Unsigned>::failure(std::move(value).error());
    }
    auto const* integer = std::get_if<std::int64_t>(value.value());
    if (integer == nullptr || *integer <= 0 ||
        static_cast<std::uint64_t>(*integer) > static_cast<std::uint64_t>(std::numeric_limits<Unsigned>::max())) {
        return StorageResult<Unsigned>::failure(storage_error("jobu.storage.invalid_integer",
                                                              "Persisted JobU integer is invalid",
                                                              field,
                                                              integer == nullptr ? "wrong_type" : "out_of_range"));
    }
    return StorageResult<Unsigned>::success(static_cast<Unsigned>(*integer));
}

template <typename Rep>
auto nonnegative_duration_count_to_storage(Rep count) -> StorageResult<jb::db::Value>
{
    static_assert(std::is_integral_v<Rep>);
    if (count < 0 || !std::in_range<std::int64_t>(count)) {
        return StorageResult<jb::db::Value>::failure(
            storage_error("jobu.storage.invalid_integer", "JobU duration cannot be persisted", {}, "out_of_range"));
    }
    return StorageResult<jb::db::Value>::success(static_cast<std::int64_t>(count));
}

auto json_error(std::string_view field, std::string_view reason) -> jb::core::Error
{
    return storage_error("jobu.storage.invalid_json", "Persisted JSON document is invalid", field, reason);
}

auto invalid_run(std::string_view reason) -> jb::core::Error
{
    return storage_error("jobu.storage.invariant", "Persisted run data violates a JobU invariant", {}, reason);
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

auto decode_json(jb::db::Value const& value, std::string_view field, bool require_object, std::size_t max_size)
    -> StorageResult<jb::core::JsonValue>
{
    auto const* text = std::get_if<std::string>(&value);
    if (text == nullptr) {
        return StorageResult<jb::core::JsonValue>::failure(json_error(field, "wrong_type"));
    }
    if (text->size() > max_size) {
        return StorageResult<jb::core::JsonValue>::failure(json_error(field, "too_large"));
    }

    auto parsed = jb::core::parse_json(*text);
    if (!parsed) {
        return StorageResult<jb::core::JsonValue>::failure(json_error(field, parsed.error().code));
    }
    if (require_object && !parsed->is_object()) {
        return StorageResult<jb::core::JsonValue>::failure(json_error(field, "object_required"));
    }
    return parsed;
}

template <typename Enum, typename Text, std::size_t Size>
auto read_enum(jb::db::Record const&                          record,
               std::string_view                               field,
               std::array<std::pair<Text, Enum>, Size> const& values) -> StorageResult<Enum>
{
    auto value = required_value(record, field, "jobu.storage.invalid_enum");
    if (!value) {
        return StorageResult<Enum>::failure(std::move(value).error());
    }
    auto const* text = std::get_if<std::string>(value.value());
    if (text == nullptr) {
        return StorageResult<Enum>::failure(
            storage_error("jobu.storage.invalid_enum", "Persisted enum value is invalid", field, "wrong_type"));
    }
    auto const found = std::ranges::find_if(values, [text](auto const& entry) { return entry.first == *text; });
    if (found == values.end()) {
        return StorageResult<Enum>::failure(
            storage_error("jobu.storage.invalid_enum", "Persisted enum value is invalid", field, "unknown_text"));
    }
    return StorageResult<Enum>::success(found->second);
}

} // anonymous namespace

auto uuid_to_storage(jb::core::Uuid const& value) -> jb::db::Value
{
    auto const& bytes = value.bytes();
    return jb::core::ByteBuffer{bytes.begin(), bytes.end()};
}

auto read_uuid(jb::db::Record const& record, std::string_view field)
    -> jb::core::Result<jb::core::Uuid, jb::core::Error>
{
    auto value = required_value(record, field, "jobu.storage.invalid_uuid");
    if (!value) {
        return StorageResult<jb::core::Uuid>::failure(std::move(value).error());
    }
    auto const* bytes = std::get_if<jb::core::ByteBuffer>(value.value());
    if (bytes == nullptr || bytes->size() != jb::core::Uuid::Storage{}.size()) {
        return StorageResult<jb::core::Uuid>::failure(storage_error("jobu.storage.invalid_uuid",
                                                                    "Persisted UUID is invalid",
                                                                    field,
                                                                    bytes == nullptr ? "wrong_type" : "wrong_size"));
    }

    auto storage = jb::core::Uuid::Storage{};
    std::ranges::copy(*bytes, storage.begin());
    return StorageResult<jb::core::Uuid>::success(jb::core::Uuid{storage});
}

auto timestamp_to_storage(jb::core::UtcTimePoint value) -> jb::core::Result<jb::db::Value, jb::core::Error>
{
    auto const microseconds = std::chrono::floor<std::chrono::microseconds>(value.time_since_epoch()).count();
    return StorageResult<jb::db::Value>::success(microseconds);
}

auto read_timestamp(jb::db::Record const& record, std::string_view field)
    -> jb::core::Result<jb::core::UtcTimePoint, jb::core::Error>
{
    auto value = required_value(record, field, "jobu.storage.invalid_time");
    if (!value) {
        return StorageResult<jb::core::UtcTimePoint>::failure(std::move(value).error());
    }
    return decode_timestamp(**value, field);
}

auto read_optional_timestamp(jb::db::Record const& record, std::string_view field)
    -> jb::core::Result<std::optional<jb::core::UtcTimePoint>, jb::core::Error>
{
    auto value = required_value(record, field, "jobu.storage.invalid_time");
    if (!value) {
        return StorageResult<std::optional<jb::core::UtcTimePoint>>::failure(std::move(value).error());
    }
    if (std::holds_alternative<jb::db::Null>(**value)) {
        return StorageResult<std::optional<jb::core::UtcTimePoint>>::success(std::nullopt);
    }
    auto decoded = decode_timestamp(**value, field);
    if (!decoded) {
        return StorageResult<std::optional<jb::core::UtcTimePoint>>::failure(std::move(decoded).error());
    }
    return StorageResult<std::optional<jb::core::UtcTimePoint>>::success(*decoded);
}

auto boolean_to_storage(bool value) -> jb::db::Value
{
    return std::int64_t{value ? 1 : 0};
}

auto read_boolean(jb::db::Record const& record, std::string_view field) -> jb::core::Result<bool, jb::core::Error>
{
    auto value = required_value(record, field, "jobu.storage.invalid_boolean");
    if (!value) {
        return StorageResult<bool>::failure(std::move(value).error());
    }
    auto const* integer = std::get_if<std::int64_t>(value.value());
    if (integer == nullptr || (*integer != 0 && *integer != 1)) {
        return StorageResult<bool>::failure(storage_error("jobu.storage.invalid_boolean",
                                                          "Persisted boolean is invalid",
                                                          field,
                                                          integer == nullptr ? "wrong_type" : "out_of_range"));
    }
    return StorageResult<bool>::success(*integer == 1);
}

auto revision_to_storage(JobRevision value) -> jb::core::Result<jb::db::Value, jb::core::Error>
{
    return positive_unsigned_to_storage(value);
}

auto read_revision(jb::db::Record const& record, std::string_view field)
    -> jb::core::Result<JobRevision, jb::core::Error>
{
    return read_positive_unsigned<JobRevision>(record, field);
}

auto attempt_number_to_storage(AttemptNumber value) -> jb::core::Result<jb::db::Value, jb::core::Error>
{
    return positive_unsigned_to_storage(value);
}

auto read_attempt_number(jb::db::Record const& record, std::string_view field)
    -> jb::core::Result<AttemptNumber, jb::core::Error>
{
    return read_positive_unsigned<AttemptNumber>(record, field);
}

auto int32_to_storage(std::int32_t value) -> jb::db::Value
{
    return static_cast<std::int64_t>(value);
}

auto read_int32(jb::db::Record const& record, std::string_view field) -> jb::core::Result<std::int32_t, jb::core::Error>
{
    auto value = required_value(record, field, "jobu.storage.invalid_integer");
    if (!value) {
        return StorageResult<std::int32_t>::failure(std::move(value).error());
    }
    auto const* integer = std::get_if<std::int64_t>(value.value());
    if (integer == nullptr || *integer < std::numeric_limits<std::int32_t>::min() ||
        *integer > std::numeric_limits<std::int32_t>::max()) {
        return StorageResult<std::int32_t>::failure(storage_error("jobu.storage.invalid_integer",
                                                                  "Persisted JobU integer is invalid",
                                                                  field,
                                                                  integer == nullptr ? "wrong_type" : "out_of_range"));
    }
    return StorageResult<std::int32_t>::success(static_cast<std::int32_t>(*integer));
}

auto positive_uint32_to_storage(std::uint32_t value) -> jb::core::Result<jb::db::Value, jb::core::Error>
{
    return positive_unsigned_to_storage(value);
}

auto read_positive_uint32(jb::db::Record const& record, std::string_view field)
    -> jb::core::Result<std::uint32_t, jb::core::Error>
{
    return read_positive_unsigned<std::uint32_t>(record, field);
}

auto optional_nonnegative_seconds_to_storage(std::optional<std::chrono::seconds> value)
    -> jb::core::Result<jb::db::Value, jb::core::Error>
{
    if (!value) {
        return StorageResult<jb::db::Value>::success(jb::db::Null{});
    }
    return nonnegative_duration_count_to_storage(value->count());
}

auto read_optional_nonnegative_seconds(jb::db::Record const& record, std::string_view field)
    -> jb::core::Result<std::optional<std::chrono::seconds>, jb::core::Error>
{
    auto value = required_value(record, field, "jobu.storage.invalid_integer");
    if (!value) {
        return StorageResult<std::optional<std::chrono::seconds>>::failure(std::move(value).error());
    }
    if (std::holds_alternative<jb::db::Null>(**value)) {
        return StorageResult<std::optional<std::chrono::seconds>>::success(std::nullopt);
    }
    auto const* integer = std::get_if<std::int64_t>(value.value());
    if (integer == nullptr || *integer < 0) {
        return StorageResult<std::optional<std::chrono::seconds>>::failure(
            storage_error("jobu.storage.invalid_integer",
                          "Persisted JobU duration is invalid",
                          field,
                          integer == nullptr ? "wrong_type" : "out_of_range"));
    }
    return StorageResult<std::optional<std::chrono::seconds>>::success(std::chrono::seconds{*integer});
}

auto nonnegative_milliseconds_to_storage(std::chrono::milliseconds value)
    -> jb::core::Result<jb::db::Value, jb::core::Error>
{
    return nonnegative_duration_count_to_storage(value.count());
}

auto read_nonnegative_milliseconds(jb::db::Record const& record, std::string_view field)
    -> jb::core::Result<std::chrono::milliseconds, jb::core::Error>
{
    auto value = required_value(record, field, "jobu.storage.invalid_integer");
    if (!value) {
        return StorageResult<std::chrono::milliseconds>::failure(std::move(value).error());
    }
    auto const* integer = std::get_if<std::int64_t>(value.value());
    if (integer == nullptr || *integer < 0) {
        return StorageResult<std::chrono::milliseconds>::failure(
            storage_error("jobu.storage.invalid_integer",
                          "Persisted JobU duration is invalid",
                          field,
                          integer == nullptr ? "wrong_type" : "out_of_range"));
    }
    return StorageResult<std::chrono::milliseconds>::success(std::chrono::milliseconds{*integer});
}

auto read_text(jb::db::Record const& record, std::string_view field) -> jb::core::Result<std::string, jb::core::Error>
{
    auto value = required_value(record, field, "jobu.storage.invalid_text");
    if (!value) {
        return StorageResult<std::string>::failure(std::move(value).error());
    }
    auto const* text = std::get_if<std::string>(value.value());
    if (text == nullptr) {
        return StorageResult<std::string>::failure(
            storage_error("jobu.storage.invalid_text", "Persisted text is invalid", field, "wrong_type"));
    }
    return StorageResult<std::string>::success(*text);
}

auto read_optional_text(jb::db::Record const& record, std::string_view field)
    -> jb::core::Result<std::optional<std::string>, jb::core::Error>
{
    auto value = required_value(record, field, "jobu.storage.invalid_text");
    if (!value) {
        return StorageResult<std::optional<std::string>>::failure(std::move(value).error());
    }
    if (std::holds_alternative<jb::db::Null>(**value)) {
        return StorageResult<std::optional<std::string>>::success(std::nullopt);
    }
    auto const* text = std::get_if<std::string>(value.value());
    if (text == nullptr) {
        return StorageResult<std::optional<std::string>>::failure(
            storage_error("jobu.storage.invalid_text", "Persisted text is invalid", field, "wrong_type"));
    }
    return StorageResult<std::optional<std::string>>::success(*text);
}

auto read_optional_blob(jb::db::Record const& record, std::string_view field)
    -> jb::core::Result<std::optional<jb::core::ByteBuffer>, jb::core::Error>
{
    auto value = required_value(record, field, "jobu.storage.invalid_blob");
    if (!value) {
        return StorageResult<std::optional<jb::core::ByteBuffer>>::failure(std::move(value).error());
    }
    if (std::holds_alternative<jb::db::Null>(**value)) {
        return StorageResult<std::optional<jb::core::ByteBuffer>>::success(std::nullopt);
    }
    auto const* bytes = std::get_if<jb::core::ByteBuffer>(value.value());
    if (bytes == nullptr) {
        return StorageResult<std::optional<jb::core::ByteBuffer>>::failure(
            storage_error("jobu.storage.invalid_blob", "Persisted binary value is invalid", field, "wrong_type"));
    }
    return StorageResult<std::optional<jb::core::ByteBuffer>>::success(*bytes);
}

auto json_to_storage(jb::core::JsonValue const& value, bool require_object, std::size_t max_size)
    -> jb::core::Result<jb::db::Value, jb::core::Error>
{
    if (require_object && !value.is_object()) {
        return StorageResult<jb::db::Value>::failure(json_error({}, "object_required"));
    }
    auto serialized = jb::core::serialize_json(value);
    if (!serialized) {
        return StorageResult<jb::db::Value>::failure(json_error({}, serialized.error().code));
    }
    if (serialized->size() > max_size) {
        return StorageResult<jb::db::Value>::failure(json_error({}, "too_large"));
    }
    return StorageResult<jb::db::Value>::success(jb::db::Value{std::move(serialized).value()});
}

auto read_json(jb::db::Record const& record, std::string_view field, bool require_object, std::size_t max_size)
    -> jb::core::Result<jb::core::JsonValue, jb::core::Error>
{
    auto value = required_value(record, field, "jobu.storage.invalid_json");
    if (!value) {
        return StorageResult<jb::core::JsonValue>::failure(std::move(value).error());
    }
    return decode_json(**value, field, require_object, max_size);
}

auto read_optional_json(jb::db::Record const& record, std::string_view field, bool require_object, std::size_t max_size)
    -> jb::core::Result<std::optional<jb::core::JsonValue>, jb::core::Error>
{
    auto value = required_value(record, field, "jobu.storage.invalid_json");
    if (!value) {
        return StorageResult<std::optional<jb::core::JsonValue>>::failure(std::move(value).error());
    }
    if (std::holds_alternative<jb::db::Null>(**value)) {
        return StorageResult<std::optional<jb::core::JsonValue>>::success(std::nullopt);
    }
    auto decoded = decode_json(**value, field, require_object, max_size);
    if (!decoded) {
        return StorageResult<std::optional<jb::core::JsonValue>>::failure(std::move(decoded).error());
    }
    return StorageResult<std::optional<jb::core::JsonValue>>::success(std::move(decoded).value());
}

auto job_run_columns() noexcept -> std::string_view
{
    return kJobRunColumns;
}

auto read_job_run(jb::db::Record const& record, AttributeRegistry const& attributes) -> StorageResult<JobRun>
{
    auto id = read_uuid(record, "run_id");
    if (!id) {
        return StorageResult<JobRun>::failure(std::move(id).error());
    }
    auto job_id = read_uuid(record, "run_job_id");
    if (!job_id) {
        return StorageResult<JobRun>::failure(std::move(job_id).error());
    }
    auto revision = read_revision(record, "run_job_revision");
    if (!revision) {
        return StorageResult<JobRun>::failure(std::move(revision).error());
    }
    auto queue_id = read_uuid(record, "run_queue_id");
    if (!queue_id) {
        return StorageResult<JobRun>::failure(std::move(queue_id).error());
    }
    auto origin = read_run_origin(record, "run_origin");
    if (!origin) {
        return StorageResult<JobRun>::failure(std::move(origin).error());
    }
    auto schedule_owned = read_boolean(record, "run_schedule_owned");
    if (!schedule_owned) {
        return StorageResult<JobRun>::failure(std::move(schedule_owned).error());
    }
    auto planned = read_timestamp(record, "run_planned_at_us");
    if (!planned) {
        return StorageResult<JobRun>::failure(std::move(planned).error());
    }
    auto runnable = read_timestamp(record, "run_runnable_at_us");
    if (!runnable) {
        return StorageResult<JobRun>::failure(std::move(runnable).error());
    }
    auto started = read_optional_timestamp(record, "run_started_at_us");
    if (!started) {
        return StorageResult<JobRun>::failure(std::move(started).error());
    }
    auto completed = read_optional_timestamp(record, "run_completed_at_us");
    if (!completed) {
        return StorageResult<JobRun>::failure(std::move(completed).error());
    }
    auto type = read_job_type(record, "run_type");
    if (!type) {
        return StorageResult<JobRun>::failure(std::move(type).error());
    }
    auto priority = read_int32(record, "run_priority");
    if (!priority) {
        return StorageResult<JobRun>::failure(std::move(priority).error());
    }
    auto attributes_json = read_json(record, "run_attributes_json", true, kMaximumJsonDocumentBytes);
    if (!attributes_json) {
        return StorageResult<JobRun>::failure(std::move(attributes_json).error());
    }
    auto decoded_attributes = decode_attribute_document(attributes,
                                                        *attributes_json,
                                                        AttributeScope::Job,
                                                        AttributeDocumentMode::Materialized);
    if (!decoded_attributes) {
        return StorageResult<JobRun>::failure(std::move(decoded_attributes).error());
    }
    auto payload = read_json(record, "run_payload_json", true, kMaximumJsonDocumentBytes);
    if (!payload) {
        return StorageResult<JobRun>::failure(std::move(payload).error());
    }
    auto state = read_run_state(record, "run_state");
    if (!state) {
        return StorageResult<JobRun>::failure(std::move(state).error());
    }
    auto result = read_optional_json(record, "run_result_json", true, kMaximumJsonDocumentBytes);
    if (!result) {
        return StorageResult<JobRun>::failure(std::move(result).error());
    }

    if (*schedule_owned && *origin != RunOrigin::Scheduled) {
        return StorageResult<JobRun>::failure(invalid_run("schedule_owned_origin"));
    }
    if (is_terminal(*state) != completed->has_value()) {
        return StorageResult<JobRun>::failure(invalid_run("completion_state_mismatch"));
    }
    if (!is_terminal(*state) && result->has_value()) {
        return StorageResult<JobRun>::failure(invalid_run("non_terminal_result"));
    }
    if (!valid_started_at(*state, started->has_value())) {
        return StorageResult<JobRun>::failure(invalid_run("started_state_mismatch"));
    }

    return StorageResult<JobRun>::success(JobRun{
        .id             = *id,
        .job_id         = *job_id,
        .job_revision   = *revision,
        .queue_id       = *queue_id,
        .origin         = *origin,
        .schedule_owned = *schedule_owned,
        .planned_at     = *planned,
        .runnable_at    = *runnable,
        .started_at     = *started,
        .completed_at   = *completed,
        .type           = *type,
        .priority       = *priority,
        .attributes     = std::move(decoded_attributes).value(),
        .payload        = std::move(payload).value(),
        .state          = *state,
        .result         = std::move(result).value(),
    });
}

auto storage_text(QueueState value) noexcept -> std::string_view
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

auto storage_text(RecoveryPolicy value) noexcept -> std::string_view
{
    switch (value) {
        case RecoveryPolicy::FailInterrupted:
            return "fail_interrupted";
        case RecoveryPolicy::RetryInterrupted:
            return "retry_interrupted";
    }
    return {};
}

auto storage_text(JobState value) noexcept -> std::string_view
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

auto storage_text(JobType value) noexcept -> std::string_view
{
    switch (value) {
        case JobType::Cli:
            return "cli";
        case JobType::Http:
            return "http";
    }
    return {};
}

auto storage_text(RunOrigin value) noexcept -> std::string_view
{
    switch (value) {
        case RunOrigin::Scheduled:
            return "scheduled";
        case RunOrigin::Manual:
            return "manual";
        case RunOrigin::Submitted:
            return "submitted";
    }
    return {};
}

auto storage_text(RunState value) noexcept -> std::string_view
{
    switch (value) {
        case RunState::Scheduled:
            return "scheduled";
        case RunState::Running:
            return "running";
        case RunState::RetryWait:
            return "retry_wait";
        case RunState::Succeeded:
            return "succeeded";
        case RunState::Failed:
            return "failed";
        case RunState::Interrupted:
            return "interrupted";
        case RunState::Cancelled:
            return "cancelled";
    }
    return {};
}

auto storage_text(AttemptState value) noexcept -> std::string_view
{
    switch (value) {
        case AttemptState::Pending:
            return "pending";
        case AttemptState::Running:
            return "running";
        case AttemptState::Completed:
            return "completed";
    }
    return {};
}

auto storage_text(AttemptOutcome value) noexcept -> std::string_view
{
    switch (value) {
        case AttemptOutcome::Succeeded:
            return "succeeded";
        case AttemptOutcome::Failed:
            return "failed";
        case AttemptOutcome::Interrupted:
            return "interrupted";
        case AttemptOutcome::Cancelled:
            return "cancelled";
    }
    return {};
}

auto read_queue_state(jb::db::Record const& record, std::string_view field)
    -> jb::core::Result<QueueState, jb::core::Error>
{
    constexpr auto values = std::array{
        std::pair{"active",     QueueState::Active    },
        std::pair{"suspending", QueueState::Suspending},
        std::pair{"suspended",  QueueState::Suspended },
        std::pair{"deleted",    QueueState::Deleted   },
    };
    return read_enum(record, field, values);
}

auto read_recovery_policy(jb::db::Record const& record, std::string_view field)
    -> jb::core::Result<RecoveryPolicy, jb::core::Error>
{
    constexpr auto values = std::array{
        std::pair{"fail_interrupted",  RecoveryPolicy::FailInterrupted },
        std::pair{"retry_interrupted", RecoveryPolicy::RetryInterrupted},
    };
    return read_enum(record, field, values);
}

auto read_job_state(jb::db::Record const& record, std::string_view field) -> jb::core::Result<JobState, jb::core::Error>
{
    constexpr auto values = std::array{
        std::pair{"active",     JobState::Active    },
        std::pair{"suspending", JobState::Suspending},
        std::pair{"suspended",  JobState::Suspended },
        std::pair{"deleted",    JobState::Deleted   },
    };
    return read_enum(record, field, values);
}

auto read_job_type(jb::db::Record const& record, std::string_view field) -> jb::core::Result<JobType, jb::core::Error>
{
    constexpr auto values = std::array{
        std::pair{"cli",  JobType::Cli },
        std::pair{"http", JobType::Http},
    };
    return read_enum(record, field, values);
}

auto read_run_origin(jb::db::Record const& record, std::string_view field)
    -> jb::core::Result<RunOrigin, jb::core::Error>
{
    constexpr auto values = std::array{
        std::pair{"scheduled", RunOrigin::Scheduled},
        std::pair{"manual",    RunOrigin::Manual   },
        std::pair{"submitted", RunOrigin::Submitted},
    };
    return read_enum(record, field, values);
}

auto read_run_state(jb::db::Record const& record, std::string_view field) -> jb::core::Result<RunState, jb::core::Error>
{
    constexpr auto values = std::array{
        std::pair{"scheduled",   RunState::Scheduled  },
        std::pair{"running",     RunState::Running    },
        std::pair{"retry_wait",  RunState::RetryWait  },
        std::pair{"succeeded",   RunState::Succeeded  },
        std::pair{"failed",      RunState::Failed     },
        std::pair{"interrupted", RunState::Interrupted},
        std::pair{"cancelled",   RunState::Cancelled  },
    };
    return read_enum(record, field, values);
}

auto read_attempt_state(jb::db::Record const& record, std::string_view field)
    -> jb::core::Result<AttemptState, jb::core::Error>
{
    constexpr auto values = std::array{
        std::pair{"pending",   AttemptState::Pending  },
        std::pair{"running",   AttemptState::Running  },
        std::pair{"completed", AttemptState::Completed},
    };
    return read_enum(record, field, values);
}

auto read_attempt_outcome(jb::db::Record const& record, std::string_view field)
    -> jb::core::Result<AttemptOutcome, jb::core::Error>
{
    constexpr auto values = std::array{
        std::pair{"succeeded",   AttemptOutcome::Succeeded  },
        std::pair{"failed",      AttemptOutcome::Failed     },
        std::pair{"interrupted", AttemptOutcome::Interrupted},
        std::pair{"cancelled",   AttemptOutcome::Cancelled  },
    };
    return read_enum(record, field, values);
}

} // namespace jb::jobu::detail
