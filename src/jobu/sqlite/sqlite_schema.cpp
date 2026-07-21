#include "sqlite_schema.hpp"

#include "query.hpp"
#include "sqlite_schema_priv.hpp"
#include "transaction.hpp"
#include "value.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace jb::jobu::sqlite {

namespace {

using VoidResult = jb::core::Result<void, jb::core::Error>;

constexpr std::array schema_objects{
    detail::SchemaObject{
                         .kind         = detail::SchemaObjectKind::Table,
                         .name         = "jobu_schema",
                         .owner        = "jobu_schema",
                         .ddl          = R"sql(CREATE TABLE jobu_schema (
    singleton INTEGER PRIMARY KEY CHECK (typeof(singleton) = 'integer' AND singleton = 1),
    version INTEGER NOT NULL CHECK (typeof(version) = 'integer' AND version > 0)
))sql",
                         .column_probe = "SELECT singleton, version FROM jobu_schema WHERE 0",
                         },
    detail::SchemaObject{
                         .kind  = detail::SchemaObjectKind::Table,
                         .name  = "jobu_queues",
                         .owner = "jobu_queues",
                         .ddl   = R"sql(CREATE TABLE jobu_queues (
    id BLOB PRIMARY KEY NOT NULL CHECK (typeof(id) = 'blob' AND length(id) = 16),
    name TEXT NOT NULL CHECK (typeof(name) = 'text'),
    deleted_name TEXT CHECK (deleted_name IS NULL OR typeof(deleted_name) = 'text'),
    state TEXT NOT NULL CHECK (typeof(state) = 'text' AND state IN ('active', 'suspending', 'suspended', 'deleted')),
    weight INTEGER NOT NULL CHECK (typeof(weight) = 'integer' AND weight > 0),
    concurrency_limit INTEGER NOT NULL CHECK (typeof(concurrency_limit) = 'integer' AND concurrency_limit > 0),
    recovery_policy TEXT NOT NULL CHECK (
        typeof(recovery_policy) = 'text' AND recovery_policy IN ('fail_interrupted', 'retry_interrupted')
    ),
    defaults_json TEXT NOT NULL CHECK (typeof(defaults_json) = 'text'),
    retention_seconds INTEGER CHECK (
        retention_seconds IS NULL OR (typeof(retention_seconds) = 'integer' AND retention_seconds >= 0)
    ),
    runnable_wait_warning_ms INTEGER NOT NULL CHECK (
        typeof(runnable_wait_warning_ms) = 'integer' AND runnable_wait_warning_ms >= 0
    ),
    created_at_us INTEGER NOT NULL CHECK (typeof(created_at_us) = 'integer'),
    updated_at_us INTEGER NOT NULL CHECK (typeof(updated_at_us) = 'integer'),
    deleted_at_us INTEGER CHECK (deleted_at_us IS NULL OR typeof(deleted_at_us) = 'integer'),
    CHECK (
        (state = 'deleted' AND deleted_name IS NOT NULL AND deleted_at_us IS NOT NULL)
        OR (state <> 'deleted' AND deleted_name IS NULL AND deleted_at_us IS NULL)
    )
))sql",
                         .column_probe =
            "SELECT id, name, deleted_name, state, weight, concurrency_limit, recovery_policy, defaults_json, "
            "retention_seconds, runnable_wait_warning_ms, created_at_us, updated_at_us, deleted_at_us "
            "FROM jobu_queues WHERE 0", },
    detail::SchemaObject{
                         .kind  = detail::SchemaObjectKind::Table,
                         .name  = "jobu_jobs",
                         .owner = "jobu_jobs",
                         .ddl   = R"sql(CREATE TABLE jobu_jobs (
    id BLOB PRIMARY KEY NOT NULL CHECK (typeof(id) = 'blob' AND length(id) = 16),
    queue_id BLOB NOT NULL CHECK (typeof(queue_id) = 'blob' AND length(queue_id) = 16),
    revision INTEGER NOT NULL CHECK (typeof(revision) = 'integer' AND revision > 0),
    name TEXT CHECK (name IS NULL OR typeof(name) = 'text'),
    state TEXT NOT NULL CHECK (typeof(state) = 'text' AND state IN ('active', 'suspending', 'suspended', 'deleted')),
    type TEXT NOT NULL CHECK (typeof(type) = 'text' AND type IN ('cli', 'http')),
    schedule_kind TEXT NOT NULL CHECK (typeof(schedule_kind) = 'text' AND schedule_kind IN ('once', 'cron')),
    scheduled_at_us INTEGER CHECK (scheduled_at_us IS NULL OR typeof(scheduled_at_us) = 'integer'),
    cron_expression TEXT CHECK (cron_expression IS NULL OR typeof(cron_expression) = 'text'),
    cron_timezone TEXT CHECK (cron_timezone IS NULL OR typeof(cron_timezone) = 'text'),
    priority INTEGER NOT NULL CHECK (
        typeof(priority) = 'integer' AND priority BETWEEN -2147483648 AND 2147483647
    ),
    attributes_json TEXT NOT NULL CHECK (typeof(attributes_json) = 'text'),
    payload_json TEXT NOT NULL CHECK (typeof(payload_json) = 'text'),
    created_at_us INTEGER NOT NULL CHECK (typeof(created_at_us) = 'integer'),
    updated_at_us INTEGER NOT NULL CHECK (typeof(updated_at_us) = 'integer'),
    deleted_at_us INTEGER CHECK (deleted_at_us IS NULL OR typeof(deleted_at_us) = 'integer'),
    FOREIGN KEY (queue_id) REFERENCES jobu_queues(id) ON DELETE RESTRICT,
    CHECK (
        (schedule_kind = 'once' AND scheduled_at_us IS NOT NULL AND cron_expression IS NULL AND cron_timezone IS NULL)
        OR
        (schedule_kind = 'cron' AND scheduled_at_us IS NULL AND cron_expression IS NOT NULL AND cron_timezone IS NOT NULL)
    ),
    CHECK (
        (state = 'deleted' AND deleted_at_us IS NOT NULL)
        OR (state <> 'deleted' AND deleted_at_us IS NULL)
    )
))sql",
                         .column_probe =
            "SELECT id, queue_id, revision, name, state, type, schedule_kind, scheduled_at_us, cron_expression, "
            "cron_timezone, priority, attributes_json, payload_json, created_at_us, updated_at_us, deleted_at_us "
            "FROM jobu_jobs WHERE 0", },
    detail::SchemaObject{
                         .kind  = detail::SchemaObjectKind::Table,
                         .name  = "jobu_runs",
                         .owner = "jobu_runs",
                         .ddl   = R"sql(CREATE TABLE jobu_runs (
    id BLOB PRIMARY KEY NOT NULL CHECK (typeof(id) = 'blob' AND length(id) = 16),
    job_id BLOB NOT NULL CHECK (typeof(job_id) = 'blob' AND length(job_id) = 16),
    job_revision INTEGER NOT NULL CHECK (typeof(job_revision) = 'integer' AND job_revision > 0),
    queue_id BLOB NOT NULL CHECK (typeof(queue_id) = 'blob' AND length(queue_id) = 16),
    origin TEXT NOT NULL CHECK (typeof(origin) = 'text' AND origin IN ('scheduled', 'manual', 'submitted')),
    schedule_owned INTEGER NOT NULL CHECK (typeof(schedule_owned) = 'integer' AND schedule_owned IN (0, 1)),
    planned_at_us INTEGER NOT NULL CHECK (typeof(planned_at_us) = 'integer'),
    runnable_at_us INTEGER NOT NULL CHECK (typeof(runnable_at_us) = 'integer'),
    started_at_us INTEGER CHECK (started_at_us IS NULL OR typeof(started_at_us) = 'integer'),
    completed_at_us INTEGER CHECK (completed_at_us IS NULL OR typeof(completed_at_us) = 'integer'),
    type TEXT NOT NULL CHECK (typeof(type) = 'text' AND type IN ('cli', 'http')),
    priority INTEGER NOT NULL CHECK (
        typeof(priority) = 'integer' AND priority BETWEEN -2147483648 AND 2147483647
    ),
    attributes_json TEXT NOT NULL CHECK (typeof(attributes_json) = 'text'),
    payload_json TEXT NOT NULL CHECK (typeof(payload_json) = 'text'),
    state TEXT NOT NULL CHECK (
        typeof(state) = 'text'
        AND state IN ('scheduled', 'running', 'retry_wait', 'succeeded', 'failed', 'interrupted', 'cancelled')
    ),
    result_json TEXT CHECK (result_json IS NULL OR typeof(result_json) = 'text'),
    FOREIGN KEY (job_id) REFERENCES jobu_jobs(id) ON DELETE RESTRICT,
    FOREIGN KEY (queue_id) REFERENCES jobu_queues(id) ON DELETE RESTRICT
))sql",
                         .column_probe =
            "SELECT id, job_id, job_revision, queue_id, origin, schedule_owned, planned_at_us, runnable_at_us, "
            "started_at_us, completed_at_us, type, priority, attributes_json, payload_json, state, result_json "
            "FROM jobu_runs WHERE 0", },
    detail::SchemaObject{
                         .kind  = detail::SchemaObjectKind::Table,
                         .name  = "jobu_attempts",
                         .owner = "jobu_attempts",
                         .ddl   = R"sql(CREATE TABLE jobu_attempts (
    run_id BLOB NOT NULL CHECK (typeof(run_id) = 'blob' AND length(run_id) = 16),
    attempt_number INTEGER NOT NULL CHECK (typeof(attempt_number) = 'integer' AND attempt_number > 0),
    due_at_us INTEGER NOT NULL CHECK (typeof(due_at_us) = 'integer'),
    started_at_us INTEGER CHECK (started_at_us IS NULL OR typeof(started_at_us) = 'integer'),
    completed_at_us INTEGER CHECK (completed_at_us IS NULL OR typeof(completed_at_us) = 'integer'),
    state TEXT NOT NULL CHECK (typeof(state) = 'text' AND state IN ('pending', 'running', 'completed')),
    outcome TEXT CHECK (
        outcome IS NULL
        OR (typeof(outcome) = 'text' AND outcome IN ('succeeded', 'failed', 'interrupted', 'cancelled'))
    ),
    result_json TEXT CHECK (result_json IS NULL OR typeof(result_json) = 'text'),
    PRIMARY KEY (run_id, attempt_number),
    FOREIGN KEY (run_id) REFERENCES jobu_runs(id) ON DELETE CASCADE
))sql",
                         .column_probe =
            "SELECT run_id, attempt_number, due_at_us, started_at_us, completed_at_us, state, outcome, result_json "
            "FROM jobu_attempts WHERE 0", },
    detail::SchemaObject{
                         .kind         = detail::SchemaObjectKind::Table,
                         .name         = "jobu_attempt_output",
                         .owner        = "jobu_attempt_output",
                         .ddl          = R"sql(CREATE TABLE jobu_attempt_output (
    run_id BLOB NOT NULL CHECK (typeof(run_id) = 'blob' AND length(run_id) = 16),
    attempt_number INTEGER NOT NULL CHECK (typeof(attempt_number) = 'integer' AND attempt_number > 0),
    stdout_blob BLOB CHECK (stdout_blob IS NULL OR typeof(stdout_blob) = 'blob'),
    stderr_blob BLOB CHECK (stderr_blob IS NULL OR typeof(stderr_blob) = 'blob'),
    stdout_truncated INTEGER NOT NULL CHECK (typeof(stdout_truncated) = 'integer' AND stdout_truncated IN (0, 1)),
    stderr_truncated INTEGER NOT NULL CHECK (typeof(stderr_truncated) = 'integer' AND stderr_truncated IN (0, 1)),
    capture_lost INTEGER NOT NULL CHECK (typeof(capture_lost) = 'integer' AND capture_lost IN (0, 1)),
    PRIMARY KEY (run_id, attempt_number),
    FOREIGN KEY (run_id, attempt_number)
        REFERENCES jobu_attempts(run_id, attempt_number) ON DELETE CASCADE
))sql",
                         .column_probe = "SELECT run_id, attempt_number, stdout_blob, stderr_blob, stdout_truncated, stderr_truncated, "
                        "capture_lost FROM jobu_attempt_output WHERE 0", },
    detail::SchemaObject{
                         .kind         = detail::SchemaObjectKind::Table,
                         .name         = "jobu_secrets",
                         .owner        = "jobu_secrets",
                         .ddl          = R"sql(CREATE TABLE jobu_secrets (
    name TEXT PRIMARY KEY NOT NULL CHECK (typeof(name) = 'text'),
    value_blob BLOB NOT NULL CHECK (typeof(value_blob) = 'blob'),
    created_at_us INTEGER NOT NULL CHECK (typeof(created_at_us) = 'integer'),
    updated_at_us INTEGER NOT NULL CHECK (typeof(updated_at_us) = 'integer')
))sql",
                         .column_probe = "SELECT name, value_blob, created_at_us, updated_at_us FROM jobu_secrets WHERE 0",
                         },
    detail::SchemaObject{
                         .kind         = detail::SchemaObjectKind::Table,
                         .name         = "jobu_secret_refs",
                         .owner        = "jobu_secret_refs",
                         .ddl          = R"sql(CREATE TABLE jobu_secret_refs (
    secret_name TEXT NOT NULL CHECK (typeof(secret_name) = 'text'),
    job_id BLOB NOT NULL CHECK (typeof(job_id) = 'blob' AND length(job_id) = 16),
    field_path TEXT NOT NULL CHECK (typeof(field_path) = 'text'),
    PRIMARY KEY (secret_name, job_id, field_path),
    FOREIGN KEY (secret_name) REFERENCES jobu_secrets(name) ON DELETE RESTRICT,
    FOREIGN KEY (job_id) REFERENCES jobu_jobs(id) ON DELETE CASCADE
))sql",
                         .column_probe = "SELECT secret_name, job_id, field_path FROM jobu_secret_refs WHERE 0",
                         },
    detail::SchemaObject{
                         .kind  = detail::SchemaObjectKind::Table,
                         .name  = "jobu_idempotency",
                         .owner = "jobu_idempotency",
                         .ddl   = R"sql(CREATE TABLE jobu_idempotency (
    method TEXT NOT NULL CHECK (typeof(method) = 'text'),
    scope_id BLOB NOT NULL CHECK (typeof(scope_id) = 'blob' AND length(scope_id) = 16),
    key TEXT NOT NULL CHECK (typeof(key) = 'text'),
    request_json TEXT NOT NULL CHECK (typeof(request_json) = 'text'),
    result_json TEXT NOT NULL CHECK (typeof(result_json) = 'text'),
    resource_id BLOB NOT NULL CHECK (typeof(resource_id) = 'blob' AND length(resource_id) = 16),
    created_at_us INTEGER NOT NULL CHECK (typeof(created_at_us) = 'integer'),
    expires_at_us INTEGER CHECK (expires_at_us IS NULL OR typeof(expires_at_us) = 'integer'),
    PRIMARY KEY (method, scope_id, key)
))sql",
                         .column_probe =
            "SELECT method, scope_id, key, request_json, result_json, resource_id, created_at_us, expires_at_us "
            "FROM jobu_idempotency WHERE 0", },
    detail::SchemaObject{
                         .kind         = detail::SchemaObjectKind::Index,
                         .name         = "jobu_queues_name_uidx",
                         .owner        = "jobu_queues",
                         .ddl          = "CREATE UNIQUE INDEX jobu_queues_name_uidx ON jobu_queues(name)",
                         .column_probe = {},
                         },
    detail::SchemaObject{
                         .kind         = detail::SchemaObjectKind::Index,
                         .name         = "jobu_jobs_queue_state_type_priority_id_idx",
                         .owner        = "jobu_jobs",
                         .ddl          = "CREATE INDEX jobu_jobs_queue_state_type_priority_id_idx "
                        "ON jobu_jobs(queue_id, state, type, priority DESC, id)", .column_probe = {},
                         },
    detail::SchemaObject{
                         .kind         = detail::SchemaObjectKind::Index,
                         .name         = "jobu_runs_queue_state_runnable_priority_planned_id_idx",
                         .owner        = "jobu_runs",
                         .ddl          = "CREATE INDEX jobu_runs_queue_state_runnable_priority_planned_id_idx "
                        "ON jobu_runs(queue_id, state, runnable_at_us, priority DESC, planned_at_us, id)",                                                             .column_probe = {},
                         },
    detail::SchemaObject{
                         .kind         = detail::SchemaObjectKind::Index,
                         .name         = "jobu_runs_job_state_idx",
                         .owner        = "jobu_runs",
                         .ddl          = "CREATE INDEX jobu_runs_job_state_idx ON jobu_runs(job_id, state)",
                         .column_probe = {},
                         },
    detail::SchemaObject{
                         .kind         = detail::SchemaObjectKind::Index,
                         .name         = "jobu_attempts_state_started_idx",
                         .owner        = "jobu_attempts",
                         .ddl          = "CREATE INDEX jobu_attempts_state_started_idx ON jobu_attempts(state, started_at_us)",
                         .column_probe = {},
                         },
    detail::SchemaObject{
                         .kind         = detail::SchemaObjectKind::Index,
                         .name         = "jobu_runs_terminal_completed_id_idx",
                         .owner        = "jobu_runs",
                         .ddl          = "CREATE INDEX jobu_runs_terminal_completed_id_idx ON jobu_runs(completed_at_us DESC, id)",
                         .column_probe = {},
                         },
    detail::SchemaObject{
                         .kind         = detail::SchemaObjectKind::Index,
                         .name         = "jobu_idempotency_resource_id_idx",
                         .owner        = "jobu_idempotency",
                         .ddl          = "CREATE INDEX jobu_idempotency_resource_id_idx ON jobu_idempotency(resource_id)",
                         .column_probe = {},
                         },
    detail::SchemaObject{
                         .kind         = detail::SchemaObjectKind::Index,
                         .name         = "jobu_secret_refs_job_id_idx",
                         .owner        = "jobu_secret_refs",
                         .ddl          = "CREATE INDEX jobu_secret_refs_job_id_idx ON jobu_secret_refs(job_id)",
                         .column_probe = {},
                         },
    detail::SchemaObject{
                         .kind         = detail::SchemaObjectKind::Index,
                         .name         = "jobu_runs_schedule_owned_non_terminal_uidx",
                         .owner        = "jobu_runs",
                         .ddl          = "CREATE UNIQUE INDEX jobu_runs_schedule_owned_non_terminal_uidx ON jobu_runs(job_id) "
                        "WHERE schedule_owned = 1 AND state IN ('scheduled', 'running', 'retry_wait')",                                                                     .column_probe = {},
                         },
};

