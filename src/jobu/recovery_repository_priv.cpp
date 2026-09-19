#include "recovery_repository_priv.hpp"

#include "attempt_repository_priv.hpp"
#include "domain_storage_priv.hpp"
#include "job_repository_priv.hpp"
#include "json.hpp"
#include "query.hpp"
#include "queue_repository_priv.hpp"
#include "run_repository_priv.hpp"
#include "scheduler_repository_priv.hpp"
#include "storage_failure_priv.hpp"
#include "value.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace jb::jobu::detail {
namespace {

template <typename T>
using RecoveryResult = jb::core::Result<T, jb::core::Error>;

auto invariant(std::string_view reason) -> jb::core::Error
{
    return {.category = jb::core::ErrorCategory::Internal,
            .code     = "jobu.recovery.invariant",
            .message  = "Persisted recovery data violates a JobU invariant",
            .detail   = "reason=" + std::string{reason}};
}

auto storage_error(jb::core::Error const& error) -> jb::core::Error
{
    return sanitized_storage_error(error, StorageOperation::Recovery);
}

auto interruption_result() -> RecoveryResult<std::string>
{
    // Interruption records an unknown external outcome, never a fabricated runner failure.
    return jb::core::serialize_json({
        .data = jb::core::JsonValue::Object{
                                            {"reason", {.data = std::string{"daemon_interrupted"}}},
                                            {"outcome_unknown", {.data = true}},
                                            }
    });
}

auto expect_one_affected(jb::db::Query const& query, std::string_view reason) -> RecoveryResult<void>
{
    if (query.num_rows_affected() != 1) {
        return RecoveryResult<void>::failure(invariant(reason));
    }
    return RecoveryResult<void>::success();
}

auto invalid_limit() -> jb::core::Error
{
    return {.category = jb::core::ErrorCategory::InvalidArgument,
            .code     = "jobu.storage.invalid_limit",
            .message  = "Recovery scan limit must be in 1..4096"};
}

auto valid_limit(std::size_t limit) noexcept -> bool
{
    return limit >= 1 && limit <= 4096;
}

auto nonterminal(RunState state) noexcept -> bool
{
    return state == RunState::Scheduled || state == RunState::Running || state == RunState::RetryWait;
}

// Repository lookups mix driver failures and durable decoding failures. Translate only the
// latter to recovery invariants; driver codes remain available to the shared storage policy.
template <typename T>
auto lookup_error(RecoveryResult<T> const& result) -> jb::core::Error
{
    if (result.error().code.starts_with("db.")) {
        return storage_error(result.error());
    }
    return invariant("durable_decode");
}

auto read_count(jb::db::Record const& row, std::string_view field) -> RecoveryResult<std::uint64_t>
{
    auto const* value = row.value(field);
    auto const* count = value == nullptr ? nullptr : std::get_if<std::int64_t>(value);
    if (count == nullptr || *count < 0) {
        return RecoveryResult<std::uint64_t>::failure(invariant("history_count"));
    }
    return RecoveryResult<std::uint64_t>::success(static_cast<std::uint64_t>(*count));
}

struct History {
    std::uint64_t count{};
    std::uint64_t latest{};
    std::uint64_t running{};
    std::uint64_t unfinished{};
    std::uint64_t invalid_prior{};
};

auto history(jb::db::Database& database, jb::core::Uuid const& id) -> RecoveryResult<History>
{
    // Aggregate only metadata: retry histories may be arbitrarily long. Interrupted is a
    // valid prior outcome after recovery, without changing normal scheduler retry eligibility.
    jb::db::Query query{database};
    auto          result = query.prepare(
        "SELECT COUNT(*) AS total, COALESCE(MAX(attempt_number), 0) AS latest, "
        "COUNT(CASE WHEN state = 'running' THEN 1 END) AS running, "
        "COUNT(CASE WHEN state <> 'completed' THEN 1 END) AS unfinished, "
        "COUNT(CASE WHEN attempt_number < (SELECT MAX(attempt_number) FROM jobu_attempts WHERE run_id = :id) "
        "AND (state <> 'completed' OR outcome IS NULL OR outcome NOT IN ('failed', 'interrupted')) "
        "THEN 1 END) AS invalid_prior FROM jobu_attempts WHERE run_id = :id");
    if (result) {
        result = query.bind_value(":id", uuid_to_storage(id));
    }
    if (result) {
        result = query.exec();
    }
    if (!result) {
        return RecoveryResult<History>::failure(storage_error(result.error()));
    }
    auto next = query.next();
    if (!next) {
        return RecoveryResult<History>::failure(storage_error(next.error()));
    }
    if (!*next) {
        return RecoveryResult<History>::failure(invariant("missing_history_summary"));
    }
    auto total         = read_count(query.record(), "total");
    auto latest        = read_count(query.record(), "latest");
    auto running       = read_count(query.record(), "running");
    auto unfinished    = read_count(query.record(), "unfinished");
    auto invalid_prior = read_count(query.record(), "invalid_prior");
    if (!total || !latest || !running || !unfinished || !invalid_prior) {
        return RecoveryResult<History>::failure(invariant("history_count"));
    }
    return RecoveryResult<History>::success({.count         = *total,
                                             .latest        = *latest,
                                             .running       = *running,
                                             .unfinished    = *unfinished,
                                             .invalid_prior = *invalid_prior});
}

auto validate_barriers(jb::db::Database& database, jb::core::Uuid const& job_id) -> RecoveryResult<void>
{
    jb::db::Query query{database};
    auto          result =
        query.prepare("SELECT COUNT(CASE WHEN schedule_owned = 1 THEN 1 END) AS scheduled, "
                      "COUNT(CASE WHEN origin = 'manual' THEN 1 END) AS manual "
                      "FROM jobu_runs WHERE job_id = :id AND state IN ('scheduled', 'running', 'retry_wait')");
    if (result) {
        result = query.bind_value(":id", uuid_to_storage(job_id));
    }
    if (result) {
        result = query.exec();
    }
    if (!result) {
        return RecoveryResult<void>::failure(storage_error(result.error()));
    }
    auto next = query.next();
    if (!next) {
        return RecoveryResult<void>::failure(storage_error(next.error()));
    }
    if (!*next) {
        return RecoveryResult<void>::failure(invariant("missing_barrier_summary"));
    }
    auto scheduled = read_count(query.record(), "scheduled");
    auto manual    = read_count(query.record(), "manual");
    if (!scheduled || !manual || *scheduled > 1 || *manual > 1 || (*manual == 1 && *scheduled != 1)) {
        return RecoveryResult<void>::failure(invariant("manual_barrier_relationship"));
    }
    return RecoveryResult<void>::success();
}

// Only fixed internal table names reach this helper. The composite cursor prevents a page
// boundary within one run's history from skipping remaining attempts. No output bytes are read.
auto scan_keys(jb::db::Database&                 database,
               std::string_view                  table,
               std::size_t                       limit,
               std::optional<RecoveryAttemptKey> after) -> RecoveryResult<std::vector<RecoveryAttemptKey>>
{
    if (!valid_limit(limit)) {
        return RecoveryResult<std::vector<RecoveryAttemptKey>>::failure(invalid_limit());
    }
    auto sql = "SELECT run_id, attempt_number FROM " + std::string{table};
    if (after) {
        sql += " WHERE run_id > :id OR (run_id = :id AND attempt_number > :number)";
    }
    sql += " ORDER BY run_id ASC, attempt_number ASC LIMIT :limit";
    jb::db::Query query{database};
    auto          result = query.prepare(sql);
    if (result && after) {
        auto number = attempt_number_to_storage(after->attempt_number);
        if (!number) {
            return RecoveryResult<std::vector<RecoveryAttemptKey>>::failure(std::move(number).error());
        }
        result = query.bind_value(":id", uuid_to_storage(after->run_id));
        if (result) {
            result = query.bind_value(":number", *number);
        }
    }
    if (result) {
        result = query.bind_value(":limit", static_cast<std::int64_t>(limit));
    }
    if (result) {
        result = query.exec();
    }
    if (!result) {
        return RecoveryResult<std::vector<RecoveryAttemptKey>>::failure(storage_error(result.error()));
    }
    auto keys = std::vector<RecoveryAttemptKey>{};
    keys.reserve(limit);
    while (true) {
        auto next = query.next();
        if (!next) {
            return RecoveryResult<std::vector<RecoveryAttemptKey>>::failure(storage_error(next.error()));
        }
        if (!*next) {
            break;
        }
        auto id     = read_uuid(query.record(), "run_id");
        auto number = read_attempt_number(query.record(), "attempt_number");
        if (!id || !number) {
            return RecoveryResult<std::vector<RecoveryAttemptKey>>::failure(invariant("attempt_key"));
        }
        keys.push_back({.run_id = *id, .attempt_number = *number});
    }
    return RecoveryResult<std::vector<RecoveryAttemptKey>>::success(std::move(keys));
}

auto scan_ids(jb::db::Database&             database,
              std::string_view              table,
              std::size_t                   limit,
              std::optional<jb::core::Uuid> after_id,
              std::optional<RunState>       state = {}) -> RecoveryResult<std::vector<jb::core::Uuid>>
{
    if (!valid_limit(limit)) {
        return RecoveryResult<std::vector<jb::core::Uuid>>::failure(invalid_limit());
    }
    auto ids = std::vector<jb::core::Uuid>{};
    {
        auto sql = std::string{"SELECT id FROM " + std::string{table} + " WHERE 1 = 1"};
        if (after_id) {
            sql += " AND id > :id";
        }
        if (state) {
            sql += " AND state = :state";
        }
        sql += " ORDER BY id ASC LIMIT :limit";
        jb::db::Query query{database};
        auto          result = query.prepare(sql);
        if (result && after_id) {
            result = query.bind_value(":id", uuid_to_storage(*after_id));
        }
        if (result && state) {
            result = query.bind_value(":state", jb::db::make_text(storage_text(*state)));
        }
        if (result) {
            result = query.bind_value(":limit", static_cast<std::int64_t>(limit));
        }
        if (result) {
            result = query.exec();
        }
        if (!result) {
            return RecoveryResult<std::vector<jb::core::Uuid>>::failure(storage_error(result.error()));
        }
        ids.reserve(limit);
        while (true) {
            auto next = query.next();
            if (!next) {
                return RecoveryResult<std::vector<jb::core::Uuid>>::failure(storage_error(next.error()));
            }
            if (!*next) {
                break;
            }
            auto id = read_uuid(query.record(), "id");
            if (!id) {
                return RecoveryResult<std::vector<jb::core::Uuid>>::failure(invariant("entity_key"));
            }
            ids.push_back(*id);
        }
    }
    return RecoveryResult<std::vector<jb::core::Uuid>>::success(std::move(ids));
}

auto validate_output(jb::db::Database& database, RecoveryAttemptKey const& key) -> RecoveryResult<void>
{
    jb::db::Query query{database};
    auto result = query.prepare("SELECT stdout_truncated, stderr_truncated, capture_lost FROM jobu_attempt_output "
                                "WHERE run_id = :id AND attempt_number = :number");
    if (result) {
        result = query.bind_value(":id", uuid_to_storage(key.run_id));
    }
    if (result) {
        result = query.bind_value(":number", static_cast<std::int64_t>(key.attempt_number));
    }
    if (result) {
        result = query.exec();
    }
    if (!result) {
        return RecoveryResult<void>::failure(storage_error(result.error()));
    }
    auto next = query.next();
    if (!next) {
        return RecoveryResult<void>::failure(storage_error(next.error()));
    }
    if (*next) {
        for (auto const* field : {"stdout_truncated", "stderr_truncated", "capture_lost"}) {
            if (!read_boolean(query.record(), field)) {
                return RecoveryResult<void>::failure(invariant("output_metadata"));
            }
        }
        // The metadata query must be gone before a repository lookup opens another query.
        auto finished = query.finish();
        if (!finished) {
            return RecoveryResult<void>::failure(storage_error(finished.error()));
        }
        AttemptRepository attempts{database};
        auto              attempt = attempts.find(key.run_id, key.attempt_number);
        if (!attempt) {
            return RecoveryResult<void>::failure(lookup_error(attempt));
        }
        if (!*attempt || (*attempt)->state != AttemptState::Completed) {
            return RecoveryResult<void>::failure(invariant("output_without_completed_attempt"));
        }
    }
    return RecoveryResult<void>::success();
}

} // namespace

