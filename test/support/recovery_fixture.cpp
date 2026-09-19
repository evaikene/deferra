#include "recovery_fixture.hpp"

#include "attribute_codec_priv.hpp"
#include "domain_storage_priv.hpp"
#include "job_repository_priv.hpp"
#include "job_validation_priv.hpp"
#include "query.hpp"
#include "queue_repository_priv.hpp"
#include "run_repository_priv.hpp"
#include "sqlite/sqlite_driver.hpp"
#include "sqlite/sqlite_schema.hpp"
#include "transaction.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>

namespace jb::test {

using namespace std::chrono_literals;
using namespace jobu;
using namespace jobu::detail;

namespace {

auto at(std::int64_t seconds) -> core::UtcTimePoint
{
    return core::UtcTimePoint{std::chrono::seconds{seconds}};
}

auto result_document() -> core::JsonValue
{
    return {.data = core::JsonValue::Object{{"reason", {.data = std::string{"fixture"}}}}};
}

auto timestamp(std::optional<core::UtcTimePoint> value) -> db::Value
{
    if (!value) {
        return db::Null{};
    }
    auto encoded = timestamp_to_storage(*value);
    REQUIRE(encoded);
    return std::move(*encoded);
}

auto json(std::optional<core::JsonValue> const& value) -> db::Value
{
    if (!value) {
        return db::Null{};
    }
    auto encoded = json_to_storage(*value, true, maximum_job_document_bytes);
    REQUIRE(encoded);
    return std::move(*encoded);
}

} // namespace

auto recovery_id(std::uint32_t suffix) -> core::Uuid
{
    auto bytes = core::Uuid::Storage{};
    bytes[6]   = std::byte{0x70};
    bytes[8]   = std::byte{0x80};
    for (std::size_t index = 0; index < sizeof(suffix); ++index) {
        bytes[15 - index] = static_cast<std::byte>((suffix >> (index * 8U)) & 0xffU);
    }
    return core::Uuid{bytes};
}

auto recovery_queue(core::Uuid id, QueueState state, RecoveryPolicy policy) -> Queue
{
    return {.id              = id,
            .name            = "recovery-" + id.to_string(),
            .state           = state,
            .recovery_policy = policy,
            .created_at      = at(1),
            .updated_at      = at(2),
            .deleted_at      = state == QueueState::Deleted ? std::optional{at(2)} : std::nullopt};
}

RecoveryFixture::RecoveryFixture()
    : database{std::make_unique<db::sqlite::Driver>(db::sqlite::Options{.database_file = database_file,
                                                                        .busy_timeout  = 1000ms,
                                                                        .durability = db::sqlite::Durability::Normal})}
{
    REQUIRE(database.open());
    REQUIRE(jobu::sqlite::ensure_schema(database));
}

auto RecoveryFixture::make_job(core::Uuid id, core::Uuid queue_id, JobType type) const -> JobDefinition
{
    auto attributes = materialize_attributes(registry,
                                             {
    },
                                             {},
                                             {{"retry.max_attempts", {.data = std::int64_t{3}}}});
    REQUIRE(attributes);
    core::JsonValue payload{.data = core::JsonValue::Object{}};
    if (type == JobType::Cli) {
        payload.data = core::JsonValue::Object{
            {"command", {.data = std::string{"/bin/true"}}}
        };
    }
    else {
        payload.data = core::JsonValue::Object{
            {"url", {.data = std::string{"http://127.0.0.1/fixture"}}}
        };
    }
    return {.id         = id,
            .queue_id   = queue_id,
            .type       = type,
            .schedule   = OnceSchedule{at(10)},
            .attributes = std::move(*attributes),
            .payload    = std::move(payload),
            .created_at = at(1),
            .updated_at = at(2)};
}

auto RecoveryFixture::make_run(core::Uuid           id,
                               JobDefinition const& job,
                               RunState             state,
                               AttemptNumber        prior_failures,
                               RunOrigin            origin) const -> RecoveryRunFixture
{
    REQUIRE((origin == RunOrigin::Scheduled || origin == RunOrigin::Manual));
    // Leave room for the current attempt and one extra row when checking for unexpected history.
    REQUIRE(prior_failures < 999);
    REQUIRE((state != RunState::Scheduled || prior_failures == 0));
    REQUIRE((state != RunState::RetryWait || prior_failures > 0));

    auto expected = RecoveryRunFixture{
        .run = {.id             = id,
                .job_id         = job.id,
                .job_revision   = job.revision,
                .queue_id       = job.queue_id,
                .origin         = origin,
                .schedule_owned = origin == RunOrigin::Scheduled,
                .planned_at     = at(10),
                .runnable_at    = at(10),
                .type           = job.type,
                .priority       = job.priority,
                .attributes     = job.attributes,
                .payload        = job.payload,
                .state          = state}
    };

    // A retry changes due time, but the run retains its first start and immutable snapshot.
    auto due = at(10);
    for (AttemptNumber number = 1; number <= prior_failures; ++number) {
        expected.attempts.push_back({
            .attempt = {.run_id         = id,
                        .attempt_number = number,
                        .due_at         = due,
                        .started_at     = due + 1s,
                        .completed_at   = due + 2s,
                        .state          = AttemptState::Completed,
                        .outcome        = AttemptOutcome::Failed,
                        .result         = result_document()}
        });
        due += 10s;
    }
    expected.run.runnable_at = due;
    if (state != RunState::Scheduled && (state != RunState::Cancelled || prior_failures != 0)) {
        expected.run.started_at = at(11);
    }

    if (state == RunState::Running || state == RunState::Succeeded || state == RunState::Failed ||
        state == RunState::Interrupted) {
        auto attempt = JobAttempt{.run_id         = id,
                                  .attempt_number = prior_failures + 1,
                                  .due_at         = due,
                                  .started_at     = due + 1s,
                                  .state          = AttemptState::Running};
        if (state != RunState::Running) {
            attempt.state        = AttemptState::Completed;
            attempt.completed_at = due + 2s;
            attempt.result       = result_document();
            if (state == RunState::Succeeded) {
                attempt.outcome = AttemptOutcome::Succeeded;
            }
            else if (state == RunState::Failed) {
                attempt.outcome = AttemptOutcome::Failed;
            }
            else {
                attempt.outcome = AttemptOutcome::Interrupted;
            }
        }
        expected.attempts.push_back({.attempt = std::move(attempt)});
    }

    if (state == RunState::Succeeded || state == RunState::Failed || state == RunState::Interrupted ||
        state == RunState::Cancelled) {
        expected.run.completed_at = due + 2s;
        expected.run.result       = result_document();
    }
    return expected;
}

void RecoveryFixture::insert_queue(Queue const& queue)
{
    auto defaults = encode_and_serialize_attribute_document(registry,
                                                            queue.defaults,
                                                            AttributeScope::QueueDefault,
                                                            AttributeDocumentMode::Partial);
    REQUIRE(defaults);
    QueueRepository queues{database, registry};

    // The normal insertion API creates live queues. Use its tombstone writer for the deleted representation.
    auto live  = queue;
    live.state = queue.state == QueueState::Deleted ? QueueState::Suspended : queue.state;
    live.deleted_at.reset();
    REQUIRE(queues.insert(live, live.name, *defaults));
    if (queue.state == QueueState::Deleted) {
        REQUIRE(queue.deleted_at);
        auto deleted = queues.mark_deleted(queue.id, "deleted-" + queue.id.to_string(), queue.name, *queue.deleted_at);
        REQUIRE(deleted);
        REQUIRE(*deleted);
    }
}

void RecoveryFixture::insert_job(JobDefinition const& job)
{
    auto attributes = encode_and_serialize_attribute_document(registry,
                                                              job.attributes,
                                                              AttributeScope::Job,
                                                              AttributeDocumentMode::Materialized);
    auto payload    = validate_and_serialize_job_payload(job.type, job.payload);
    REQUIRE(attributes);
    REQUIRE(payload);
    JobRepository jobs{database, registry};
    REQUIRE(jobs.insert(job, *attributes, *payload));
}

void RecoveryFixture::insert_run(RecoveryRunFixture const& expected)
{
    auto const& run        = expected.run;
    auto        attributes = encode_and_serialize_attribute_document(registry,
                                                                     run.attributes,
                                                                     AttributeScope::Job,
                                                                     AttributeDocumentMode::Materialized);
    REQUIRE(attributes);
    auto revision = revision_to_storage(run.job_revision);
    REQUIRE(revision);
    auto begun = db::Transaction::begin(database);
    REQUIRE(begun);
    auto transaction = std::move(*begun);

    // Seed a complete snapshot directly: insert_manual() deliberately accepts only newly Scheduled runs.
    // This also keeps deliberate corruption in the test visible instead of silently normalizing it here.
    {
        db::Query query{database};
        REQUIRE(query.prepare(
            "INSERT INTO jobu_runs(id, job_id, job_revision, queue_id, origin, schedule_owned, planned_at_us, "
            "runnable_at_us, started_at_us, completed_at_us, type, priority, attributes_json, payload_json, state, "
            "result_json) VALUES(:id, :job, :revision, :queue, :origin, :owned, :planned, :due, :started, "
            ":completed, :type, :priority, :attributes, :payload, :state, :result)"));
        REQUIRE(query.bind_value(":id", uuid_to_storage(run.id)));
        REQUIRE(query.bind_value(":job", uuid_to_storage(run.job_id)));
        REQUIRE(query.bind_value(":revision", *revision));
        REQUIRE(query.bind_value(":queue", uuid_to_storage(run.queue_id)));
        REQUIRE(query.bind_value(":origin", db::make_text(storage_text(run.origin))));
        REQUIRE(query.bind_value(":owned", boolean_to_storage(run.schedule_owned)));
        REQUIRE(query.bind_value(":planned", timestamp(run.planned_at)));
        REQUIRE(query.bind_value(":due", timestamp(run.runnable_at)));
        REQUIRE(query.bind_value(":started", timestamp(run.started_at)));
        REQUIRE(query.bind_value(":completed", timestamp(run.completed_at)));
        REQUIRE(query.bind_value(":type", db::make_text(storage_text(run.type))));
        REQUIRE(query.bind_value(":priority", int32_to_storage(run.priority)));
        REQUIRE(query.bind_value(":attributes", db::make_text(attributes->serialized())));
        REQUIRE(query.bind_value(":payload", json(run.payload)));
        REQUIRE(query.bind_value(":state", db::make_text(storage_text(run.state))));
        REQUIRE(query.bind_value(":result", json(run.result)));
        REQUIRE(query.exec());
    }

    AttemptRepository attempts{database};
    for (auto const& entry : expected.attempts) {
        REQUIRE(attempts.insert_attempt(entry.attempt));
        if (entry.output) {
            REQUIRE(attempts.insert_or_replace_output(run.id, entry.attempt.attempt_number, *entry.output));
        }
    }
    REQUIRE(transaction.commit());
}

void RecoveryFixture::reopen()
{
    REQUIRE(database.close());
    REQUIRE(database.open());
    REQUIRE(jobu::sqlite::ensure_schema(database));
}

void RecoveryFixture::require_run(RecoveryRunFixture const& expected)
{
    RunRepository runs{database, registry};
    auto          found = runs.find_by_id(expected.run.id);
    REQUIRE(found);
    REQUIRE(found->has_value());
    auto const& actual = **found;
    auto const& run    = expected.run;
    CHECK(actual.id == run.id);
    CHECK(actual.job_id == run.job_id);
    CHECK(actual.job_revision == run.job_revision);
    CHECK(actual.queue_id == run.queue_id);
    CHECK(actual.origin == run.origin);
    CHECK(actual.schedule_owned == run.schedule_owned);
    CHECK(actual.planned_at == run.planned_at);
    CHECK(actual.runnable_at == run.runnable_at);
    CHECK(actual.started_at == run.started_at);
    CHECK(actual.completed_at == run.completed_at);
    CHECK(actual.type == run.type);
    CHECK(actual.priority == run.priority);
    auto actual_attributes = encode_attribute_document(registry,
                                                       actual.attributes,
                                                       AttributeScope::Job,
                                                       AttributeDocumentMode::Materialized);
    auto expected_attributes =
        encode_attribute_document(registry, run.attributes, AttributeScope::Job, AttributeDocumentMode::Materialized);
    REQUIRE(actual_attributes);
    REQUIRE(expected_attributes);
    CHECK(*actual_attributes == *expected_attributes);
    CHECK(actual.payload == run.payload);
    CHECK(actual.state == run.state);
    CHECK(actual.result == run.result);

    // Compare the entire small history, not just expected keys, so speculative or duplicate attempts are visible.
    REQUIRE(expected.attempts.size() < 1000);
    AttemptRepository attempts{database};
    auto              history = attempts.list_for_run(run.id, 1000);
    REQUIRE(history);
    REQUIRE(history->size() == expected.attempts.size());
    for (std::size_t index = 0; index < history->size(); ++index) {
        auto const& actual_attempt = (*history)[index];
        auto const& entry          = expected.attempts[index];
        auto const& attempt        = entry.attempt;
        CHECK(actual_attempt.run_id == attempt.run_id);
        CHECK(actual_attempt.attempt_number == attempt.attempt_number);
        CHECK(actual_attempt.due_at == attempt.due_at);
        CHECK(actual_attempt.started_at == attempt.started_at);
        CHECK(actual_attempt.completed_at == attempt.completed_at);
        CHECK(actual_attempt.state == attempt.state);
        CHECK(actual_attempt.outcome == attempt.outcome);
        CHECK(actual_attempt.result == attempt.result);

        auto output = attempts.find_output(run.id, attempt.attempt_number);
        REQUIRE(output);
        REQUIRE(output->has_value() == entry.output.has_value());
        if (entry.output) {
            CHECK((*output)->stdout_bytes == entry.output->stdout_bytes);
            CHECK((*output)->stderr_bytes == entry.output->stderr_bytes);
            CHECK((*output)->stdout_truncated == entry.output->stdout_truncated);
            CHECK((*output)->stderr_truncated == entry.output->stderr_truncated);
            CHECK((*output)->capture_lost == entry.output->capture_lost);
        }
    }
}

} // namespace jb::test