enum class FailurePhase : std::uint8_t {
    Precondition,
    Creation,
    Validation,
};

enum class MarkerKind : std::uint8_t {
    Absent,
    Malformed,
    Older,
    Current,
    Newer,
};

struct MarkerStatus {
    MarkerKind   kind{MarkerKind::Malformed};
    std::int64_t version{0};
};

auto append_detail(std::string& detail, std::string_view key, std::string_view value) -> void
{
    if (value.empty()) {
        return;
    }
    if (!detail.empty()) {
        detail.push_back(' ');
    }
    detail += key;
    detail.push_back('=');
    detail += value;
}

auto schema_error(FailurePhase phase, std::string_view context = {}, jb::core::Error const* lower = nullptr)
    -> jb::core::Error
{
    auto error = jb::core::Error{};
    switch (phase) {
        case FailurePhase::Precondition:
            error.category = jb::core::ErrorCategory::InvalidArgument;
            error.code     = "jobu.schema.invalid_database";
            error.message  = "The database cannot be used for the JobU schema";
            break;
        case FailurePhase::Creation:
            error.category = jb::core::ErrorCategory::Internal;
            error.code     = "jobu.schema.create_failed";
            error.message  = "The JobU database schema could not be created";
            break;
        case FailurePhase::Validation:
            error.category = jb::core::ErrorCategory::Internal;
            error.code     = "jobu.schema.invalid";
            error.message  = "The JobU database schema is invalid";
            break;
    }
    append_detail(error.detail, "context", context);
    if (lower != nullptr) {
        append_detail(error.detail, "lower_code", lower->code);
    }
    return error;
}