RecoveryRepository::RecoveryRepository(jb::db::Database& database, AttributeRegistry const& attributes) noexcept
    : _database{database}
    , _attributes{attributes}
{}

auto RecoveryRepository::interrupt_attempt(RecoveryAttemptKey const& key, jb::core::UtcTimePoint recovery_time)
    -> RecoveryResult<void>
{
    // A scan is only a candidate source. Re-read ownership, history and output inside the
    // caller's transaction before the first write, then verify the selected attempt is still current.
    auto run = find_run(key.run_id);
    if (!run) {
        return RecoveryResult<void>::failure(std::move(run).error());
    }
    if (run->state != RunState::Running) {
        return RecoveryResult<void>::failure(invariant("interruption_run_not_running"));
    }
    AttemptRepository attempts{_database};
    auto              attempt = attempts.find(key.run_id, key.attempt_number);
    if (!attempt) {
        return RecoveryResult<void>::failure(lookup_error(attempt));
    }
    // find_run established that exactly the latest attempt is Running. An earlier key
    // must not rewrite completed history, even if it came from a previously valid scan.
    if (!*attempt || (*attempt)->state != AttemptState::Running) {
        return RecoveryResult<void>::failure(invariant("interruption_attempt_not_current"));
    }
    auto serialized = interruption_result();
    if (!serialized) {
        return RecoveryResult<void>::failure(storage_error(serialized.error()));
    }

    SchedulerRepository scheduler{_database, _attributes};
    auto                completed = scheduler.complete_attempt(key.run_id,
                                                               key.attempt_number,
                                                               recovery_time,
                                                               AttemptOutcome::Interrupted,
                                                               *serialized);
    if (!completed) {
        return RecoveryResult<void>::failure(storage_error(completed.error()));
    }

    // Use INSERT, not the normal output replacement API: unexpected capture is evidence
    // of an invalid durable state. Empty blobs describe lost capture, not observed silence.
    auto number = attempt_number_to_storage(key.attempt_number);
    if (!number) {
        return RecoveryResult<void>::failure(storage_error(number.error()));
    }
    jb::db::Query query{_database};
    auto          result = query.prepare(
        "INSERT INTO jobu_attempt_output(run_id, attempt_number, stdout_blob, stderr_blob, "
        "stdout_truncated, stderr_truncated, capture_lost) VALUES(:id, :number, :empty, :empty, 0, 0, 1)");
    if (result) {
        result = query.bind_value(":id", uuid_to_storage(key.run_id));
    }
    if (result) {
        result = query.bind_value(":number", *number);
    }
    if (result) {
        result = query.bind_value(":empty", jb::db::make_blob({}));
    }
    if (result) {
        result = query.exec();
    }
    if (!result) {
        return RecoveryResult<void>::failure(storage_error(result.error()));
    }
    return expect_one_affected(query, "interruption_output_affected_rows");
}

