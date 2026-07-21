#pragma once

#include "attempt.hpp"
#include "job.hpp"
#include "queue.hpp"
#include "record.hpp"
#include "result.hpp"
#include "run.hpp"
#include "value.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace jb::jobu::detail {

[[nodiscard]] auto uuid_to_storage(jb::core::Uuid const& value) -> jb::db::Value;
[[nodiscard]] auto read_uuid(jb::db::Record const& record, std::string_view field)
    -> jb::core::Result<jb::core::Uuid, jb::core::Error>;

[[nodiscard]] auto timestamp_to_storage(jb::core::UtcTimePoint value)
    -> jb::core::Result<jb::db::Value, jb::core::Error>;
[[nodiscard]] auto read_timestamp(jb::db::Record const& record, std::string_view field)
    -> jb::core::Result<jb::core::UtcTimePoint, jb::core::Error>;
[[nodiscard]] auto read_optional_timestamp(jb::db::Record const& record, std::string_view field)
    -> jb::core::Result<std::optional<jb::core::UtcTimePoint>, jb::core::Error>;

[[nodiscard]] auto boolean_to_storage(bool value) -> jb::db::Value;
[[nodiscard]] auto read_boolean(jb::db::Record const& record, std::string_view field)
    -> jb::core::Result<bool, jb::core::Error>;

[[nodiscard]] auto revision_to_storage(JobRevision value) -> jb::core::Result<jb::db::Value, jb::core::Error>;
[[nodiscard]] auto read_revision(jb::db::Record const& record, std::string_view field)
    -> jb::core::Result<JobRevision, jb::core::Error>;

[[nodiscard]] auto attempt_number_to_storage(AttemptNumber value) -> jb::core::Result<jb::db::Value, jb::core::Error>;
[[nodiscard]] auto read_attempt_number(jb::db::Record const& record, std::string_view field)
    -> jb::core::Result<AttemptNumber, jb::core::Error>;

[[nodiscard]] auto int32_to_storage(std::int32_t value) -> jb::db::Value;
[[nodiscard]] auto read_int32(jb::db::Record const& record, std::string_view field)
    -> jb::core::Result<std::int32_t, jb::core::Error>;

[[nodiscard]] auto read_text(jb::db::Record const& record, std::string_view field)
    -> jb::core::Result<std::string, jb::core::Error>;
[[nodiscard]] auto read_optional_text(jb::db::Record const& record, std::string_view field)
    -> jb::core::Result<std::optional<std::string>, jb::core::Error>;

[[nodiscard]] auto json_to_storage(jb::rpc::JsonValue const& value, bool require_object, std::size_t max_size)
    -> jb::core::Result<jb::db::Value, jb::core::Error>;
[[nodiscard]] auto
read_json(jb::db::Record const& record, std::string_view field, bool require_object, std::size_t max_size)
    -> jb::core::Result<jb::rpc::JsonValue, jb::core::Error>;
[[nodiscard]] auto
read_optional_json(jb::db::Record const& record, std::string_view field, bool require_object, std::size_t max_size)
    -> jb::core::Result<std::optional<jb::rpc::JsonValue>, jb::core::Error>;

[[nodiscard]] auto storage_text(QueueState value) noexcept -> std::string_view;
[[nodiscard]] auto storage_text(RecoveryPolicy value) noexcept -> std::string_view;
[[nodiscard]] auto storage_text(JobState value) noexcept -> std::string_view;
[[nodiscard]] auto storage_text(JobType value) noexcept -> std::string_view;
[[nodiscard]] auto storage_text(RunOrigin value) noexcept -> std::string_view;
[[nodiscard]] auto storage_text(RunState value) noexcept -> std::string_view;
[[nodiscard]] auto storage_text(AttemptState value) noexcept -> std::string_view;
[[nodiscard]] auto storage_text(AttemptOutcome value) noexcept -> std::string_view;

[[nodiscard]] auto read_queue_state(jb::db::Record const& record, std::string_view field)
    -> jb::core::Result<QueueState, jb::core::Error>;
[[nodiscard]] auto read_recovery_policy(jb::db::Record const& record, std::string_view field)
    -> jb::core::Result<RecoveryPolicy, jb::core::Error>;
[[nodiscard]] auto read_job_state(jb::db::Record const& record, std::string_view field)
    -> jb::core::Result<JobState, jb::core::Error>;
[[nodiscard]] auto read_job_type(jb::db::Record const& record, std::string_view field)
    -> jb::core::Result<JobType, jb::core::Error>;
[[nodiscard]] auto read_run_origin(jb::db::Record const& record, std::string_view field)
    -> jb::core::Result<RunOrigin, jb::core::Error>;
[[nodiscard]] auto read_run_state(jb::db::Record const& record, std::string_view field)
    -> jb::core::Result<RunState, jb::core::Error>;
[[nodiscard]] auto read_attempt_state(jb::db::Record const& record, std::string_view field)
    -> jb::core::Result<AttemptState, jb::core::Error>;
[[nodiscard]] auto read_attempt_outcome(jb::db::Record const& record, std::string_view field)
    -> jb::core::Result<AttemptOutcome, jb::core::Error>;

} // namespace jb::jobu::detail