template <typename T>
auto schema_failure(FailurePhase phase, std::string_view context = {}, jb::core::Error const* lower = nullptr)
    -> jb::core::Result<T, jb::core::Error>
{
    return jb::core::Result<T, jb::core::Error>::failure(schema_error(phase, context, lower));
}

auto special_error(jb::core::ErrorCategory category,
                   std::string_view        code,
                   std::string_view        message,
                   std::string_view        context = {}) -> jb::core::Error
{
    auto error = jb::core::Error{
        .category = category,
        .code     = std::string{code},
        .message  = std::string{message},
    };
    append_detail(error.detail, "context", context);
    return error;
}

auto expected_kind_text(detail::SchemaObjectKind kind) noexcept -> std::string_view
{
    return kind == detail::SchemaObjectKind::Table ? "table" : "index";
}

auto inspect_marker(jb::db::Database& database) -> jb::core::Result<MarkerStatus, jb::core::Error>
{
    {
        jb::db::Query query{database};
        auto          prepared =
            query.prepare("SELECT type, name, tbl_name FROM sqlite_schema "
                          "WHERE name = :name AND type IN ('table', 'index', 'trigger', 'view') ORDER BY type, name");
        if (!prepared) {
            return schema_failure<MarkerStatus>(FailurePhase::Validation, "jobu_schema marker", &prepared.error());
        }
        auto bound = query.bind_value(":name", jb::db::make_text("jobu_schema"));
        if (!bound) {
            return schema_failure<MarkerStatus>(FailurePhase::Validation, "jobu_schema marker", &bound.error());
        }
        auto executed = query.exec();
        if (!executed) {
            return schema_failure<MarkerStatus>(FailurePhase::Validation, "jobu_schema marker", &executed.error());
        }
        auto next = query.next();
        if (!next) {
            return schema_failure<MarkerStatus>(FailurePhase::Validation, "jobu_schema marker", &next.error());
        }
        if (!next.value()) {
            return jb::core::Result<MarkerStatus, jb::core::Error>::success({.kind = MarkerKind::Absent});
        }
        if (query.record().count() != 3) {
            return jb::core::Result<MarkerStatus, jb::core::Error>::success({.kind = MarkerKind::Malformed});
        }
        auto const* type      = std::get_if<std::string>(&query.value(0));
        auto const* name      = std::get_if<std::string>(&query.value(1));
        auto const* owner     = std::get_if<std::string>(&query.value(2));
        auto const  row_valid = type != nullptr && name != nullptr && owner != nullptr && *type == "table" &&
                              *name == "jobu_schema" && *owner == "jobu_schema";
        auto        extra     = query.next();
        if (!extra) {
            return schema_failure<MarkerStatus>(FailurePhase::Validation, "jobu_schema marker", &extra.error());
        }
        if (!row_valid || extra.value()) {
            return jb::core::Result<MarkerStatus, jb::core::Error>::success({.kind = MarkerKind::Malformed});
        }
    }

    jb::db::Query query{database};
    auto          executed = query.exec("SELECT singleton, version FROM jobu_schema ORDER BY singleton, version");
    if (!executed) {
        return schema_failure<MarkerStatus>(FailurePhase::Validation, "jobu_schema version", &executed.error());
    }

    auto         rows      = std::size_t{0};
    auto         malformed = false;
    std::int64_t singleton = 0;
    std::int64_t version   = 0;
    while (true) {
        auto next = query.next();
        if (!next) {
            return schema_failure<MarkerStatus>(FailurePhase::Validation, "jobu_schema version", &next.error());
        }
        if (!next.value()) {
            break;
        }
        ++rows;
        if (query.record().count() != 2) {
            malformed = true;
            continue;
        }
        auto const* row_singleton = std::get_if<std::int64_t>(&query.value(0));
        auto const* row_version   = std::get_if<std::int64_t>(&query.value(1));
        if (row_singleton == nullptr || row_version == nullptr) {
            malformed = true;
            continue;
        }
        if (rows == 1) {
            singleton = *row_singleton;
            version   = *row_version;
        }
    }

    if (malformed || rows != 1 || singleton != 1 || version <= 0) {
        return jb::core::Result<MarkerStatus, jb::core::Error>::success({.kind = MarkerKind::Malformed});
    }
    if (version < static_cast<std::int64_t>(current_schema_version)) {
        return jb::core::Result<MarkerStatus, jb::core::Error>::success(
            {.kind = MarkerKind::Older, .version = version});
    }
    if (version > static_cast<std::int64_t>(current_schema_version)) {
        return jb::core::Result<MarkerStatus, jb::core::Error>::success(
            {.kind = MarkerKind::Newer, .version = version});
    }
    return jb::core::Result<MarkerStatus, jb::core::Error>::success({.kind = MarkerKind::Current, .version = version});
}