auto RecoveryRepository::set_run_interrupted(RecoveryAttemptKey const& key, jb::core::UtcTimePoint recovery_time)
    -> RecoveryResult<void>
{
    auto number = attempt_number_to_storage(key.attempt_number);
    if (!number) {
        return RecoveryResult<void>::failure(storage_error(number.error()));
    }
    auto completed = timestamp_to_storage(recovery_time);
    if (!completed) {
        return RecoveryResult<void>::failure(storage_error(completed.error()));
    }
    auto serialized = interruption_result();
    if (!serialized) {
        return RecoveryResult<void>::failure(storage_error(serialized.error()));
    }

    // The attempt/output writes deliberately precede this update. Guard their matching
    // metadata so a stale key, wrong timestamp, missing capture or repeated call cannot
    // terminalize a different transition. Only these three run fields may change.
    jb::db::Query query{_database};
    auto          result =
        query.prepare("UPDATE jobu_runs SET state = 'interrupted', completed_at_us = :completed, result_json = :result "
                      "WHERE id = :id AND state = 'running' AND started_at_us IS NOT NULL "
                      "AND completed_at_us IS NULL AND result_json IS NULL "
                      "AND :number = (SELECT MAX(attempt_number) FROM jobu_attempts WHERE run_id = :id) "
                      "AND NOT EXISTS (SELECT 1 FROM jobu_attempts WHERE run_id = :id AND state <> 'completed') "
                      "AND EXISTS (SELECT 1 FROM jobu_attempts WHERE run_id = :id AND attempt_number = :number "
                      "AND state = 'completed' AND outcome = 'interrupted' AND completed_at_us = :completed "
                      "AND result_json = :result AND due_at_us = jobu_runs.runnable_at_us) "
                      "AND EXISTS (SELECT 1 FROM jobu_attempt_output WHERE run_id = :id AND attempt_number = :number "
                      "AND stdout_blob = :empty AND stderr_blob = :empty "
                      "AND stdout_truncated = 0 AND stderr_truncated = 0 AND capture_lost = 1)");
    if (result) {
        result = query.bind_value(":id", uuid_to_storage(key.run_id));
    }
    if (result) {
        result = query.bind_value(":number", *number);
    }
    if (result) {
        result = query.bind_value(":completed", *completed);
    }
    if (result) {
        result = query.bind_value(":result", jb::db::make_text(*serialized));
    }
    if (result) {
        result = query.bind_value(":empty", jb::db::make_blob({}));
    }
    if (result) {
        result = query.exec();
    }
    if (!result) {
        return RecoveryResult<void>::failure(storage_error(result.error()));
    }
    return expect_one_affected(query, "interruption_run_affected_rows");
}

