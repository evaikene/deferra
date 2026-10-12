#include "jobu_scale_fixture.hpp"

#include "attempt_repository_priv.hpp"
#include "attribute_codec_priv.hpp"
#include "domain_storage_priv.hpp"
#include "idempotency_codec_priv.hpp"
#include "idempotency_repository_priv.hpp"
#include "job_repository_priv.hpp"
#include "job_validation_priv.hpp"
#include "query.hpp"
#include "run_repository_priv.hpp"
#include "transaction.hpp"

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <set>
#include <string>
#include <sys/resource.h>
#include <utility>
#include <variant>

namespace jb::test {

using namespace std::chrono_literals;
using namespace jobu;
using namespace jobu::detail;

auto ScaleUuidGenerator::generate() -> core::Result<core::Uuid, core::Error>
{
    return core::Result<core::Uuid, core::Error>::success(recovery_id(_next++));
}

auto ScaleDatabaseObservation::classify(std::string_view sql) -> std::string
{
    auto name = std::string{"other"};
    if (sql.starts_with("SELECT r.id, r.job_id")) {
        name = "eligibility";
    }
    else if (sql.find("AS retry_history_attempt_count") != std::string_view::npos) {
        name = "scheduler.projection";
    }
    else if (sql.starts_with("SELECT run_id FROM jobu_run_timing")) {
        name = "open.page";
    }
    else if (sql.starts_with("UPDATE jobu_run_timing SET delay_warned")) {
        name = "warning.claim";
    }
    else if (sql.starts_with("UPDATE jobu_run_timing")) {
        name = "timing.write";
    }
    else if (sql.starts_with("DELETE FROM jobu_runs")) {
        name = "run.delete";
    }
    else if (sql.starts_with("DELETE FROM jobu_idempotency")) {
        name = "replay.delete";
    }
    else if (sql.starts_with("SELECT id AS owner_id") || sql.starts_with("SELECT resource_id AS owner_id")) {
        name = "owner.page";
    }
    else if (sql.starts_with("SELECT id AS run_id, completed_at_us")) {
        name = "history.page";
    }
    else if (sql.starts_with("SELECT r.state AS run_state")) {
        name = "statistics.runs";
    }
    else if (sql.starts_with("SELECT a.attempt_number AS attempt_number")) {
        name = "statistics.attempts";
    }
    else if (sql.starts_with("SELECT DISTINCT")) {
        name = "statistics.groups";
    }

    // Capture only the statement families whose plans/page bounds matter to this gate.
    // Dynamic placeholder counts produce a bounded inventory (at most the accepted batch limit).
    if (name != "other" && name != "timing.write" && name != "warning.claim") {
        statements.try_emplace(std::string{sql}, name);
    }
    return name;
}

void ScaleDatabaseObservation::observe(DatabaseCall const& call)
{
    if (call.phase != DatabaseFaultPhase::AfterSuccess) {
        return;
    }
    if (call.operation == DatabaseOperation::Begin) {
        transaction_open = true;
    }
    else if (call.operation == DatabaseOperation::Commit || call.operation == DatabaseOperation::Rollback) {
        transaction_open  = false;
        commits          += call.operation == DatabaseOperation::Commit ? 1 : 0;
    }
    else if (call.operation == DatabaseOperation::Execute) {
        ++executes[call.boundary];
        page_rows[call.boundary] = 0;
    }
    else if (call.operation == DatabaseOperation::Fetch) {
        ++fetches[call.boundary];
        // The forwarding checkpoint includes the final empty fetch. Page bounds therefore allow one extra.
        auto const rows                  = ++page_rows[call.boundary];
        maximum_page_rows[call.boundary] = std::max(maximum_page_rows[call.boundary], rows);
    }
}

void ScaleDatabaseObservation::reset_counts()
{
    executes.clear();
    fetches.clear();
    page_rows.clear();
    maximum_page_rows.clear();
    commits = 0;
}

JobuScaleFixture::JobuScaleFixture(std::optional<std::filesystem::path> const& path)
    : storage{[this](std::unique_ptr<db::Driver> driver) {
                  return std::make_unique<FaultDatabaseDriver>(std::move(driver), faults);
              },
              path}
{
    faults->record_calls = false;
    faults->calls.clear();
    faults->classify = [this](std::string_view sql) { return observation.classify(sql); };
    faults->observe  = [this](DatabaseCall const& call) { observation.observe(call); };
}

auto JobuScaleFixture::queue_id(std::size_t index) -> core::Uuid
{
    return recovery_id(static_cast<std::uint32_t>(index + 1));
}

auto JobuScaleFixture::pending_id(std::size_t index) -> core::Uuid
{
    return recovery_id(static_cast<std::uint32_t>(10'000'000 + (2 * index)));
}

auto JobuScaleFixture::terminal_id(std::size_t index) -> core::Uuid
{
    return recovery_id(static_cast<std::uint32_t>(20'000'000 + (2 * index)));
}

auto JobuScaleFixture::pending_queue(std::size_t index, JobuScaleProfile profile) -> std::size_t
{
    return index < profile.pending / 2 ? 0 : 1 + (index % (profile.queues - 1));
}

auto JobuScaleFixture::terminal_queue(std::size_t index, JobuScaleProfile profile) -> std::size_t
{
    return index < profile.terminal / 3 ? 0 : 1 + (index % (profile.queues - 1));
}

void JobuScaleFixture::seed(JobuScaleProfile profile, std::string const& http_url)
{
    REQUIRE(profile.queues >= 3);
    REQUIRE(profile.queues <= 100);
    REQUIRE(profile.pending <= 10'000);
    REQUIRE(profile.terminal <= 100'000);
    REQUIRE(count() == 0);

    for (std::size_t index = 0; index < profile.queues; ++index) {
        auto queue                  = recovery_queue(queue_id(index));
        queue.concurrency_limit     = 1;
        queue.weight                = static_cast<std::uint32_t>(1 + (index % 5));
        queue.runnable_wait_warning = 10ms;
        if (index % 3 == 2) {
            queue.history_retention = 0s;
        }
        else if (index % 3 == 0) {
            queue.history_retention = 10s;
        }
        storage.insert_queue(queue);
    }

    // Serialization happens once per runner type. Jobs/runs still go through their real insertion APIs.
    auto cli    = storage.make_job(recovery_id(500), queue_id(0));
    cli.payload = {
        .data = core::JsonValue::Object{{"command", {.data = std::string{"/bin/sleep"}}},
                                        {"arguments", {.data = core::JsonValue::Array{{.data = std::string{"600"}}}}}}
    };
    cli.attributes["retry.max_attempts"] = {.data = std::int64_t{64}};
    cli.attributes["job.timeout"]        = {.data = core::Duration{600s}};
    auto http                            = cli;
    http.type                            = JobType::Http;
    http.payload                         = {.data = core::JsonValue::Object{{"url", {.data = http_url}}}};
    auto attributes                      = encode_and_serialize_attribute_document(storage.registry,
                                                                                   cli.attributes,
                                                                                   AttributeScope::Job,
                                                                                   AttributeDocumentMode::Materialized);
    auto cli_payload                     = validate_and_serialize_job_payload(cli.type, cli.payload);
    auto http_payload                    = validate_and_serialize_job_payload(http.type, http.payload);
    REQUIRE(attributes);
    REQUIRE(cli_payload);
    REQUIRE(http_payload);
    JobRepository         jobs{storage.database, storage.registry};
    RunRepository         runs{storage.database, storage.registry};
    AttemptRepository     attempts{storage.database};
    IdempotencyRepository replays{storage.database};

    for (std::size_t first = 0; first < profile.pending + profile.terminal; first += 500) {
        auto transaction = db::Transaction::begin(storage.database);
        REQUIRE(transaction);
        auto const last = std::min(first + 500, profile.pending + profile.terminal);
        for (auto index = first; index < last; ++index) {
            auto const  terminal = index >= profile.pending;
            auto const  ordinal  = terminal ? index - profile.pending : index;
            auto const  id       = terminal ? terminal_id(ordinal) : pending_id(ordinal);
            auto        job      = ordinal % 2 == 0 ? cli : http;
            auto const& payload  = ordinal % 2 == 0 ? *cli_payload : *http_payload;
            job.id = recovery_id(static_cast<std::uint32_t>((terminal ? 20'000'001 : 10'000'001) + (2 * ordinal)));
            job.queue_id = queue_id(terminal ? terminal_queue(ordinal, profile) : pending_queue(ordinal, profile));
            job.state    = terminal ? JobState::Succeeded : JobState::Active;
            REQUIRE(jobs.insert(job, *attributes, payload));
            // Manual replay must retire atomically with its run. The remaining terminal
            // rows are schedule-owned; pending creation keys must remain pinned by live work.
            if (terminal && ordinal < 100) {
                auto snapshot = storage.make_run(id, job, RunState::Scheduled, 0, RunOrigin::Manual).run;
                REQUIRE(runs.insert_manual(snapshot, InitialRunMeasurement::Complete));
                auto request = encode_run_now_idempotency_request({.job_id = job.id});
                auto replay  = encode_run_now_idempotency_result(snapshot, storage.registry);
                REQUIRE(request);
                REQUIRE(replay);
                REQUIRE(decode_run_now_idempotency_result(*replay, storage.registry));
                REQUIRE(replays.insert({.method       = "job.run_now",
                                        .scope_id     = job.id,
                                        .key          = "scale-manual-" + id.to_string(),
                                        .request_json = *request,
                                        .result_json  = *replay,
                                        .resource_id  = id,
                                        .created_at   = core::UtcTimePoint{1s}}));
            }
            else {
                REQUIRE(runs.insert_schedule_owned({.id              = id,
                                                    .job_id          = job.id,
                                                    .job_revision    = job.revision,
                                                    .queue_id        = job.queue_id,
                                                    .planned_at      = core::UtcTimePoint{10s},
                                                    .runnable_at     = core::UtcTimePoint{10s},
                                                    .type            = job.type,
                                                    .attributes_json = attributes->serialized(),
                                                    .payload_json    = payload.serialized()},
                                                   InitialRunMeasurement::Complete));
            }
            if (!terminal && ordinal < 16) {
                auto request = encode_job_create_idempotency_request({.queue    = job.queue_id,
                                                                      .type     = job.type,
                                                                      .schedule = OnceSchedule{core::UtcTimePoint{10s}},
                                                                      .attributes = job.attributes,
                                                                      .payload    = job.payload},
                                                                     job.queue_id,
                                                                     storage.registry);
                auto original       = job;
                original.updated_at = original.created_at;
                auto replay         = encode_job_idempotency_result(original, storage.registry);
                REQUIRE(request);
                REQUIRE(replay);
                REQUIRE(decode_job_idempotency_result(*replay, storage.registry));
                REQUIRE(replays.insert({.method       = "job.create",
                                        .scope_id     = job.queue_id,
                                        .key          = "scale-pending-" + id.to_string(),
                                        .request_json = *request,
                                        .result_json  = *replay,
                                        .resource_id  = job.id,
                                        .created_at   = core::UtcTimePoint{1s}}));
            }
            if (terminal) {
                db::Query update{storage.database};
                REQUIRE(update.prepare("UPDATE jobu_runs SET state='succeeded', started_at_us=11000000, "
                                       "completed_at_us=12000000, result_json='{}' WHERE id=:id"));
                REQUIRE(update.bind_value(":id", uuid_to_storage(id)));
                REQUIRE(update.exec());
                REQUIRE(attempts.insert_attempt({.run_id         = id,
                                                 .attempt_number = 1,
                                                 .due_at         = core::UtcTimePoint{10s},
                                                 .started_at     = core::UtcTimePoint{11s},
                                                 .completed_at   = core::UtcTimePoint{12s},
                                                 .state          = AttemptState::Completed,
                                                 .outcome        = AttemptOutcome::Succeeded,
                                                 .result = core::JsonValue{.data = core::JsonValue::Object{}}}));
                db::Query timing{storage.database};
                REQUIRE(timing.prepare("UPDATE jobu_run_timing SET measurement_status=:quality, runnable_wait_us=:wait "
                                       "WHERE run_id=:id"));
                REQUIRE(timing.bind_value(":quality",
                                          db::make_text(ordinal % 3 == 0   ? "complete"
                                                        : ordinal % 3 == 1 ? "partial"
                                                                           : "unmeasured")));
                REQUIRE(timing.bind_value(":wait", std::int64_t{ordinal % 3 == 2 ? 0 : 10'000}));
                REQUIRE(timing.bind_value(":id", uuid_to_storage(id)));
                REQUIRE(timing.exec());
            }
        }
        REQUIRE(transaction->commit());
    }
    observation.reset_counts();
}

auto JobuScaleFixture::scalar(std::string_view sql) -> std::int64_t
{
    db::Query query{storage.database};
    REQUIRE(query.exec(sql));
    auto row = query.next();
    REQUIRE(row);
    REQUIRE(*row);
    REQUIRE(std::holds_alternative<std::int64_t>(query.value(0)));
    return std::get<std::int64_t>(query.value(0));
}

auto JobuScaleFixture::count(std::string_view predicate) -> std::int64_t
{
    return scalar(fmt::format("SELECT COUNT(*) FROM jobu_runs WHERE {}", predicate));
}

void JobuScaleFixture::execute(std::string_view sql)
{
    db::Query query{storage.database};
    REQUIRE(query.exec(sql));
}

void JobuScaleFixture::query_plans()
{
    auto const statements = observation.statements;
    for (auto const& [sql, name] : statements) {
        if (!sql.starts_with("SELECT")) {
            continue;
        }
        db::Query query{storage.database};
        REQUIRE(query.prepare("EXPLAIN QUERY PLAN " + sql));
        // Bind every named slot explicitly. These captured repository statements use ASCII
        // identifiers; NULL affects no returned fixture data because EXPLAIN never executes the query.
        std::set<std::string> names;
        for (std::size_t position = 0; position < sql.size(); ++position) {
            if (sql[position] != ':') {
                continue;
            }
            auto end = position + 1;
            while (end < sql.size() &&
                   ((sql[end] >= 'a' && sql[end] <= 'z') || (sql[end] >= '0' && sql[end] <= '9') || sql[end] == '_')) {
                ++end;
            }
            names.insert(sql.substr(position, end - position));
            position = end - 1;
        }
        for (auto const& name : names) {
            REQUIRE(query.bind_value(name, db::Null{}));
        }
        REQUIRE(query.exec());
        for (;;) {
            auto row = query.next();
            REQUIRE(row);
            if (!*row) {
                break;
            }
            auto detail = read_text(query.record(), "detail");
            REQUIRE(detail);
            std::cout << "SCALE_PLAN\t" << name << '\t' << *detail << '\n';
        }
    }
}

void scale_measurement(std::string_view operation, double milliseconds, std::size_t units)
{
    std::cout << fmt::format("SCALE_MEASUREMENT\t{{\"operation\":\"{}\",\"milliseconds\":{},\"units\":{}}}\n",
                             operation,
                             milliseconds,
                             units);
    rusage usage{};
    REQUIRE(::getrusage(RUSAGE_SELF, &usage) == 0);
    std::cout << fmt::format("SCALE_MEMORY\t{{\"operation\":\"{}\",\"peak_rss_kib\":{}}}\n",
                             operation,
                             usage.ru_maxrss);
}

} // namespace jb::test