auto find_unmarked_user_object(jb::db::Database& database)
    -> jb::core::Result<std::optional<std::string>, jb::core::Error>
{
    jb::db::Query query{database};
    auto executed = query.exec("SELECT type, name FROM sqlite_schema "
                               "WHERE type IN ('table', 'index', 'trigger', 'view') AND name NOT GLOB 'sqlite_*' "
                               "ORDER BY type, name");
    if (!executed) {
        return schema_failure<std::optional<std::string>>(FailurePhase::Validation,
                                                          "unmarked database",
                                                          &executed.error());
    }

    auto first_object = std::optional<std::string>{};
    while (true) {
        auto next = query.next();
        if (!next) {
            return schema_failure<std::optional<std::string>>(FailurePhase::Validation,
                                                              "unmarked database",
                                                              &next.error());
        }
        if (!next.value()) {
            break;
        }
        if (query.record().count() != 2) {
            return schema_failure<std::optional<std::string>>(FailurePhase::Validation, "unmarked database");
        }
        auto const* type = std::get_if<std::string>(&query.value(0));
        auto const* name = std::get_if<std::string>(&query.value(1));
        if (type == nullptr || name == nullptr) {
            return schema_failure<std::optional<std::string>>(FailurePhase::Validation, "unmarked database");
        }
        if (!first_object) {
            first_object = *name;
        }
    }
    return jb::core::Result<std::optional<std::string>, jb::core::Error>::success(std::move(first_object));
}