auto RecoveryRepository::find_run(jb::core::Uuid const& id) -> RecoveryResult<JobRun>
{
    RunRepository runs{_database, _attributes};
    auto          found = runs.find_by_id(id);
    if (!found) {
        return RecoveryResult<JobRun>::failure(lookup_error(found));
    }
    if (!*found) {
        return RecoveryResult<JobRun>::failure(invariant("missing_run"));
    }
    auto            run = std::move(**found);
    JobRepository   jobs{_database, _attributes};
    QueueRepository queues{_database, _attributes};
    auto            job   = jobs.find_by_id(run.job_id, true);
    auto            queue = queues.find_by_id(run.queue_id, true);
    if (!job || !queue) {
        return RecoveryResult<JobRun>::failure(!job ? lookup_error(job) : lookup_error(queue));
    }
    if (!*job || !*queue || run.origin == RunOrigin::Submitted ||
        run.schedule_owned != (run.origin == RunOrigin::Scheduled)) {
        return RecoveryResult<JobRun>::failure(invariant("run_ownership"));
    }

    // A moved job's terminal history keeps its former queue. Only live work must match
    // current ownership. Suspended jobs may still run manual work in an active queue.
    if (nonterminal(run.state)) {
        if (run.queue_id != (*job)->queue_id || (*job)->state == JobState::Deleted ||
            (*queue)->state == QueueState::Deleted ||
            (run.state == RunState::Running && ((*queue)->state == QueueState::Suspended ||
                                                (run.schedule_owned && (*job)->state == JobState::Suspended)))) {
            return RecoveryResult<JobRun>::failure(invariant("nonterminal_ownership"));
        }
        auto barriers = validate_barriers(_database, run.job_id);
        if (!barriers) {
            return RecoveryResult<JobRun>::failure(std::move(barriers).error());
        }
    }

    auto summary = history(_database, id);
    if (!summary) {
        return RecoveryResult<JobRun>::failure(std::move(summary).error());
    }
    // Positive composite keys are unique, so count == maximum proves numbering starts at
    // one without gaps. Exactly the latest attempt may remain unfinished on a Running run.
    auto expected_running = run.state == RunState::Running ? 1U : 0U;
    if (summary->count != summary->latest || summary->running != expected_running ||
        summary->unfinished != expected_running || summary->invalid_prior != 0 ||
        (run.state == RunState::Scheduled && summary->count != 0)) {
        return RecoveryResult<JobRun>::failure(invariant("attempt_history"));
    }
    if (summary->count == 0) {
        if (run.state != RunState::Scheduled && (run.state != RunState::Cancelled || run.started_at)) {
            return RecoveryResult<JobRun>::failure(invariant("missing_attempt_history"));
        }
        return RecoveryResult<JobRun>::success(std::move(run));
    }

    AttemptRepository attempts{_database};
    auto              first  = attempts.find(id, 1);
    auto              latest = attempts.find(id, summary->latest);
    if (!first || !latest) {
        return RecoveryResult<JobRun>::failure(!first ? lookup_error(first) : lookup_error(latest));
    }
    if (!*first || !*latest || !(*first)->started_at || run.started_at != (*first)->started_at) {
        return RecoveryResult<JobRun>::failure(invariant("first_attempt_start"));
    }
    auto const& last = **latest;
    if (run.state == RunState::Running) {
        if (last.state != AttemptState::Running || last.due_at != run.runnable_at) {
            return RecoveryResult<JobRun>::failure(invariant("running_attempt"));
        }
    }
    else {
        auto retry_outcome = last.outcome == AttemptOutcome::Failed || last.outcome == AttemptOutcome::Interrupted;
        auto expected      = AttemptOutcome::Cancelled;
        switch (run.state) {
            case RunState::Succeeded:
                expected = AttemptOutcome::Succeeded;
                break;
            case RunState::Failed:
                expected = AttemptOutcome::Failed;
                break;
            case RunState::Interrupted:
                expected = AttemptOutcome::Interrupted;
                break;
            default:
                break;
        }
        if (last.state != AttemptState::Completed || (run.state == RunState::RetryWait && !retry_outcome) ||
            (!nonterminal(run.state) && last.outcome != expected &&
             (run.state != RunState::Cancelled || !retry_outcome))) {
            return RecoveryResult<JobRun>::failure(invariant("latest_attempt_outcome"));
        }
        // Cancellation during RetryWait adds no attempt. Otherwise terminal completion shares
        // the last attempt's timestamp and due time. Wall-clock ordering itself is not required.
        if (!nonterminal(run.state) && last.outcome == expected &&
            (last.completed_at != run.completed_at || last.due_at != run.runnable_at)) {
            return RecoveryResult<JobRun>::failure(invariant("terminal_attempt_timestamps"));
        }
    }
    auto output = validate_output(_database, {.run_id = id, .attempt_number = summary->latest});
    if (!output) {
        return RecoveryResult<JobRun>::failure(std::move(output).error());
    }
    return RecoveryResult<JobRun>::success(std::move(run));
}

