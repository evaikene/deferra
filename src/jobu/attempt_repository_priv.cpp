#include "attempt_repository_priv.hpp"

#include "domain_storage_priv.hpp"
#include "query.hpp"
#include "value.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace jb::jobu::detail {

namespace {

template <typename T>
using RepositoryResult = jb::core::Result<T, jb::core::Error>;

constexpr std::size_t kMaximumJsonDocumentBytes = std::size_t{256} * 1024U;
constexpr std::size_t kMaximumAttemptList       = 1000U;

constexpr auto kAttemptSelection =
    "SELECT run_id AS attempt_run_id, attempt_number AS attempt_number, due_at_us AS attempt_due_at_us, "
    "started_at_us AS attempt_started_at_us, completed_at_us AS attempt_completed_at_us, "
    "state AS attempt_state, outcome AS attempt_outcome, result_json AS attempt_result_json FROM jobu_attempts ";

constexpr auto kOutputSelection =
    "SELECT run_id AS output_run_id, attempt_number AS output_attempt_number, stdout_blob AS output_stdout_blob, "
    "stderr_blob AS output_stderr_blob, stdout_truncated AS output_stdout_truncated, "
    "stderr_truncated AS output_stderr_truncated, capture_lost AS output_capture_lost FROM jobu_attempt_output ";

auto repository_error(jb::core::ErrorCategory category, std::string_view code, std::string_view message)
    -> jb::core::Error
{
    return {
        .category = category,
        .code     = std::string{code},
        .message  = std::string{message},
    };
}

auto invalid_attempt(std::string_view reason) -> jb::core::Error
{
    auto error   = repository_error(jb::core::ErrorCategory::Internal,
                                    "jobu.storage.invariant",
                                    "Persisted attempt data violates a JobU invariant");
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

auto optional_timestamp_to_storage(std::optional<jb::core::UtcTimePoint> value) -> RepositoryResult<jb::db::Value>
{
    if (!value) {
        return RepositoryResult<jb::db::Value>::success(jb::db::Null{});
    }
    return timestamp_to_storage(*value);
}

auto optional_outcome_to_storage(std::optional<AttemptOutcome> value) -> jb::db::Value
{
    if (!value) {
        return jb::db::Null{};
    }
    return jb::db::make_text(storage_text(*value));
}

auto optional_json_to_storage(std::optional<jb::rpc::JsonValue> const& value) -> RepositoryResult<jb::db::Value>
{
    if (!value) {
        return RepositoryResult<jb::db::Value>::success(jb::db::Null{});
    }
    return json_to_storage(*value, true, kMaximumJsonDocumentBytes);
}

auto optional_blob_to_storage(std::optional<jb::core::ByteBuffer> const& value) -> jb::db::Value
{
    if (!value) {
        return jb::db::Null{};
    }
    return jb::db::make_blob(*value);
}

auto read_optional_outcome(jb::db::Record const& record, std::string_view field)
    -> RepositoryResult<std::optional<AttemptOutcome>>
{
    auto const* value = record.value(field);
    if (value == nullptr) {
        return RepositoryResult<std::optional<AttemptOutcome>>::failure(invalid_attempt("missing_outcome"));
    }
    if (std::holds_alternative<jb::db::Null>(*value)) {
        return RepositoryResult<std::optional<AttemptOutcome>>::success(std::nullopt);
    }
    auto outcome = read_attempt_outcome(record, field);
    if (!outcome) {
        return RepositoryResult<std::optional<AttemptOutcome>>::failure(std::move(outcome).error());
    }
    return RepositoryResult<std::optional<AttemptOutcome>>::success(*outcome);
}

auto valid_attempt_state(JobAttempt const& attempt) noexcept -> bool
{
    if (attempt.state == AttemptState::Pending) {
        return !attempt.started_at && !attempt.completed_at && !attempt.outcome && !attempt.result;
    }
    if (attempt.state == AttemptState::Running) {
        return attempt.started_at.has_value() && !attempt.completed_at && !attempt.outcome && !attempt.result;
    }
    if (attempt.state == AttemptState::Completed) {
        return attempt.completed_at.has_value() && attempt.outcome.has_value() &&
               (attempt.started_at.has_value() || *attempt.outcome == AttemptOutcome::Cancelled);
    }
    return false;
}

auto decode_attempt(jb::db::Record const& record) -> RepositoryResult<JobAttempt>
{
    auto run_id = read_uuid(record, "attempt_run_id");
    if (!run_id) {
        return RepositoryResult<JobAttempt>::failure(std::move(run_id).error());
    }
    auto number = read_attempt_number(record, "attempt_number");
    if (!number) {
        return RepositoryResult<JobAttempt>::failure(std::move(number).error());
    }
    auto due = read_timestamp(record, "attempt_due_at_us");
    if (!due) {
        return RepositoryResult<JobAttempt>::failure(std::move(due).error());
    }
    auto started = read_optional_timestamp(record, "attempt_started_at_us");
    if (!started) {
        return RepositoryResult<JobAttempt>::failure(std::move(started).error());
    }
    auto completed = read_optional_timestamp(record, "attempt_completed_at_us");
    if (!completed) {
        return RepositoryResult<JobAttempt>::failure(std::move(completed).error());
    }
    auto state = read_attempt_state(record, "attempt_state");
    if (!state) {
        return RepositoryResult<JobAttempt>::failure(std::move(state).error());
    }
    auto outcome = read_optional_outcome(record, "attempt_outcome");
    if (!outcome) {
        return RepositoryResult<JobAttempt>::failure(std::move(outcome).error());
    }
    auto result = read_optional_json(record, "attempt_result_json", true, kMaximumJsonDocumentBytes);
    if (!result) {
        return RepositoryResult<JobAttempt>::failure(std::move(result).error());
    }

    auto attempt = JobAttempt{
        .run_id         = *run_id,
        .attempt_number = *number,
        .due_at         = *due,
        .started_at     = *started,
        .completed_at   = *completed,
        .state          = *state,
        .outcome        = *outcome,
        .result         = std::move(result).value(),
    };
    if (!valid_attempt_state(attempt)) {
        return RepositoryResult<JobAttempt>::failure(invalid_attempt("state_fields_mismatch"));
    }
    return RepositoryResult<JobAttempt>::success(std::move(attempt));
}

auto decode_output(jb::db::Record const& record) -> RepositoryResult<AttemptOutput>
{
    auto run_id = read_uuid(record, "output_run_id");
    if (!run_id) {
        return RepositoryResult<AttemptOutput>::failure(std::move(run_id).error());
    }
    auto number = read_attempt_number(record, "output_attempt_number");
    if (!number) {
        return RepositoryResult<AttemptOutput>::failure(std::move(number).error());
    }
    auto stdout_bytes = read_optional_blob(record, "output_stdout_blob");
    if (!stdout_bytes) {
        return RepositoryResult<AttemptOutput>::failure(std::move(stdout_bytes).error());
    }
    auto stderr_bytes = read_optional_blob(record, "output_stderr_blob");
    if (!stderr_bytes) {
        return RepositoryResult<AttemptOutput>::failure(std::move(stderr_bytes).error());
    }
    auto stdout_truncated = read_boolean(record, "output_stdout_truncated");
    if (!stdout_truncated) {
        return RepositoryResult<AttemptOutput>::failure(std::move(stdout_truncated).error());
    }
    auto stderr_truncated = read_boolean(record, "output_stderr_truncated");
    if (!stderr_truncated) {
        return RepositoryResult<AttemptOutput>::failure(std::move(stderr_truncated).error());
    }
    auto capture_lost = read_boolean(record, "output_capture_lost");
    if (!capture_lost) {
        return RepositoryResult<AttemptOutput>::failure(std::move(capture_lost).error());
    }
    return RepositoryResult<AttemptOutput>::success(AttemptOutput{
        .stdout_bytes     = std::move(stdout_bytes).value(),
        .stderr_bytes     = std::move(stderr_bytes).value(),
        .stdout_truncated = *stdout_truncated,
        .stderr_truncated = *stderr_truncated,
        .capture_lost     = *capture_lost,
    });
}

auto bind_output(jb::db::Query&        query,
                 jb::core::Uuid const& run_id,
                 jb::db::Value         attempt_number,
                 AttemptOutput const&  output) -> RepositoryResult<void>
{
    return bind_all(query,
                    {
                        {":run_id",           uuid_to_storage(run_id)                      },
                        {":attempt_number",   std::move(attempt_number)                    },
                        {":stdout_blob",      optional_blob_to_storage(output.stdout_bytes)},
                        {":stderr_blob",      optional_blob_to_storage(output.stderr_bytes)},
                        {":stdout_truncated", boolean_to_storage(output.stdout_truncated)  },
                        {":stderr_truncated", boolean_to_storage(output.stderr_truncated)  },
                        {":capture_lost",     boolean_to_storage(output.capture_lost)      },
    });
}

auto has_row(jb::db::Database& database, std::string_view sql, jb::core::Uuid const& id) -> RepositoryResult<bool>
{
    jb::db::Query query{database};
    auto          prepared = query.prepare(sql);
    if (!prepared) {
        return RepositoryResult<bool>::failure(std::move(prepared).error());
    }
    auto bound = query.bind_value(":id", uuid_to_storage(id));
    if (!bound) {
        return RepositoryResult<bool>::failure(std::move(bound).error());
    }
    auto executed = query.exec();
    if (!executed) {
        return RepositoryResult<bool>::failure(std::move(executed).error());
    }
    return query.next();
}

auto output_exists(jb::db::Database& database, jb::core::Uuid const& run_id, jb::db::Value attempt_number)
    -> RepositoryResult<bool>
{
    jb::db::Query query{database};
    auto          prepared = query.prepare("SELECT 1 FROM jobu_attempt_output WHERE run_id = :run_id "
                                           "AND attempt_number = :attempt_number LIMIT 1");
    if (!prepared) {
        return RepositoryResult<bool>::failure(std::move(prepared).error());
    }
    auto bound = bind_all(query,
                          {
                              {":run_id",         uuid_to_storage(run_id)  },
                              {":attempt_number", std::move(attempt_number)},
    });
    if (!bound) {
        return RepositoryResult<bool>::failure(std::move(bound).error());
    }
    auto executed = query.exec();
    if (!executed) {
        return RepositoryResult<bool>::failure(std::move(executed).error());
    }
    return query.next();
}

} // anonymous namespace

AttemptRepository::AttemptRepository(jb::db::Database& database) noexcept
    : _database{database}
{}

auto AttemptRepository::insert_attempt(JobAttempt const& attempt) -> jb::core::Result<void, jb::core::Error>
{
    if (!valid_attempt_state(attempt)) {
        return RepositoryResult<void>::failure(invalid_attempt("insert_state_fields_mismatch"));
    }
    auto number = attempt_number_to_storage(attempt.attempt_number);
    if (!number) {
        return RepositoryResult<void>::failure(std::move(number).error());
    }
    auto due = timestamp_to_storage(attempt.due_at);
    if (!due) {
        return RepositoryResult<void>::failure(std::move(due).error());
    }
    auto started = optional_timestamp_to_storage(attempt.started_at);
    if (!started) {
        return RepositoryResult<void>::failure(std::move(started).error());
    }
    auto completed = optional_timestamp_to_storage(attempt.completed_at);
    if (!completed) {
        return RepositoryResult<void>::failure(std::move(completed).error());
    }
    auto result = optional_json_to_storage(attempt.result);
    if (!result) {
        return RepositoryResult<void>::failure(std::move(result).error());
    }

    jb::db::Query query{_database};
    auto          prepared = query.prepare(
        "INSERT INTO jobu_attempts(run_id, attempt_number, due_at_us, started_at_us, completed_at_us, state, "
        "outcome, result_json) VALUES(:run_id, :attempt_number, :due_at_us, :started_at_us, :completed_at_us, "
        ":state, :outcome, :result_json)");
    if (!prepared) {
        return RepositoryResult<void>::failure(std::move(prepared).error());
    }
    auto bound = bind_all(query,
                          {
                              {":run_id",          uuid_to_storage(attempt.run_id)               },
                              {":attempt_number",  std::move(number).value()                     },
                              {":due_at_us",       std::move(due).value()                        },
                              {":started_at_us",   std::move(started).value()                    },
                              {":completed_at_us", std::move(completed).value()                  },
                              {":state",           jb::db::make_text(storage_text(attempt.state))},
                              {":outcome",         optional_outcome_to_storage(attempt.outcome)  },
                              {":result_json",     std::move(result).value()                     },
    });
    if (!bound) {
        return bound;
    }
    return query.exec();
}

auto AttemptRepository::find(jb::core::Uuid const& run_id, AttemptNumber attempt_number)
    -> jb::core::Result<std::optional<JobAttempt>, jb::core::Error>
{
    auto number = attempt_number_to_storage(attempt_number);
    if (!number) {
        return RepositoryResult<std::optional<JobAttempt>>::failure(std::move(number).error());
    }
    jb::db::Query query{_database};
    auto          prepared =
        query.prepare(std::string{kAttemptSelection} + "WHERE run_id = :run_id AND attempt_number = :attempt_number");
    if (!prepared) {
        return RepositoryResult<std::optional<JobAttempt>>::failure(std::move(prepared).error());
    }
    auto bound = bind_all(query,
                          {
                              {":run_id",         uuid_to_storage(run_id)  },
                              {":attempt_number", std::move(number).value()},
    });
    if (!bound) {
        return RepositoryResult<std::optional<JobAttempt>>::failure(std::move(bound).error());
    }
    auto executed = query.exec();
    if (!executed) {
        return RepositoryResult<std::optional<JobAttempt>>::failure(std::move(executed).error());
    }
    auto next = query.next();
    if (!next) {
        return RepositoryResult<std::optional<JobAttempt>>::failure(std::move(next).error());
    }
    if (!*next) {
        return RepositoryResult<std::optional<JobAttempt>>::success(std::nullopt);
    }
    auto decoded = decode_attempt(query.record());
    if (!decoded) {
        return RepositoryResult<std::optional<JobAttempt>>::failure(std::move(decoded).error());
    }
    return RepositoryResult<std::optional<JobAttempt>>::success(std::move(decoded).value());
}

auto AttemptRepository::list_for_run(jb::core::Uuid const& run_id, std::size_t limit)
    -> jb::core::Result<std::vector<JobAttempt>, jb::core::Error>
{
    if (limit == 0 || limit > kMaximumAttemptList || !std::in_range<std::int64_t>(limit)) {
        return RepositoryResult<std::vector<JobAttempt>>::failure(invalid_limit());
    }
    jb::db::Query query{_database};
    auto          prepared = query.prepare(std::string{kAttemptSelection} +
                                           "WHERE run_id = :run_id ORDER BY attempt_number ASC LIMIT :limit");
    if (!prepared) {
        return RepositoryResult<std::vector<JobAttempt>>::failure(std::move(prepared).error());
    }
    auto bound = bind_all(query,
                          {
                              {":run_id", uuid_to_storage(run_id)         },
                              {":limit",  static_cast<std::int64_t>(limit)},
    });
    if (!bound) {
        return RepositoryResult<std::vector<JobAttempt>>::failure(std::move(bound).error());
    }
    auto executed = query.exec();
    if (!executed) {
        return RepositoryResult<std::vector<JobAttempt>>::failure(std::move(executed).error());
    }
    auto attempts = std::vector<JobAttempt>{};
    attempts.reserve(limit);
    while (true) {
        auto next = query.next();
        if (!next) {
            return RepositoryResult<std::vector<JobAttempt>>::failure(std::move(next).error());
        }
        if (!*next) {
            break;
        }
        auto decoded = decode_attempt(query.record());
        if (!decoded) {
            return RepositoryResult<std::vector<JobAttempt>>::failure(std::move(decoded).error());
        }
        attempts.push_back(std::move(decoded).value());
    }
    return RepositoryResult<std::vector<JobAttempt>>::success(std::move(attempts));
}

auto AttemptRepository::insert_or_replace_output(jb::core::Uuid const& run_id,
                                                 AttemptNumber         attempt_number,
                                                 AttemptOutput const& output) -> jb::core::Result<void, jb::core::Error>
{
    auto number = attempt_number_to_storage(attempt_number);
    if (!number) {
        return RepositoryResult<void>::failure(std::move(number).error());
    }
    auto existing = output_exists(_database, run_id, *number);
    if (!existing) {
        return RepositoryResult<void>::failure(std::move(existing).error());
    }

    jb::db::Query query{_database};
    auto          prepared =
        *existing
            ? query.prepare("UPDATE jobu_attempt_output SET stdout_blob = :stdout_blob, stderr_blob = :stderr_blob, "
                            "stdout_truncated = :stdout_truncated, stderr_truncated = :stderr_truncated, "
                            "capture_lost = :capture_lost WHERE run_id = :run_id "
                            "AND attempt_number = :attempt_number")
            : query.prepare("INSERT INTO jobu_attempt_output(run_id, attempt_number, stdout_blob, stderr_blob, "
                            "stdout_truncated, stderr_truncated, capture_lost) VALUES(:run_id, :attempt_number, "
                            ":stdout_blob, :stderr_blob, :stdout_truncated, :stderr_truncated, :capture_lost)");
    if (!prepared) {
        return RepositoryResult<void>::failure(std::move(prepared).error());
    }
    auto bound = bind_output(query, run_id, std::move(number).value(), output);
    if (!bound) {
        return bound;
    }
    return query.exec();
}

auto AttemptRepository::find_output(jb::core::Uuid const& run_id, AttemptNumber attempt_number)
    -> jb::core::Result<std::optional<AttemptOutput>, jb::core::Error>
{
    auto number = attempt_number_to_storage(attempt_number);
    if (!number) {
        return RepositoryResult<std::optional<AttemptOutput>>::failure(std::move(number).error());
    }
    jb::db::Query query{_database};
    auto          prepared =
        query.prepare(std::string{kOutputSelection} + "WHERE run_id = :run_id AND attempt_number = :attempt_number");
    if (!prepared) {
        return RepositoryResult<std::optional<AttemptOutput>>::failure(std::move(prepared).error());
    }
    auto bound = bind_all(query,
                          {
                              {":run_id",         uuid_to_storage(run_id)  },
                              {":attempt_number", std::move(number).value()},
    });
    if (!bound) {
        return RepositoryResult<std::optional<AttemptOutput>>::failure(std::move(bound).error());
    }
    auto executed = query.exec();
    if (!executed) {
        return RepositoryResult<std::optional<AttemptOutput>>::failure(std::move(executed).error());
    }
    auto next = query.next();
    if (!next) {
        return RepositoryResult<std::optional<AttemptOutput>>::failure(std::move(next).error());
    }
    if (!*next) {
        return RepositoryResult<std::optional<AttemptOutput>>::success(std::nullopt);
    }
    auto decoded = decode_output(query.record());
    if (!decoded) {
        return RepositoryResult<std::optional<AttemptOutput>>::failure(std::move(decoded).error());
    }
    return RepositoryResult<std::optional<AttemptOutput>>::success(std::move(decoded).value());
}

auto AttemptRepository::has_any_for_run(jb::core::Uuid const& run_id) -> jb::core::Result<bool, jb::core::Error>
{
    return has_row(_database, "SELECT 1 FROM jobu_attempts WHERE run_id = :id LIMIT 1", run_id);
}

auto AttemptRepository::has_started_for_job(jb::core::Uuid const& job_id) -> jb::core::Result<bool, jb::core::Error>
{
    return has_row(_database,
                   "SELECT 1 FROM jobu_attempts AS attempts JOIN jobu_runs AS runs ON runs.id = attempts.run_id "
                   "WHERE runs.job_id = :id AND attempts.started_at_us IS NOT NULL LIMIT 1",
                   job_id);
}

} // namespace jb::jobu::detail