auto validate_object(jb::db::Database& database, detail::SchemaObject const& object, FailurePhase phase) -> VoidResult
{
    jb::db::Query query{database};
    auto          prepared =
        query.prepare("SELECT type, name, tbl_name FROM sqlite_schema "
                      "WHERE name = :name AND type IN ('table', 'index', 'trigger', 'view') ORDER BY type, name");
    if (!prepared) {
        return schema_failure<void>(phase, object.name, &prepared.error());
    }
    auto bound = query.bind_value(":name", jb::db::make_text(object.name));
    if (!bound) {
        return schema_failure<void>(phase, object.name, &bound.error());
    }
    auto executed = query.exec();
    if (!executed) {
        return schema_failure<void>(phase, object.name, &executed.error());
    }
    auto next = query.next();
    if (!next) {
        return schema_failure<void>(phase, object.name, &next.error());
    }
    if (!next.value() || query.record().count() != 3) {
        return schema_failure<void>(phase, object.name);
    }
    auto const* type  = std::get_if<std::string>(&query.value(0));
    auto const* name  = std::get_if<std::string>(&query.value(1));
    auto const* owner = std::get_if<std::string>(&query.value(2));
    if (type == nullptr || name == nullptr || owner == nullptr || *type != expected_kind_text(object.kind) ||
        *name != object.name || *owner != object.owner) {
        return schema_failure<void>(phase, object.name);
    }
    auto extra = query.next();
    if (!extra) {
        return schema_failure<void>(phase, object.name, &extra.error());
    }
    if (extra.value()) {
        return schema_failure<void>(phase, object.name);
    }
    return VoidResult::success();
}