auto RecoveryRepository::list_runs(std::size_t                   limit,
                                   std::optional<jb::core::Uuid> after_id,
                                   std::optional<RunState>       state) -> RecoveryResult<std::vector<JobRun>>
{
    if (!valid_limit(limit)) {
        return RecoveryResult<std::vector<JobRun>>::failure(invalid_limit());
    }
    // Materialize only page keys, then release the scan before decoding related rows. An inner
    // join here would hide orphan runs instead of diagnosing them in find_run().
    auto ids = scan_ids(_database, "jobu_runs", limit, after_id, state);
    if (!ids) {
        return RecoveryResult<std::vector<JobRun>>::failure(std::move(ids).error());
    }
    auto rows = std::vector<JobRun>{};
    rows.reserve(ids->size());
    for (auto const& id : *ids) {
        auto run = find_run(id);
        if (!run) {
            return RecoveryResult<std::vector<JobRun>>::failure(std::move(run).error());
        }
        rows.push_back(std::move(*run));
    }
    return RecoveryResult<std::vector<JobRun>>::success(std::move(rows));
}

auto RecoveryRepository::list_attempts(std::size_t limit, std::optional<RecoveryAttemptKey> after)
    -> RecoveryResult<std::vector<JobAttempt>>
{
    auto keys = scan_keys(_database, "jobu_attempts", limit, after);
    if (!keys) {
        return RecoveryResult<std::vector<JobAttempt>>::failure(std::move(keys).error());
    }
    auto rows = std::vector<JobAttempt>{};
    rows.reserve(keys->size());
    AttemptRepository attempts{_database};
    RunRepository     runs{_database, _attributes};
    for (auto const& key : *keys) {
        auto attempt = attempts.find(key.run_id, key.attempt_number);
        auto run     = runs.find_by_id(key.run_id);
        if (!attempt || !run) {
            return RecoveryResult<std::vector<JobAttempt>>::failure(!attempt ? lookup_error(attempt)
                                                                             : lookup_error(run));
        }
        if (!*attempt || !*run || (*attempt)->state == AttemptState::Pending || !(*attempt)->started_at ||
            ((*attempt)->state == AttemptState::Running && (*run)->state != RunState::Running)) {
            return RecoveryResult<std::vector<JobAttempt>>::failure(invariant("attempt_relationship"));
        }
        auto output = validate_output(_database, key);
        if (!output) {
            return RecoveryResult<std::vector<JobAttempt>>::failure(std::move(output).error());
        }
        rows.push_back(std::move(**attempt));
    }
    return RecoveryResult<std::vector<JobAttempt>>::success(std::move(rows));
}