auto validate_column_probe(jb::db::Database& database, detail::SchemaObject const& object, FailurePhase phase)
    -> VoidResult
{
    if (object.column_probe.empty()) {
        return VoidResult::success();
    }
    jb::db::Query query{database};
    auto          executed = query.exec(object.column_probe);
    if (!executed) {
        return schema_failure<void>(phase, object.name, &executed.error());
    }
    auto next = query.next();
    if (!next) {
        return schema_failure<void>(phase, object.name, &next.error());
    }
    if (next.value()) {
        return schema_failure<void>(phase, object.name);
    }
    return VoidResult::success();
}

auto validate_foreign_keys(jb::db::Database& database, FailurePhase phase) -> VoidResult
{
    {
        jb::db::Query query{database};
        auto          executed = query.exec("PRAGMA foreign_keys");
        if (!executed) {
            return schema_failure<void>(phase, "foreign_keys", &executed.error());
        }
        auto next = query.next();
        if (!next) {
            return schema_failure<void>(phase, "foreign_keys", &next.error());
        }
        if (!next.value() || query.record().count() != 1) {
            return schema_failure<void>(phase, "foreign_keys");
        }
        auto const* enabled = std::get_if<std::int64_t>(&query.value(0));
        if (enabled == nullptr || *enabled != 1) {
            return schema_failure<void>(phase, "foreign_keys");
        }
        auto extra = query.next();
        if (!extra) {
            return schema_failure<void>(phase, "foreign_keys", &extra.error());
        }
        if (extra.value()) {
            return schema_failure<void>(phase, "foreign_keys");
        }
    }

    jb::db::Query query{database};
    auto          executed = query.exec("PRAGMA foreign_key_check");
    if (!executed) {
        return schema_failure<void>(phase, "foreign_key_check", &executed.error());
    }
    auto violations  = std::size_t{0};
    auto first_table = std::string{};
    while (true) {
        auto next = query.next();
        if (!next) {
            return schema_failure<void>(phase, "foreign_key_check", &next.error());
        }
        if (!next.value()) {
            break;
        }
        if (query.record().count() != 4) {
            return schema_failure<void>(phase, "foreign_key_check");
        }
        auto const* table        = std::get_if<std::string>(&query.value(0));
        auto const* parent       = std::get_if<std::string>(&query.value(2));
        auto const* index        = std::get_if<std::int64_t>(&query.value(3));
        auto const  row_id_valid = std::holds_alternative<std::int64_t>(query.value(1)) ||
                                   std::holds_alternative<jb::db::Null>(query.value(1));
        if (table == nullptr || parent == nullptr || index == nullptr || !row_id_valid) {
            return schema_failure<void>(phase, "foreign_key_check");
        }
        ++violations;
        if (first_table.empty()) {
            first_table = *table;
        }
    }
    if (violations != 0) {
        return schema_failure<void>(phase, first_table);
    }
    return VoidResult::success();
}

auto validate_schema_v1(jb::db::Database& database, FailurePhase phase) -> VoidResult
{
    for (auto const& object : schema_objects) {
        auto valid_object = validate_object(database, object, phase);
        if (!valid_object) {
            return valid_object;
        }
        auto valid_columns = validate_column_probe(database, object, phase);
        if (!valid_columns) {
            return valid_columns;
        }
    }
    return validate_foreign_keys(database, phase);
}

auto create_schema_v1(jb::db::Database& database, detail::CreationStepObserver observer) -> VoidResult
{
    auto completed_statements = std::size_t{0};
    for (auto const& object : schema_objects) {
        {
            jb::db::Query query{database};
            auto          executed = query.exec(object.ddl);
            if (!executed) {
                return schema_failure<void>(FailurePhase::Creation, object.name, &executed.error());
            }
        }
        ++completed_statements;
        if (observer != nullptr) {
            auto observed = observer(completed_statements, object.name);
            if (!observed) {
                return schema_failure<void>(FailurePhase::Creation, object.name, &observed.error());
            }
        }
    }

    jb::db::Query query{database};
    auto          prepared = query.prepare("INSERT INTO jobu_schema(singleton, version) VALUES (:singleton, :version)");
    if (!prepared) {
        return schema_failure<void>(FailurePhase::Creation, "jobu_schema version", &prepared.error());
    }
    auto singleton_bound = query.bind_value(":singleton", std::int64_t{1});
    if (!singleton_bound) {
        return schema_failure<void>(FailurePhase::Creation, "jobu_schema version", &singleton_bound.error());
    }
    auto version_bound = query.bind_value(":version", static_cast<std::int64_t>(current_schema_version));
    if (!version_bound) {
        return schema_failure<void>(FailurePhase::Creation, "jobu_schema version", &version_bound.error());
    }
    auto executed = query.exec();
    if (!executed) {
        return schema_failure<void>(FailurePhase::Creation, "jobu_schema version", &executed.error());
    }
    return VoidResult::success();
}

} // anonymous namespace