auto RecoveryRepository::list_outputs(std::size_t limit, std::optional<RecoveryAttemptKey> after)
    -> RecoveryResult<std::vector<RecoveryAttemptKey>>
{
    auto keys = scan_keys(_database, "jobu_attempt_output", limit, after);
    if (!keys) {
        return keys;
    }
    for (auto const& key : *keys) {
        auto valid = validate_output(_database, key);
        if (!valid) {
            return RecoveryResult<std::vector<RecoveryAttemptKey>>::failure(std::move(valid).error());
        }
    }
    return keys;
}

auto RecoveryRepository::list_jobs(std::size_t limit, std::optional<jb::core::Uuid> after_id)
    -> RecoveryResult<std::vector<JobDefinition>>
{
    // Include definitions without runs: missing recurring work and drained suspensions are
    // repair candidates, while a missing or deleted current owner is an invariant failure.
    auto ids = scan_ids(_database, "jobu_jobs", limit, after_id);
    if (!ids) {
        return RecoveryResult<std::vector<JobDefinition>>::failure(std::move(ids).error());
    }
    JobRepository   jobs{_database, _attributes};
    QueueRepository queues{_database, _attributes};
    auto            rows = std::vector<JobDefinition>{};
    rows.reserve(ids->size());
    for (auto const& id : *ids) {
        auto job = jobs.find_by_id(id, true);
        if (!job) {
            return RecoveryResult<std::vector<JobDefinition>>::failure(lookup_error(job));
        }
        if (!*job) {
            return RecoveryResult<std::vector<JobDefinition>>::failure(invariant("missing_job"));
        }
        auto queue = queues.find_by_id((*job)->queue_id, true);
        if (!queue) {
            return RecoveryResult<std::vector<JobDefinition>>::failure(lookup_error(queue));
        }
        if (!*queue || ((*queue)->state == QueueState::Deleted && (*job)->state != JobState::Deleted)) {
            return RecoveryResult<std::vector<JobDefinition>>::failure(invariant("job_ownership"));
        }
        auto barriers = validate_barriers(_database, id);
        if (!barriers) {
            return RecoveryResult<std::vector<JobDefinition>>::failure(std::move(barriers).error());
        }
        rows.push_back(std::move(**job));
    }
    return RecoveryResult<std::vector<JobDefinition>>::success(std::move(rows));
}

auto RecoveryRepository::list_queues(std::size_t limit, std::optional<jb::core::Uuid> after_id)
    -> RecoveryResult<std::vector<Queue>>
{
    auto ids = scan_ids(_database, "jobu_queues", limit, after_id);
    if (!ids) {
        return RecoveryResult<std::vector<Queue>>::failure(std::move(ids).error());
    }
    QueueRepository queues{_database, _attributes};
    auto            rows = std::vector<Queue>{};
    rows.reserve(ids->size());
    for (auto const& id : *ids) {
        auto queue = queues.find_by_id(id, true);
        if (!queue) {
            return RecoveryResult<std::vector<Queue>>::failure(lookup_error(queue));
        }
        if (!*queue) {
            return RecoveryResult<std::vector<Queue>>::failure(invariant("missing_queue"));
        }
        rows.push_back(std::move(**queue));
    }
    return RecoveryResult<std::vector<Queue>>::success(std::move(rows));
}

} // namespace jb::jobu::detail