namespace detail {

auto schema_object_manifest() noexcept -> std::span<SchemaObject const>
{
    return schema_objects;
}

auto ensure_schema_impl(jb::db::Database& database, CreationStepObserver observer)
    -> jb::core::Result<SchemaStatus, jb::core::Error>
{
    using SchemaResult = jb::core::Result<SchemaStatus, jb::core::Error>;

    if (!database.is_valid() || !database.is_open() || database.driver_name() != "sqlite") {
        return schema_failure<SchemaStatus>(FailurePhase::Precondition, "database");
    }

    auto begun = jb::db::Transaction::begin(database);
    if (!begun) {
        return schema_failure<SchemaStatus>(FailurePhase::Precondition, "transaction", &begun.error());
    }
    auto transaction = std::move(begun).value();

    auto marker = inspect_marker(database);
    if (!marker) {
        return SchemaResult::failure(std::move(marker).error());
    }

    auto created = false;
    if (marker->kind == MarkerKind::Absent) {
        auto user_object = find_unmarked_user_object(database);
        if (!user_object) {
            return SchemaResult::failure(std::move(user_object).error());
        }
        if (*user_object) {
            return SchemaResult::failure(special_error(jb::core::ErrorCategory::Conflict,
                                                       "jobu.schema.database_not_empty",
                                                       "The unmarked database already contains schema objects",
                                                       **user_object));
        }
        auto created_schema = create_schema_v1(database, observer);
        if (!created_schema) {
            return SchemaResult::failure(std::move(created_schema).error());
        }
        auto validated = validate_schema_v1(database, FailurePhase::Creation);
        if (!validated) {
            return SchemaResult::failure(std::move(validated).error());
        }
        created = true;
    }
    else if (marker->kind == MarkerKind::Newer) {
        return SchemaResult::failure(special_error(jb::core::ErrorCategory::Unsupported,
                                                   "jobu.schema.newer_database",
                                                   "The database schema is newer than this JobU version",
                                                   "jobu_schema"));
    }
    else if (marker->kind == MarkerKind::Malformed || marker->kind == MarkerKind::Older) {
        return schema_failure<SchemaStatus>(FailurePhase::Validation, "jobu_schema version");
    }
    else {
        auto validated = validate_schema_v1(database, FailurePhase::Validation);
        if (!validated) {
            return SchemaResult::failure(std::move(validated).error());
        }
    }

    auto committed = transaction.commit();
    if (!committed) {
        return schema_failure<SchemaStatus>(created ? FailurePhase::Creation : FailurePhase::Validation,
                                            "transaction commit",
                                            &committed.error());
    }
    return SchemaResult::success({.version = current_schema_version, .created = created});
}

} // namespace detail

auto ensure_schema(jb::db::Database& database) -> jb::core::Result<SchemaStatus, jb::core::Error>
{
    return detail::ensure_schema_impl(database, nullptr);
}

} // namespace jb::jobu::sqlite
