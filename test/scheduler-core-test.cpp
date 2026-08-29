#include "scheduler_core_priv.hpp"

#include "attempt_repository_priv.hpp"
#include "attribute_codec_priv.hpp"
#include "attribute_registry.hpp"
#include "database.hpp"
#include "domain_storage_priv.hpp"
#include "management.hpp"
#include "query.hpp"
#include "run_repository_priv.hpp"
#include "sqlite/sqlite_driver.hpp"
#include "sqlite/sqlite_schema.hpp"
#include "support/fake_attempt_executor.hpp"
#include "support/fake_cron_engine.hpp"
#include "support/fake_time_source.hpp"
#include "support/sequence_uuid_generator.hpp"
#include "support/temporary_directory.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace jb::core;
using namespace jb::db;
using namespace jb::jobu;
using namespace jb::jobu::detail;
using namespace jb::test;
using namespace std::chrono_literals;

namespace {

auto id(std::uint8_t suffix) -> Uuid
{
    auto bytes = Uuid::Storage{};
    bytes[6]   = std::byte{0x70};
    bytes[8]   = std::byte{0x80};
    bytes[15]  = static_cast<std::byte>(suffix);
    return Uuid{bytes};
}

auto at(std::int64_t microseconds) -> UtcTimePoint
{
    return UtcTimePoint{std::chrono::microseconds{microseconds}};
}

auto make_database(std::filesystem::path database_file) -> Database
{
    return Database{std::make_unique<jb::db::sqlite::Driver>(jb::db::sqlite::Options{
        .database_file = std::move(database_file),
        .busy_timeout  = 1000ms,
        .durability    = jb::db::sqlite::Durability::Normal,
    })};
}

struct CoreFixture {
    explicit CoreFixture(std::vector<Uuid> successor_ids = {})
        : generator{std::move(successor_ids)}
        , attempts{database}
        , runs{database, registry}
    {
        REQUIRE(database.open());
        REQUIRE(jb::jobu::sqlite::ensure_schema(database));
        time.set_utc(at(120));
    }

    [[nodiscard]] auto process(SchedulerCoreOptions options) -> Result<SchedulerCycleResult, Error>
    {
        SchedulerCore core{database, registry, cron, generator, time, executor, options};
        return core.process_cycle();
    }

    TemporaryDirectory        directory;
    std::filesystem::path     database_file{directory.path() / "jobu.sqlite"};
    Database                  database{make_database(database_file)};
    StandardAttributeRegistry registry;
    FakeCronEngine            cron;
    SequenceUuidGenerator     generator;
    FakeTimeSource            time;
    FakeAttemptExecutor       executor;
    AttemptRepository         attempts;
    RunRepository             runs;
};

void insert_queue(Database& database, Uuid const& queue_id, std::uint32_t concurrency_limit, std::uint32_t weight = 1)
{
    Query query{database};
    REQUIRE(query.prepare(
        "INSERT INTO jobu_queues(id, name, deleted_name, state, weight, concurrency_limit, recovery_policy, "
        "defaults_json, retention_seconds, runnable_wait_warning_ms, created_at_us, updated_at_us, deleted_at_us) "
        "VALUES(:id, :name, NULL, 'active', :weight, :concurrency_limit, 'fail_interrupted', "
        "'{\"version\":1,\"values\":{}}', NULL, 10000, 0, 0, NULL)"));
    REQUIRE(query.bind_value(":id", uuid_to_storage(queue_id)));
    REQUIRE(
        query.bind_value(":name",
                         make_text("queue-" + std::to_string(std::to_integer<unsigned int>(queue_id.bytes().back())))));
    REQUIRE(query.bind_value(":weight", static_cast<std::int64_t>(weight)));
    REQUIRE(query.bind_value(":concurrency_limit", static_cast<std::int64_t>(concurrency_limit)));
    REQUIRE(query.exec());
}

void insert_job(Database&                   database,
                Uuid const&                 job_id,
                Uuid const&                 queue_id,
                JobType                     type,
                JobState                    state           = JobState::Active,
                std::optional<CronSchedule> schedule        = std::nullopt,
                JobRevision                 job_revision    = 1,
                std::int32_t                priority        = 0,
                std::string_view            attributes_json = R"({"version":1,"values":{}})",
                std::string_view            payload_json    = {})
{
    auto stored_payload = payload_json;
    if (stored_payload.empty()) {
        stored_payload = type == JobType::Cli ? std::string_view{R"({"command":"test"})"}
                                              : std::string_view{R"({"url":"https://example.test"})"};
    }
    auto revision = revision_to_storage(job_revision);
    REQUIRE(revision);
    Query query{database};
    REQUIRE(query.prepare(
        "INSERT INTO jobu_jobs(id, queue_id, revision, name, state, type, schedule_kind, scheduled_at_us, "
        "cron_expression, cron_timezone, priority, attributes_json, payload_json, created_at_us, updated_at_us, "
        "deleted_at_us) VALUES(:id, :queue_id, :revision, NULL, :state, :type, :schedule_kind, :scheduled_at_us, "
        ":cron_expression, :cron_timezone, :priority, :attributes_json, :payload_json, 0, 0, NULL)"));
    REQUIRE(query.bind_value(":id", uuid_to_storage(job_id)));
    REQUIRE(query.bind_value(":queue_id", uuid_to_storage(queue_id)));
    REQUIRE(query.bind_value(":revision", std::move(revision).value()));
    REQUIRE(query.bind_value(":state", make_text(storage_text(state))));
    REQUIRE(query.bind_value(":type", make_text(storage_text(type))));
    REQUIRE(query.bind_value(":schedule_kind", make_text(schedule ? "cron" : "once")));
    REQUIRE(query.bind_value(":scheduled_at_us", schedule ? Value{Null{}} : Value{std::int64_t{0}}));
    REQUIRE(query.bind_value(":cron_expression", schedule ? Value{schedule->expression} : Value{Null{}}));
    REQUIRE(query.bind_value(":cron_timezone", schedule ? Value{schedule->timezone} : Value{Null{}}));
    REQUIRE(query.bind_value(":priority", int32_to_storage(priority)));
    REQUIRE(query.bind_value(":attributes_json", make_text(attributes_json)));
    REQUIRE(query.bind_value(":payload_json", make_text(stored_payload)));
    REQUIRE(query.exec());
}

void update_recurring_job(Database&           database,
                          Uuid const&         job_id,
                          JobRevision         revision,
                          CronSchedule const& schedule,
                          JobType             type,
                          std::int32_t        priority,
                          std::string_view    attributes_json,
                          std::string_view    payload_json)
{
    auto stored_revision = revision_to_storage(revision);
    REQUIRE(stored_revision);
    Query query{database};
    REQUIRE(query.prepare("UPDATE jobu_jobs SET revision = :revision, type = :type, cron_expression = :expression, "
                          "cron_timezone = :timezone, priority = :priority, attributes_json = :attributes_json, "
                          "payload_json = :payload_json, updated_at_us = 1 WHERE id = :id"));
    REQUIRE(query.bind_value(":revision", std::move(stored_revision).value()));
    REQUIRE(query.bind_value(":type", make_text(storage_text(type))));
    REQUIRE(query.bind_value(":expression", make_text(schedule.expression)));
    REQUIRE(query.bind_value(":timezone", make_text(schedule.timezone)));
    REQUIRE(query.bind_value(":priority", int32_to_storage(priority)));
    REQUIRE(query.bind_value(":attributes_json", make_text(attributes_json)));
    REQUIRE(query.bind_value(":payload_json", make_text(payload_json)));
    REQUIRE(query.bind_value(":id", uuid_to_storage(job_id)));
    REQUIRE(query.exec());
    REQUIRE(query.num_rows_affected() == 1);
}

void mark_job_deleted(Database& database, Uuid const& job_id)
{
    Query query{database};
    REQUIRE(query.prepare("UPDATE jobu_jobs SET state = 'deleted', revision = revision + 1, updated_at_us = 1, "
                          "deleted_at_us = 1 WHERE id = :id"));
    REQUIRE(query.bind_value(":id", uuid_to_storage(job_id)));
    REQUIRE(query.exec());
    REQUIRE(query.num_rows_affected() == 1);
}

auto attribute_document(StandardAttributeRegistry const& registry,
                        std::string                      retry_mode     = "reschedule",
                        std::int64_t                     max_attempts   = 3,
                        Duration                         initial_delay  = Duration::zero(),
                        std::string                      output_capture = "on_error") -> std::string
{
    auto attributes = materialize_attributes(registry,
                                             {
    },
                                             {},
                                             {
                                                 {"retry.initial_delay", {.data = initial_delay}},
                                                 {"retry.max_attempts", {.data = max_attempts}},
                                                 {"retry.mode", {.data = std::move(retry_mode)}},
                                                 {"output.capture", {.data = std::move(output_capture)}},
                                             });
    REQUIRE(attributes);
    auto document = encode_and_serialize_attribute_document(registry,
                                                            *attributes,
                                                            AttributeScope::Job,
                                                            AttributeDocumentMode::Materialized);
    REQUIRE(document);
    return std::string{document->serialized()};
}

struct RunSpec {
    Uuid                        id;
    Uuid                        job_id;
    Uuid                        queue_id;
    UtcTimePoint                planned_at{at(100)};
    UtcTimePoint                runnable_at{at(100)};
    JobType                     type{JobType::Cli};
    std::int32_t                priority{0};
    std::string                 attributes_json;
    std::string                 payload_json{"{}"};
    RunOrigin                   origin{RunOrigin::Scheduled};
    bool                        schedule_owned{true};
    RunState                    state{RunState::Scheduled};
    std::optional<UtcTimePoint> started_at;
};

void insert_run(Database& database, RunSpec const& run)
{
    auto planned  = timestamp_to_storage(run.planned_at);
    auto runnable = timestamp_to_storage(run.runnable_at);
    REQUIRE(planned);
    REQUIRE(runnable);

    Query query{database};
    REQUIRE(query.prepare(
        "INSERT INTO jobu_runs(id, job_id, job_revision, queue_id, origin, schedule_owned, planned_at_us, "
        "runnable_at_us, started_at_us, completed_at_us, type, priority, attributes_json, payload_json, state, "
        "result_json) VALUES(:id, :job_id, 1, :queue_id, :origin, :schedule_owned, :planned_at_us, :runnable_at_us, "
        ":started_at_us, NULL, :type, :priority, :attributes_json, :payload_json, :state, NULL)"));
    REQUIRE(query.bind_value(":id", uuid_to_storage(run.id)));
    REQUIRE(query.bind_value(":job_id", uuid_to_storage(run.job_id)));
    REQUIRE(query.bind_value(":queue_id", uuid_to_storage(run.queue_id)));
    REQUIRE(query.bind_value(":origin", make_text(storage_text(run.origin))));
    REQUIRE(query.bind_value(":schedule_owned", boolean_to_storage(run.schedule_owned)));
    REQUIRE(query.bind_value(":planned_at_us", std::move(planned).value()));
    REQUIRE(query.bind_value(":runnable_at_us", std::move(runnable).value()));
    if (run.started_at) {
        auto started = timestamp_to_storage(*run.started_at);
        REQUIRE(started);
        REQUIRE(query.bind_value(":started_at_us", std::move(started).value()));
    }
    else {
        REQUIRE(query.bind_value(":started_at_us", Null{}));
    }
    REQUIRE(query.bind_value(":type", make_text(storage_text(run.type))));
    REQUIRE(query.bind_value(":priority", int32_to_storage(run.priority)));
    REQUIRE(query.bind_value(":attributes_json", make_text(run.attributes_json)));
    REQUIRE(query.bind_value(":payload_json", make_text(run.payload_json)));
    REQUIRE(query.bind_value(":state", make_text(storage_text(run.state))));
    REQUIRE(query.exec());
}

auto default_run(CoreFixture const& fixture, Uuid run_id, Uuid job_id, Uuid queue_id, JobType type) -> RunSpec
{
    return {
        .id              = run_id,
        .job_id          = job_id,
        .queue_id        = queue_id,
        .type            = type,
        .attributes_json = attribute_document(fixture.registry),
    };
}

void insert_scheduled(CoreFixture& fixture,
                      Uuid const&  queue_id,
                      std::uint8_t run_suffix,
                      JobType      type,
                      std::int32_t priority        = 0,
                      std::int64_t runnable        = 100,
                      std::int64_t planned         = 100,
                      std::string  attributes_json = {})
{
    auto const job_id = id(static_cast<std::uint8_t>(run_suffix + 100));
    insert_job(fixture.database, job_id, queue_id, type);
    auto run        = default_run(fixture, id(run_suffix), job_id, queue_id, type);
    run.priority    = priority;
    run.runnable_at = at(runnable);
    run.planned_at  = at(planned);
    if (!attributes_json.empty()) {
        run.attributes_json = std::move(attributes_json);
    }
    insert_run(fixture.database, run);
}

void insert_running(CoreFixture& fixture, Uuid const& queue_id, std::uint8_t run_suffix, JobType type)
{
    auto const job_id = id(static_cast<std::uint8_t>(run_suffix + 100));
    insert_job(fixture.database, job_id, queue_id, type);
    auto run       = default_run(fixture, id(run_suffix), job_id, queue_id, type);
    run.state      = RunState::Running;
    run.started_at = at(90);
    insert_run(fixture.database, run);
    REQUIRE(fixture.attempts.insert_attempt(JobAttempt{
        .run_id         = run.id,
        .attempt_number = 1,
        .due_at         = at(80),
        .started_at     = at(90),
        .state          = AttemptState::Running,
    }));
}

void insert_blocking_retry(CoreFixture& fixture,
                           Uuid const&  queue_id,
                           std::uint8_t run_suffix,
                           JobState     job_state = JobState::Active)
{
    auto const job_id = id(static_cast<std::uint8_t>(run_suffix + 100));
    insert_job(fixture.database, job_id, queue_id, JobType::Cli, job_state);
    auto run            = default_run(fixture, id(run_suffix), job_id, queue_id, JobType::Cli);
    run.state           = RunState::RetryWait;
    run.started_at      = at(70);
    run.runnable_at     = at(130);
    run.attributes_json = attribute_document(fixture.registry, "blocking");
    insert_run(fixture.database, run);

    auto result  = JsonValue{};
    result.data  = JsonValue::Object{};
    auto attempt = JobAttempt{
        .run_id         = run.id,
        .attempt_number = 1,
        .due_at         = at(60),
        .started_at     = at(60),
        .completed_at   = at(70),
        .state          = AttemptState::Completed,
        .outcome        = AttemptOutcome::Failed,
        .result         = std::move(result),
    };
    REQUIRE(fixture.attempts.insert_attempt(attempt));
}

void insert_scheduled_range(CoreFixture& fixture,
                            Uuid const&  queue_id,
                            std::uint8_t first_suffix,
                            std::size_t  count,
                            JobType      type)
{
    for (auto index = std::size_t{0}; index < count; ++index) {
        insert_scheduled(fixture, queue_id, static_cast<std::uint8_t>(first_suffix + index), type);
    }
}

auto starts_for(FakeAttemptExecutor const& executor, Uuid const& queue_id, JobType type) -> std::size_t
{
    return static_cast<std::size_t>(std::count_if(
        executor.start_requests().begin(),
        executor.start_requests().end(),
        [&](AttemptStartRequest const& request) { return request.queue_id == queue_id && request.type == type; }));
}

void check_cli_ratio(std::uint32_t first_weight,
                     std::uint32_t second_weight,
                     std::uint32_t dispatches,
                     std::size_t   expected_first,
                     std::size_t   expected_second)
{
    CoreFixture fixture;
    auto const  first_queue  = id(1);
    auto const  second_queue = id(2);
    insert_queue(fixture.database, first_queue, 40, first_weight);
    insert_queue(fixture.database, second_queue, 40, second_weight);
    insert_scheduled_range(fixture, first_queue, 10, 40, JobType::Cli);
    insert_scheduled_range(fixture, second_queue, 60, 40, JobType::Cli);
    fixture.executor.set_available(JobType::Cli, true);

    REQUIRE(fixture.process({.cli_concurrency = dispatches, .http_concurrency = 1, .candidate_batch_size = 7}));
    REQUIRE(fixture.executor.start_requests().size() == dispatches);
    CHECK(starts_for(fixture.executor, first_queue, JobType::Cli) == expected_first);
    CHECK(starts_for(fixture.executor, second_queue, JobType::Cli) == expected_second);
}

class AdvancingTimeSource final : public TimeSource {
public:
    [[nodiscard]] auto utc_now() const noexcept -> UtcTimePoint override
    {
        ++utc_calls;
        return at(static_cast<std::int64_t>(119U + utc_calls));
    }

    [[nodiscard]] auto monotonic_now() const noexcept -> TimePoint override { return {}; }

    mutable std::size_t utc_calls{0};
};

class LoweringExecutor final : public AttemptExecutor {
public:
    LoweringExecutor(Database& database, Uuid queue_id)
        : _database{database}
        , _queue_id{queue_id}
    {
        fake.set_available(JobType::Cli, true);
    }

    FakeAttemptExecutor fake;

    [[nodiscard]] auto is_available(JobType type) const noexcept -> bool override
    {
        if (!_lowered && type == JobType::Cli) {
            _lowered = true;
            Query query{_database};
            _mutation_ok = query.prepare("UPDATE jobu_queues SET concurrency_limit = 1 WHERE id = :id") &&
                           query.bind_value(":id", uuid_to_storage(_queue_id)) && query.exec();
        }
        return fake.is_available(type);
    }

    [[nodiscard]] auto start(AttemptStartRequest request, AttemptCompletionHandler completion)
        -> Result<void, Error> override
    {
        return fake.start(std::move(request), std::move(completion));
    }

    [[nodiscard]] auto cancel(AttemptKey const& key) -> Result<void, Error> override { return fake.cancel(key); }

    [[nodiscard]] auto mutation_ok() const noexcept -> bool { return _mutation_ok; }

private:
    Database&    _database;
    Uuid         _queue_id;
    mutable bool _lowered{false};
    mutable bool _mutation_ok{false};
};

auto result_object(std::string value = "completed") -> JsonValue
{
    auto text   = JsonValue{};
    text.data   = std::move(value);
    auto result = JsonValue{};
    result.data = JsonValue::Object{
        {"status", std::move(text)},
    };
    return result;
}

auto cli_payload(std::string command) -> JsonValue
{
    auto value   = JsonValue{};
    value.data   = std::move(command);
    auto payload = JsonValue{};
    payload.data = JsonValue::Object{
        {"command", std::move(value)},
    };
    return payload;
}

auto success(AttemptKey key, std::string value = "completed") -> AttemptCompletion
{
    return {
        .key     = key,
        .outcome = AttemptOutcome::Succeeded,
        .result  = result_object(std::move(value)),
    };
}

auto cancelled(AttemptKey key) -> AttemptCompletion
{
    return {
        .key     = key,
        .outcome = AttemptOutcome::Cancelled,
        .result  = result_object("cancelled"),
    };
}

auto failure(AttemptKey                  key,
             FailureDisposition          disposition,
             std::optional<UtcTimePoint> retry_not_before = std::nullopt) -> AttemptCompletion
{
    return {
        .key                 = key,
        .outcome             = AttemptOutcome::Failed,
        .failure_disposition = disposition,
        .retry_not_before    = retry_not_before,
        .result              = result_object("failed"),
    };
}

auto captured_output() -> jb::jobu::AttemptOutput
{
    auto primary = jb::jobu::AttemptOutputChannel{
        .bytes       = ByteBuffer{std::byte{0x00}, std::byte{0x41}},
        .total_bytes = 3,
        .truncated   = true,
    };
    auto diagnostic = jb::jobu::AttemptOutputChannel{
        .bytes       = {},
        .total_bytes = 0,
        .truncated   = false,
    };
    return {
        .primary      = std::move(primary),
        .diagnostic   = std::move(diagnostic),
        .capture_lost = true,
    };
}

void check_captured_output(AttemptRepository& attempts, AttemptKey const& key)
{
    auto output = attempts.find_output(key.run_id, key.attempt_number);
    REQUIRE(output);
    REQUIRE(output->has_value());
    auto const expected_primary = ByteBuffer{std::byte{0x00}, std::byte{0x41}};
    CHECK((*output)->stdout_bytes == expected_primary);
    REQUIRE((*output)->stderr_bytes);
    CHECK((*output)->stderr_bytes->empty());
    CHECK((*output)->stdout_truncated);
    CHECK_FALSE((*output)->stderr_truncated);
    CHECK((*output)->capture_lost);
}

class RawAttemptExecutor final : public AttemptExecutor {
public:
    [[nodiscard]] auto is_available(JobType type) const noexcept -> bool override { return type == JobType::Cli; }

    [[nodiscard]] auto start(AttemptStartRequest request, AttemptCompletionHandler completion)
        -> Result<void, Error> override
    {
        requests.push_back(std::move(request));
        handler = std::move(completion);
        return Result<void, Error>::success();
    }

    [[nodiscard]] auto cancel(AttemptKey const& key) -> Result<void, Error> override
    {
        (void)key;
        return Result<void, Error>::success();
    }

    void emit(AttemptCompletion completion)
    {
        REQUIRE(handler);
        auto selected = std::move(handler);
        handler       = {};
        selected(std::move(completion));
    }

    std::vector<AttemptStartRequest> requests;
    AttemptCompletionHandler         handler;
};

class SynchronousCompletionExecutor final : public AttemptExecutor {
public:
    [[nodiscard]] auto is_available(JobType type) const noexcept -> bool override { return type == JobType::Cli; }

    [[nodiscard]] auto start(AttemptStartRequest request, AttemptCompletionHandler completion)
        -> Result<void, Error> override
    {
        requests.push_back(request);
        completion(success(request.key));
        return Result<void, Error>::success();
    }

    [[nodiscard]] auto cancel(AttemptKey const& key) -> Result<void, Error> override
    {
        (void)key;
        return Result<void, Error>::success();
    }

    std::vector<AttemptStartRequest> requests;
};

} // anonymous namespace

TEST_CASE("Single-queue scheduler core preserves strict order across bounded batches",
          "[jobu][scheduler][core][sqlite]")
{
    CoreFixture fixture;
    auto const  queue_id = id(10);
    insert_queue(fixture.database, queue_id, 10);

    struct OrderedRun {
        std::uint8_t suffix;
        std::int32_t priority;
        std::int64_t runnable;
        std::int64_t planned;
    };

    for (auto const spec : std::vector<OrderedRun>{
             {.suffix = 20, .priority = 5, .runnable = 90, .planned = 100},
             {.suffix = 21, .priority = 6, .runnable = 90, .planned = 100},
             {.suffix = 22, .priority = 6, .runnable = 80, .planned = 110},
             {.suffix = 23, .priority = 6, .runnable = 80, .planned = 90 },
             {.suffix = 24, .priority = 6, .runnable = 80, .planned = 90 },
    }) {
        insert_scheduled(fixture, queue_id, spec.suffix, JobType::Cli, spec.priority, spec.runnable, spec.planned);
    }

    fixture.executor.set_available(JobType::Cli, true);
    AdvancingTimeSource time;
    SchedulerCore       core{
        fixture.database,
        fixture.registry,
        fixture.cron,
        fixture.generator,
        time,
        fixture.executor,
        {.cli_concurrency = 10, .http_concurrency = 10, .candidate_batch_size = 2}
    };
    REQUIRE(core.process_cycle());

    REQUIRE(time.utc_calls == 1U);
    REQUIRE(fixture.executor.start_requests().size() == 5U);
    auto const expected = std::vector<Uuid>{id(23), id(24), id(22), id(21), id(20)};
    for (auto index = std::size_t{0}; index < expected.size(); ++index) {
        CHECK(fixture.executor.start_requests()[index].key.run_id == expected[index]);
        CHECK(fixture.executor.start_requests()[index].started_at == at(120));
    }
}

TEST_CASE("Scheduler core follows smooth weighted queue ratios", "[jobu][scheduler][core][fairness][sqlite]")
{
    SECTION("1:1")
    {
        check_cli_ratio(1, 1, 30, 15, 15);
    }
    SECTION("1:2")
    {
        check_cli_ratio(1, 2, 30, 10, 20);
    }
    SECTION("2:5")
    {
        check_cli_ratio(2, 5, 35, 10, 25);
    }
}

TEST_CASE("Scheduler core breaks equal-credit ties by queue UUID", "[jobu][scheduler][core][fairness][sqlite]")
{
    CoreFixture fixture;
    auto const  first_queue  = id(1);
    auto const  second_queue = id(2);
    insert_queue(fixture.database, first_queue, 2);
    insert_queue(fixture.database, second_queue, 2);
    insert_scheduled(fixture, first_queue, 10, JobType::Cli);
    insert_scheduled(fixture, second_queue, 20, JobType::Cli);
    fixture.executor.set_available(JobType::Cli, true);

    REQUIRE(fixture.process({.cli_concurrency = 1, .http_concurrency = 1, .candidate_batch_size = 2}));
    REQUIRE(fixture.executor.start_requests().size() == 1U);
    CHECK(fixture.executor.start_requests().front().queue_id == first_queue);
}

TEST_CASE("Scheduler core resets idle credit and applies dynamic weights", "[jobu][scheduler][core][fairness][sqlite]")
{
    CoreFixture fixture;
    auto const  first_queue  = id(1);
    auto const  second_queue = id(2);
    insert_queue(fixture.database, first_queue, 8);
    insert_queue(fixture.database, second_queue, 8);
    insert_scheduled(fixture, first_queue, 10, JobType::Cli);
    fixture.executor.set_available(JobType::Cli, true);
    SchedulerCore core{
        fixture.database,
        fixture.registry,
        fixture.cron,
        fixture.generator,
        fixture.time,
        fixture.executor,
        {.cli_concurrency = 7, .http_concurrency = 1, .candidate_batch_size = 3}
    };
    ManagementService service{fixture.database, fixture.registry, fixture.cron, fixture.generator, fixture.time};

    REQUIRE(core.process_cycle());
    REQUIRE(fixture.executor.start_requests().size() == 1U);

    REQUIRE(service.update_queue({.queue = second_queue, .weight = 2, .concurrency_limit = 8}));
    insert_scheduled_range(fixture, first_queue, 11, 5, JobType::Cli);
    insert_scheduled_range(fixture, second_queue, 60, 5, JobType::Cli);
    REQUIRE(core.process_cycle());

    REQUIRE(fixture.executor.start_requests().size() == 7U);
    CHECK(starts_for(fixture.executor, first_queue, JobType::Cli) == 3U);
    CHECK(starts_for(fixture.executor, second_queue, JobType::Cli) == 4U);
}

TEST_CASE("Scheduler core keeps CLI and HTTP fairness credits independent", "[jobu][scheduler][core][fairness][sqlite]")
{
    CoreFixture fixture;
    auto const  first_queue  = id(1);
    auto const  second_queue = id(2);
    insert_queue(fixture.database, first_queue, 20, 1);
    insert_queue(fixture.database, second_queue, 20, 2);
    insert_scheduled_range(fixture, first_queue, 10, 8, JobType::Cli);
    insert_scheduled_range(fixture, second_queue, 30, 8, JobType::Cli);
    insert_scheduled_range(fixture, first_queue, 50, 8, JobType::Http);
    insert_scheduled_range(fixture, second_queue, 70, 8, JobType::Http);
    fixture.executor.set_available(JobType::Cli, true);
    fixture.executor.set_available(JobType::Http, true);

    REQUIRE(fixture.process({.cli_concurrency = 6, .http_concurrency = 6, .candidate_batch_size = 3}));
    REQUIRE(fixture.executor.start_requests().size() == 12U);
    CHECK(starts_for(fixture.executor, first_queue, JobType::Cli) == 2U);
    CHECK(starts_for(fixture.executor, second_queue, JobType::Cli) == 4U);
    CHECK(starts_for(fixture.executor, first_queue, JobType::Http) == 2U);
    CHECK(starts_for(fixture.executor, second_queue, JobType::Http) == 4U);
}

TEST_CASE("Scheduler core alternates the first type across mixed-capacity rounds",
          "[jobu][scheduler][core][fairness][sqlite]")
{
    CoreFixture fixture;
    auto const  first_queue  = id(1);
    auto const  second_queue = id(2);
    insert_queue(fixture.database, first_queue, 1);
    insert_queue(fixture.database, second_queue, 1);
    insert_scheduled(fixture, first_queue, 10, JobType::Cli);
    insert_scheduled(fixture, first_queue, 11, JobType::Http);
    insert_scheduled(fixture, second_queue, 20, JobType::Cli);
    insert_scheduled(fixture, second_queue, 21, JobType::Http);
    fixture.executor.set_available(JobType::Cli, true);
    fixture.executor.set_available(JobType::Http, true);
    SchedulerCore core{
        fixture.database,
        fixture.registry,
        fixture.cron,
        fixture.generator,
        fixture.time,
        fixture.executor,
        {.cli_concurrency = 2, .http_concurrency = 2, .candidate_batch_size = 2}
    };

    REQUIRE(core.process_cycle());
    REQUIRE(fixture.executor.start_requests().size() == 2U);
    CHECK(fixture.executor.start_requests()[0].type == JobType::Cli);
    CHECK(fixture.executor.start_requests()[0].queue_id == first_queue);
    CHECK(fixture.executor.start_requests()[1].type == JobType::Http);
    CHECK(fixture.executor.start_requests()[1].queue_id == second_queue);

    auto const third_queue  = id(3);
    auto const fourth_queue = id(4);
    insert_queue(fixture.database, third_queue, 1);
    insert_queue(fixture.database, fourth_queue, 1);
    insert_scheduled(fixture, third_queue, 30, JobType::Cli);
    insert_scheduled(fixture, third_queue, 31, JobType::Http);
    insert_scheduled(fixture, fourth_queue, 40, JobType::Cli);
    insert_scheduled(fixture, fourth_queue, 41, JobType::Http);

    REQUIRE(core.process_cycle());
    REQUIRE(fixture.executor.start_requests().size() == 4U);
    CHECK(fixture.executor.start_requests()[2].type == JobType::Http);
    CHECK(fixture.executor.start_requests()[2].queue_id == third_queue);
    CHECK(fixture.executor.start_requests()[3].type == JobType::Cli);
    CHECK(fixture.executor.start_requests()[3].queue_id == fourth_queue);
}

TEST_CASE("Scheduler core reconciles suspension and resume by queue UUID", "[jobu][scheduler][core][fairness][sqlite]")
{
    CoreFixture fixture;
    auto const  first_queue  = id(1);
    auto const  second_queue = id(2);
    insert_queue(fixture.database, first_queue, 3, 5);
    insert_queue(fixture.database, second_queue, 3, 1);
    insert_scheduled(fixture, first_queue, 10, JobType::Cli);
    fixture.executor.set_available(JobType::Cli, true);
    SchedulerCore core{
        fixture.database,
        fixture.registry,
        fixture.cron,
        fixture.generator,
        fixture.time,
        fixture.executor,
        {.cli_concurrency = 3, .http_concurrency = 1, .candidate_batch_size = 2}
    };
    ManagementService service{fixture.database, fixture.registry, fixture.cron, fixture.generator, fixture.time};

    REQUIRE(core.process_cycle());
    auto suspended = service.suspend_queue(first_queue);
    REQUIRE(suspended);
    CHECK(suspended->state == QueueState::Suspending);
    insert_scheduled(fixture, first_queue, 11, JobType::Cli);
    insert_scheduled(fixture, second_queue, 20, JobType::Cli);
    REQUIRE(core.process_cycle());
    auto resumed = service.resume_queue(first_queue);
    REQUIRE(resumed);
    CHECK(resumed->state == QueueState::Active);
    insert_scheduled(fixture, second_queue, 21, JobType::Cli);
    REQUIRE(core.process_cycle());

    REQUIRE(fixture.executor.start_requests().size() == 3U);
    CHECK(fixture.executor.start_requests()[0].queue_id == first_queue);
    CHECK(fixture.executor.start_requests()[1].queue_id == second_queue);
    CHECK(fixture.executor.start_requests()[2].queue_id == first_queue);
}

TEST_CASE("Scheduler core observes dynamic queue concurrency", "[jobu][scheduler][core][capacity][sqlite]")
{
    CoreFixture fixture;
    auto const  first_queue  = id(1);
    auto const  second_queue = id(2);
    insert_queue(fixture.database, first_queue, 3);
    insert_queue(fixture.database, second_queue, 3);
    insert_scheduled(fixture, first_queue, 10, JobType::Cli);
    fixture.executor.set_available(JobType::Cli, true);
    SchedulerCore core{
        fixture.database,
        fixture.registry,
        fixture.cron,
        fixture.generator,
        fixture.time,
        fixture.executor,
        {.cli_concurrency = 4, .http_concurrency = 1, .candidate_batch_size = 2}
    };
    ManagementService service{fixture.database, fixture.registry, fixture.cron, fixture.generator, fixture.time};

    REQUIRE(core.process_cycle());
    REQUIRE(service.update_queue({.queue = first_queue, .weight = 1, .concurrency_limit = 1}));
    insert_scheduled(fixture, first_queue, 11, JobType::Cli);
    insert_scheduled(fixture, second_queue, 20, JobType::Cli);
    REQUIRE(core.process_cycle());
    REQUIRE(service.update_queue({.queue = first_queue, .weight = 1, .concurrency_limit = 2}));
    insert_scheduled(fixture, second_queue, 21, JobType::Cli);
    REQUIRE(core.process_cycle());

    REQUIRE(fixture.executor.start_requests().size() == 4U);
    CHECK(fixture.executor.start_requests()[0].queue_id == first_queue);
    CHECK(fixture.executor.start_requests()[1].queue_id == second_queue);
    CHECK(fixture.executor.start_requests()[2].queue_id == first_queue);
    CHECK(fixture.executor.start_requests()[3].queue_id == second_queue);
}

TEST_CASE("Scheduler core completes service-requested suspension drains",
          "[jobu][scheduler][core][suspension][management][sqlite]")
{
    SECTION("a recurring job drains before its successor becomes eligible after resume")
    {
        auto const  schedule     = CronSchedule{.expression = "*/5 * * * *", .timezone = "UTC"};
        auto const  queue_id     = id(1);
        auto const  job_id       = id(2);
        auto const  run_id       = id(3);
        auto const  successor_id = id(4);
        CoreFixture fixture{{successor_id}};
        insert_queue(fixture.database, queue_id, 1);
        insert_job(fixture.database, job_id, queue_id, JobType::Cli, JobState::Active, schedule);
        insert_run(fixture.database, default_run(fixture, run_id, job_id, queue_id, JobType::Cli));
        fixture.cron.set_occurrences(schedule, {at(200), at(300)});
        fixture.executor.set_available(JobType::Cli, true);
        SchedulerCore core{
            fixture.database,
            fixture.registry,
            fixture.cron,
            fixture.generator,
            fixture.time,
            fixture.executor,
            {.cli_concurrency = 1, .http_concurrency = 1, .candidate_batch_size = 1}
        };
        ManagementService service{fixture.database, fixture.registry, fixture.cron, fixture.generator, fixture.time};

        REQUIRE(core.process_cycle());
        REQUIRE(fixture.executor.pending_keys().size() == 1U);
        auto const key = fixture.executor.pending_keys().front();
        fixture.time.set_utc(at(130));
        auto draining = service.suspend_job(job_id);
        REQUIRE(draining);
        CHECK(draining->state == JobState::Suspending);
        CHECK(draining->revision == 2);

        fixture.time.set_utc(at(150));
        REQUIRE(fixture.executor.complete(key, success(key)));
        auto suspended = service.get_job(job_id);
        REQUIRE(suspended);
        CHECK(suspended->state == JobState::Suspended);
        CHECK(suspended->revision == 3);
        CHECK(suspended->updated_at == at(150));
        auto successor = fixture.runs.find_schedule_owned(job_id);
        REQUIRE(successor);
        REQUIRE(successor->has_value());
        CHECK(successor->value().id == successor_id);
        CHECK(successor->value().job_revision == 2);
        CHECK(successor->value().planned_at == at(200));

        fixture.time.set_utc(at(250));
        REQUIRE(core.process_cycle());
        CHECK(fixture.executor.start_requests().size() == 1U);
        auto resumed = service.resume_job(job_id);
        REQUIRE(resumed);
        CHECK(resumed->state == JobState::Active);
        CHECK(resumed->revision == 4);
        REQUIRE(core.process_cycle());
        REQUIRE(fixture.executor.start_requests().size() == 2U);
        CHECK(fixture.executor.start_requests().back().key.run_id == successor_id);
    }

    SECTION("a queue drain gates pending work until resume")
    {
        CoreFixture fixture;
        auto const  queue_id = id(10);
        insert_queue(fixture.database, queue_id, 1);
        insert_scheduled(fixture, queue_id, 11, JobType::Cli);
        insert_scheduled(fixture, queue_id, 12, JobType::Cli);
        fixture.executor.set_available(JobType::Cli, true);
        SchedulerCore core{
            fixture.database,
            fixture.registry,
            fixture.cron,
            fixture.generator,
            fixture.time,
            fixture.executor,
            {.cli_concurrency = 1, .http_concurrency = 1, .candidate_batch_size = 2}
        };
        ManagementService service{fixture.database, fixture.registry, fixture.cron, fixture.generator, fixture.time};

        REQUIRE(core.process_cycle());
        auto const key = fixture.executor.pending_keys().front();
        fixture.time.set_utc(at(130));
        auto draining = service.suspend_queue(queue_id);
        REQUIRE(draining);
        CHECK(draining->state == QueueState::Suspending);

        fixture.time.set_utc(at(140));
        REQUIRE(fixture.executor.complete(key, success(key)));
        auto suspended = service.get_queue(queue_id);
        REQUIRE(suspended);
        CHECK(suspended->state == QueueState::Suspended);
        CHECK(suspended->updated_at == at(140));
        REQUIRE(core.process_cycle());
        CHECK(fixture.executor.start_requests().size() == 1U);

        fixture.time.set_utc(at(150));
        auto resumed = service.resume_queue(queue_id);
        REQUIRE(resumed);
        CHECK(resumed->state == QueueState::Active);
        REQUIRE(core.process_cycle());
        REQUIRE(fixture.executor.start_requests().size() == 2U);
        CHECK(fixture.executor.start_requests().back().key.run_id == id(12));
    }

    SECTION("remaining running work delays its queue while each job drains independently")
    {
        CoreFixture fixture;
        auto const  queue_id   = id(20);
        auto const  first_run  = id(21);
        auto const  second_run = id(22);
        auto const  first_job  = id(121);
        auto const  second_job = id(122);
        insert_queue(fixture.database, queue_id, 2);
        insert_scheduled(fixture, queue_id, 21, JobType::Cli);
        insert_scheduled(fixture, queue_id, 22, JobType::Cli);
        fixture.executor.set_available(JobType::Cli, true);
        SchedulerCore core{
            fixture.database,
            fixture.registry,
            fixture.cron,
            fixture.generator,
            fixture.time,
            fixture.executor,
            {.cli_concurrency = 2, .http_concurrency = 1, .candidate_batch_size = 2}
        };
        ManagementService service{fixture.database, fixture.registry, fixture.cron, fixture.generator, fixture.time};

        REQUIRE(core.process_cycle());
        auto const keys = fixture.executor.pending_keys();
        REQUIRE(keys.size() == 2U);
        auto const first  = std::ranges::find_if(keys, [&](AttemptKey const& key) { return key.run_id == first_run; });
        auto const second = std::ranges::find_if(keys, [&](AttemptKey const& key) { return key.run_id == second_run; });
        REQUIRE(first != keys.end());
        REQUIRE(second != keys.end());
        auto const first_key  = *first;
        auto const second_key = *second;
        fixture.time.set_utc(at(130));
        REQUIRE(service.suspend_job(first_job));
        REQUIRE(service.suspend_queue(queue_id));

        fixture.time.set_utc(at(140));
        REQUIRE(fixture.executor.complete(first_key, success(first_key)));
        auto first_suspended = service.get_job(first_job);
        REQUIRE(first_suspended);
        CHECK(first_suspended->state == JobState::Suspended);
        CHECK(first_suspended->revision == 3);
        auto queue_draining = service.get_queue(queue_id);
        REQUIRE(queue_draining);
        CHECK(queue_draining->state == QueueState::Suspending);

        REQUIRE(service.suspend_job(second_job));
        fixture.time.set_utc(at(150));
        REQUIRE(fixture.executor.complete(second_key, success(second_key)));
        auto second_suspended = service.get_job(second_job);
        REQUIRE(second_suspended);
        CHECK(second_suspended->state == JobState::Suspended);
        CHECK(second_suspended->revision == 3);
        auto queue_suspended = service.get_queue(queue_id);
        REQUIRE(queue_suspended);
        CHECK(queue_suspended->state == QueueState::Suspended);
        CHECK(queue_suspended->updated_at == at(150));
    }
}

TEST_CASE("Single-queue scheduler core enforces global and combined queue limits", "[jobu][scheduler][core][sqlite]")
{
    SECTION("CLI global limit")
    {
        CoreFixture fixture;
        auto const  queue_id = id(30);
        insert_queue(fixture.database, queue_id, 5);
        insert_scheduled(fixture, queue_id, 31, JobType::Cli);
        insert_scheduled(fixture, queue_id, 32, JobType::Cli);
        fixture.executor.set_available(JobType::Cli, true);

        REQUIRE(fixture.process({.cli_concurrency = 1, .http_concurrency = 5, .candidate_batch_size = 2}));
        REQUIRE(fixture.executor.start_requests().size() == 1U);
        CHECK(fixture.executor.start_requests().front().type == JobType::Cli);
    }

    SECTION("HTTP global limit")
    {
        CoreFixture fixture;
        auto const  queue_id = id(40);
        insert_queue(fixture.database, queue_id, 5);
        insert_scheduled(fixture, queue_id, 41, JobType::Http);
        insert_scheduled(fixture, queue_id, 42, JobType::Http);
        fixture.executor.set_available(JobType::Http, true);

        REQUIRE(fixture.process({.cli_concurrency = 5, .http_concurrency = 1, .candidate_batch_size = 2}));
        REQUIRE(fixture.executor.start_requests().size() == 1U);
        CHECK(fixture.executor.start_requests().front().type == JobType::Http);
    }

    SECTION("combined queue limit")
    {
        CoreFixture fixture;
        auto const  queue_id = id(50);
        insert_queue(fixture.database, queue_id, 1);
        insert_scheduled(fixture, queue_id, 51, JobType::Cli);
        insert_scheduled(fixture, queue_id, 52, JobType::Http);
        fixture.executor.set_available(JobType::Cli, true);
        fixture.executor.set_available(JobType::Http, true);

        REQUIRE(fixture.process({.cli_concurrency = 2, .http_concurrency = 2, .candidate_batch_size = 2}));
        REQUIRE(fixture.executor.start_requests().size() == 1U);
        CHECK(fixture.executor.start_requests().front().type == JobType::Cli);
    }
}

TEST_CASE("Single-queue scheduler core reconstructs durable occupancy", "[jobu][scheduler][core][sqlite]")
{
    SECTION("running attempts consume one global and one queue slot")
    {
        CoreFixture fixture;
        auto const  queue_id = id(60);
        insert_queue(fixture.database, queue_id, 2);
        insert_running(fixture, queue_id, 61, JobType::Cli);
        insert_scheduled(fixture, queue_id, 62, JobType::Cli);
        insert_scheduled(fixture, queue_id, 63, JobType::Http);
        fixture.executor.set_available(JobType::Cli, true);
        fixture.executor.set_available(JobType::Http, true);

        REQUIRE(fixture.process({.cli_concurrency = 1, .http_concurrency = 1, .candidate_batch_size = 1}));
        REQUIRE(fixture.executor.start_requests().size() == 1U);
        CHECK(fixture.executor.start_requests().front().type == JobType::Http);
    }

    SECTION("blocking retries consume only the combined queue slot")
    {
        CoreFixture fixture;
        auto const  queue_id = id(70);
        insert_queue(fixture.database, queue_id, 1);
        insert_blocking_retry(fixture, queue_id, 71);
        insert_scheduled(fixture, queue_id, 72, JobType::Cli);
        fixture.executor.set_available(JobType::Cli, true);

        REQUIRE(fixture.process({.cli_concurrency = 2, .http_concurrency = 2, .candidate_batch_size = 1}));
        CHECK(fixture.executor.start_requests().empty());
    }

    SECTION("suspended blocking retries release the combined queue slot")
    {
        CoreFixture fixture;
        auto const  queue_id = id(75);
        insert_queue(fixture.database, queue_id, 1);
        insert_blocking_retry(fixture, queue_id, 76, JobState::Suspended);
        insert_scheduled(fixture, queue_id, 77, JobType::Cli);
        fixture.executor.set_available(JobType::Cli, true);

        REQUIRE(fixture.process({.cli_concurrency = 2, .http_concurrency = 2, .candidate_batch_size = 1}));
        REQUIRE(fixture.executor.start_requests().size() == 1U);
        CHECK(fixture.executor.start_requests().front().key.run_id == id(77));
    }
}

TEST_CASE("Single-queue scheduler core leaves unavailable work pending", "[jobu][scheduler][core][sqlite]")
{
    CoreFixture fixture;
    auto const  queue_id = id(80);
    insert_queue(fixture.database, queue_id, 2);
    insert_scheduled(fixture, queue_id, 81, JobType::Cli);
    insert_scheduled(fixture, queue_id, 82, JobType::Http);
    fixture.executor.set_available(JobType::Http, true);

    REQUIRE(fixture.process({.cli_concurrency = 2, .http_concurrency = 2, .candidate_batch_size = 1}));
    REQUIRE(fixture.executor.start_requests().size() == 1U);
    CHECK(fixture.executor.start_requests().front().key.run_id == id(82));
}

TEST_CASE("Single-queue scheduler core treats dispatch revalidation loss as a normal skip",
          "[jobu][scheduler][core][sqlite]")
{
    CoreFixture fixture;
    auto const  queue_id = id(90);
    insert_queue(fixture.database, queue_id, 2);
    insert_running(fixture, queue_id, 91, JobType::Cli);
    insert_scheduled(fixture, queue_id, 92, JobType::Cli);

    LoweringExecutor executor{fixture.database, queue_id};
    SchedulerCore    core{
        fixture.database,
        fixture.registry,
        fixture.cron,
        fixture.generator,
        fixture.time,
        executor,
        {.cli_concurrency = 2, .http_concurrency = 2, .candidate_batch_size = 2}
    };
    REQUIRE(core.process_cycle());
    CHECK(executor.mutation_ok());
    CHECK(executor.fake.start_requests().empty());
}

TEST_CASE("Scheduler core falls through when the selected queue loses eligibility",
          "[jobu][scheduler][core][fairness][sqlite]")
{
    CoreFixture fixture;
    auto const  first_queue  = id(90);
    auto const  second_queue = id(95);
    insert_queue(fixture.database, first_queue, 2);
    insert_queue(fixture.database, second_queue, 1);
    insert_running(fixture, first_queue, 91, JobType::Cli);
    insert_scheduled(fixture, first_queue, 92, JobType::Cli);
    insert_scheduled(fixture, second_queue, 93, JobType::Cli);

    LoweringExecutor executor{fixture.database, first_queue};
    SchedulerCore    core{
        fixture.database,
        fixture.registry,
        fixture.cron,
        fixture.generator,
        fixture.time,
        executor,
        {.cli_concurrency = 2, .http_concurrency = 2, .candidate_batch_size = 2}
    };
    REQUIRE(core.process_cycle());

    CHECK(executor.mutation_ok());
    REQUIRE(executor.fake.start_requests().size() == 1U);
    CHECK(executor.fake.start_requests().front().queue_id == second_queue);
    CHECK(executor.fake.start_requests().front().key.run_id == id(93));
}

TEST_CASE("Scheduler core commits terminal success before releasing capacity",
          "[jobu][scheduler][core][completion][sqlite]")
{
    CoreFixture fixture;
    auto const  queue_id = id(100);
    insert_queue(fixture.database, queue_id, 1);
    insert_scheduled(fixture, queue_id, 101, JobType::Cli);
    insert_scheduled(fixture, queue_id, 102, JobType::Cli);
    fixture.executor.set_available(JobType::Cli, true);
    SchedulerCore core{
        fixture.database,
        fixture.registry,
        fixture.cron,
        fixture.generator,
        fixture.time,
        fixture.executor,
        {.cli_concurrency = 1, .http_concurrency = 1, .candidate_batch_size = 2}
    };

    REQUIRE(core.process_cycle());
    REQUIRE(fixture.executor.pending_keys().size() == 1U);
    auto const first_key = fixture.executor.pending_keys().front();
    fixture.time.set_utc(at(200));
    REQUIRE(fixture.executor.complete(first_key, success(first_key, "succeeded")));

    auto first_run = fixture.runs.find_by_id(first_key.run_id);
    REQUIRE(first_run);
    REQUIRE(first_run->has_value());
    CHECK(first_run->value().state == RunState::Succeeded);
    CHECK(first_run->value().completed_at == at(200));
    REQUIRE(first_run->value().result);
    CHECK(first_run->value().result->as_object().at("status").as_string() == "succeeded");
    auto first_attempt = fixture.attempts.find(first_key.run_id, first_key.attempt_number);
    REQUIRE(first_attempt);
    REQUIRE(first_attempt->has_value());
    CHECK(first_attempt->value().state == AttemptState::Completed);
    CHECK(first_attempt->value().outcome == AttemptOutcome::Succeeded);
    CHECK(first_attempt->value().completed_at == at(200));
    auto first_output = fixture.attempts.find_output(first_key.run_id, first_key.attempt_number);
    REQUIRE(first_output);
    CHECK_FALSE(first_output->has_value());

    REQUIRE(core.process_cycle());
    REQUIRE(fixture.executor.pending_keys().size() == 1U);
    auto const second_key = fixture.executor.pending_keys().front();
    CHECK(second_key.run_id == id(102));
    fixture.time.set_utc(at(201));
    REQUIRE(fixture.executor.complete(second_key, success(second_key)));
}

TEST_CASE("Scheduler core persists runner output with success retry and terminal completion",
          "[jobu][scheduler][core][completion][output][sqlite]")
{
    enum class CompletionKind : std::uint8_t {
        Success,
        Retry,
        Terminal,
    };

    auto const verify = [](CompletionKind kind) {
        CoreFixture fixture;
        auto const  queue_id = id(103);
        insert_queue(fixture.database, queue_id, 1);
        auto const capture    = std::string{kind == CompletionKind::Success ? "always" : "on_error"};
        auto const attributes = attribute_document(fixture.registry, "reschedule", 3, Duration::zero(), capture);
        insert_scheduled(fixture, queue_id, 104, JobType::Cli, 0, 100, 100, attributes);
        fixture.executor.set_available(JobType::Cli, true);
        SchedulerCore core{
            fixture.database,
            fixture.registry,
            fixture.cron,
            fixture.generator,
            fixture.time,
            fixture.executor,
            {.cli_concurrency = 1, .http_concurrency = 1, .candidate_batch_size = 1}
        };

        REQUIRE(core.process_cycle());
        auto const key = fixture.executor.pending_keys().front();
        auto       completion =
            kind == CompletionKind::Success
                ? success(key)
                : failure(key,
                          kind == CompletionKind::Retry ? FailureDisposition::Retryable : FailureDisposition::Terminal);
        completion.output = captured_output();
        fixture.time.set_utc(at(200));
        REQUIRE(fixture.executor.complete(key, std::move(completion)));

        check_captured_output(fixture.attempts, key);
        auto attempt = fixture.attempts.find(key.run_id, key.attempt_number);
        REQUIRE(attempt);
        REQUIRE(attempt->has_value());
        CHECK(attempt->value().state == AttemptState::Completed);
        auto run = fixture.runs.find_by_id(key.run_id);
        REQUIRE(run);
        REQUIRE(run->has_value());
        if (kind == CompletionKind::Success) {
            CHECK(attempt->value().outcome == AttemptOutcome::Succeeded);
            CHECK(run->value().state == RunState::Succeeded);
        }
        else if (kind == CompletionKind::Retry) {
            CHECK(attempt->value().outcome == AttemptOutcome::Failed);
            CHECK(run->value().state == RunState::RetryWait);
        }
        else {
            CHECK(attempt->value().outcome == AttemptOutcome::Failed);
            CHECK(run->value().state == RunState::Failed);
        }
    };

    SECTION("success")
    {
        verify(CompletionKind::Success);
    }
    SECTION("retry")
    {
        verify(CompletionKind::Retry);
    }
    SECTION("terminal failure")
    {
        verify(CompletionKind::Terminal);
    }
}

TEST_CASE("Scheduler core creates one recurring successor from the newest definition",
          "[jobu][scheduler][core][completion][recurrence][sqlite]")
{
    auto const  original_schedule = CronSchedule{.expression = "0 * * * *", .timezone = "UTC"};
    auto const  latest_schedule   = CronSchedule{.expression = "30 * * * *", .timezone = "Europe/Tallinn"};
    auto const  queue_id          = id(150);
    auto const  job_id            = id(151);
    auto const  run_id            = id(152);
    auto const  successor_id      = id(153);
    CoreFixture fixture{{successor_id}};
    insert_queue(fixture.database, queue_id, 1);
    insert_job(fixture.database, job_id, queue_id, JobType::Cli, JobState::Active, original_schedule);
    auto run = default_run(fixture, run_id, job_id, queue_id, JobType::Cli);
    insert_run(fixture.database, run);
    fixture.cron.set_occurrences(latest_schedule, {at(150), at(250), at(350)});
    fixture.executor.set_available(JobType::Cli, true);
    fixture.executor.set_available(JobType::Http, true);
    SchedulerCore core{
        fixture.database,
        fixture.registry,
        fixture.cron,
        fixture.generator,
        fixture.time,
        fixture.executor,
        {.cli_concurrency = 1, .http_concurrency = 1, .candidate_batch_size = 1}
    };

    REQUIRE(core.process_cycle());
    REQUIRE(fixture.executor.pending_keys().size() == 1U);
    auto const key               = fixture.executor.pending_keys().front();
    auto const latest_attributes = attribute_document(fixture.registry, "blocking", 5);
    update_recurring_job(fixture.database,
                         job_id,
                         2,
                         latest_schedule,
                         JobType::Http,
                         42,
                         latest_attributes,
                         R"({"url":"https://updated.example"})");

    fixture.time.set_utc(at(200));
    REQUIRE(fixture.executor.complete(key, success(key)));

    auto completed = fixture.runs.find_by_id(run_id);
    REQUIRE(completed);
    REQUIRE(completed->has_value());
    CHECK(completed->value().state == RunState::Succeeded);
    auto successor = fixture.runs.find_schedule_owned(job_id);
    REQUIRE(successor);
    REQUIRE(successor->has_value());
    CHECK(successor->value().id == successor_id);
    CHECK(successor->value().job_revision == 2);
    CHECK(successor->value().queue_id == queue_id);
    CHECK(successor->value().planned_at == at(250));
    CHECK(successor->value().runnable_at == at(250));
    CHECK(successor->value().type == JobType::Http);
    CHECK(successor->value().priority == 42);
    CHECK(std::get<std::int64_t>(successor->value().attributes.at("retry.max_attempts").data) == 5);
    CHECK(std::get<std::string>(successor->value().attributes.at("retry.mode").data) == "blocking");
    CHECK(successor->value().payload.as_object().at("url").as_string() == "https://updated.example");
    REQUIRE(fixture.cron.validation_calls().size() == 1U);
    CHECK(fixture.cron.validation_calls().front().expression == latest_schedule.expression);
    REQUIRE(fixture.cron.next_calls().size() == 1U);
    CHECK(fixture.cron.next_calls().front().exclusive_lower_bound == at(200));

    fixture.time.set_utc(at(250));
    REQUIRE(core.process_cycle());
    REQUIRE(fixture.executor.pending_keys().size() == 1U);
    CHECK(fixture.executor.pending_keys().front().run_id == successor_id);
}

TEST_CASE("Scheduler core uses the later planned time as the recurring cancellation lower bound",
          "[jobu][scheduler][core][completion][recurrence][sqlite]")
{
    auto const  schedule     = CronSchedule{.expression = "*/5 * * * *", .timezone = "UTC"};
    auto const  queue_id     = id(155);
    auto const  job_id       = id(156);
    auto const  run_id       = id(157);
    auto const  successor_id = id(158);
    CoreFixture fixture{{successor_id}};
    insert_queue(fixture.database, queue_id, 1);
    insert_job(fixture.database, job_id, queue_id, JobType::Cli, JobState::Active, schedule);
    auto run        = default_run(fixture, run_id, job_id, queue_id, JobType::Cli);
    run.planned_at  = at(150);
    run.runnable_at = at(100);
    insert_run(fixture.database, run);
    fixture.cron.set_occurrences(schedule, {at(140), at(160), at(200)});
    fixture.executor.set_available(JobType::Cli, true);
    SchedulerCore core{
        fixture.database,
        fixture.registry,
        fixture.cron,
        fixture.generator,
        fixture.time,
        fixture.executor,
        {.cli_concurrency = 1, .http_concurrency = 1, .candidate_batch_size = 1}
    };

    REQUIRE(core.process_cycle());
    auto const key = fixture.executor.pending_keys().front();
    fixture.time.set_utc(at(130));
    REQUIRE(fixture.executor.complete(key, cancelled(key)));

    auto completed = fixture.runs.find_by_id(run_id);
    REQUIRE(completed);
    REQUIRE(completed->has_value());
    CHECK(completed->value().state == RunState::Cancelled);
    auto successor = fixture.runs.find_schedule_owned(job_id);
    REQUIRE(successor);
    REQUIRE(successor->has_value());
    CHECK(successor->value().id == successor_id);
    CHECK(successor->value().planned_at == at(160));
    REQUIRE(fixture.cron.next_calls().size() == 1U);
    CHECK(fixture.cron.next_calls().front().exclusive_lower_bound == at(150));
}

TEST_CASE("Scheduler core omits recurring successors for excluded terminal work",
          "[jobu][scheduler][core][completion][recurrence][sqlite]")
{
    SECTION("one-time definition")
    {
        CoreFixture fixture;
        auto const  queue_id = id(160);
        auto const  job_id   = id(161);
        auto const  run_id   = id(162);
        insert_queue(fixture.database, queue_id, 1);
        insert_job(fixture.database, job_id, queue_id, JobType::Cli);
        insert_run(fixture.database, default_run(fixture, run_id, job_id, queue_id, JobType::Cli));
        fixture.executor.set_available(JobType::Cli, true);
        SchedulerCore core{
            fixture.database,
            fixture.registry,
            fixture.cron,
            fixture.generator,
            fixture.time,
            fixture.executor,
            {.cli_concurrency = 1, .http_concurrency = 1, .candidate_batch_size = 1}
        };

        REQUIRE(core.process_cycle());
        auto const key = fixture.executor.pending_keys().front();
        REQUIRE(fixture.executor.complete(key, success(key)));
        auto successor = fixture.runs.find_schedule_owned(job_id);
        REQUIRE(successor);
        CHECK_FALSE(successor->has_value());
        CHECK(fixture.cron.validation_calls().empty());
    }

    SECTION("manual run")
    {
        CoreFixture fixture;
        auto const  schedule         = CronSchedule{.expression = "*/5 * * * *", .timezone = "UTC"};
        auto const  queue_id         = id(165);
        auto const  job_id           = id(166);
        auto const  run_id           = id(167);
        auto const  scheduled_run_id = id(168);
        insert_queue(fixture.database, queue_id, 1);
        insert_job(fixture.database, job_id, queue_id, JobType::Cli, JobState::Active, schedule);
        auto scheduled_run        = default_run(fixture, scheduled_run_id, job_id, queue_id, JobType::Cli);
        scheduled_run.planned_at  = at(300);
        scheduled_run.runnable_at = at(300);
        insert_run(fixture.database, scheduled_run);
        auto run           = default_run(fixture, run_id, job_id, queue_id, JobType::Cli);
        run.origin         = RunOrigin::Manual;
        run.schedule_owned = false;
        insert_run(fixture.database, run);
        fixture.executor.set_available(JobType::Cli, true);
        SchedulerCore core{
            fixture.database,
            fixture.registry,
            fixture.cron,
            fixture.generator,
            fixture.time,
            fixture.executor,
            {.cli_concurrency = 1, .http_concurrency = 1, .candidate_batch_size = 1}
        };

        REQUIRE(core.process_cycle());
        auto const key = fixture.executor.pending_keys().front();
        REQUIRE(fixture.executor.complete(key, success(key)));
        auto successor = fixture.runs.find_schedule_owned(job_id);
        REQUIRE(successor);
        REQUIRE(successor->has_value());
        CHECK(successor->value().id == scheduled_run_id);
        CHECK(successor->value().planned_at == at(300));
        CHECK(fixture.cron.validation_calls().empty());
    }

    SECTION("deleted definition")
    {
        CoreFixture fixture;
        auto const  schedule = CronSchedule{.expression = "*/5 * * * *", .timezone = "UTC"};
        auto const  queue_id = id(170);
        auto const  job_id   = id(171);
        auto const  run_id   = id(172);
        insert_queue(fixture.database, queue_id, 1);
        insert_job(fixture.database, job_id, queue_id, JobType::Cli, JobState::Active, schedule);
        insert_run(fixture.database, default_run(fixture, run_id, job_id, queue_id, JobType::Cli));
        fixture.executor.set_available(JobType::Cli, true);
        SchedulerCore core{
            fixture.database,
            fixture.registry,
            fixture.cron,
            fixture.generator,
            fixture.time,
            fixture.executor,
            {.cli_concurrency = 1, .http_concurrency = 1, .candidate_batch_size = 1}
        };

        REQUIRE(core.process_cycle());
        auto const key = fixture.executor.pending_keys().front();
        mark_job_deleted(fixture.database, job_id);
        REQUIRE(fixture.executor.complete(key, success(key)));
        auto successor = fixture.runs.find_schedule_owned(job_id);
        REQUIRE(successor);
        CHECK_FALSE(successor->has_value());
        CHECK(fixture.cron.validation_calls().empty());
        REQUIRE(core.process_cycle());
    }
}

TEST_CASE("Scheduler core rolls recurring successor failures back and fails closed",
          "[jobu][scheduler][core][completion][recurrence][rollback][sqlite]")
{
    enum class FailureKind : std::uint8_t {
        CronValidation,
        CronNext,
        Uuid,
        ScheduleConflict,
    };

    auto const verify_failure = [](FailureKind kind) {
        auto        successor_ids = kind == FailureKind::Uuid ? std::vector<Uuid>{} : std::vector<Uuid>{id(184)};
        CoreFixture fixture{std::move(successor_ids)};
        auto const  schedule = CronSchedule{.expression = "*/5 * * * *", .timezone = "UTC"};
        auto const  queue_id = id(180);
        auto const  job_id   = id(181);
        auto const  run_id   = id(182);
        insert_queue(fixture.database, queue_id, 1);
        insert_job(fixture.database, job_id, queue_id, JobType::Cli, JobState::Active, schedule);
        insert_run(fixture.database, default_run(fixture, run_id, job_id, queue_id, JobType::Cli));
        fixture.cron.set_occurrences(schedule, {at(200)});
        fixture.executor.set_available(JobType::Cli, true);

        auto expected_code = std::string{};
        if (kind == FailureKind::CronValidation) {
            expected_code = "test.cron.validation_failed";
            fixture.cron.set_validation_error(Error{
                .category = ErrorCategory::InvalidArgument,
                .code     = expected_code,
                .message  = "Configured cron validation failure",
            });
        }
        else if (kind == FailureKind::CronNext) {
            expected_code = "test.cron.next_failed";
            fixture.cron.set_next_error(Error{
                .category = ErrorCategory::ResourceExhausted,
                .code     = expected_code,
                .message  = "Configured cron next-occurrence failure",
            });
        }
        else if (kind == FailureKind::Uuid) {
            expected_code = "test.uuid.sequence_exhausted";
        }
        else {
            expected_code = "jobu.run.schedule_conflict";
        }

        SchedulerCore core{
            fixture.database,
            fixture.registry,
            fixture.cron,
            fixture.generator,
            fixture.time,
            fixture.executor,
            {.cli_concurrency = 1, .http_concurrency = 1, .candidate_batch_size = 1}
        };
        REQUIRE(core.process_cycle());
        auto const key = fixture.executor.pending_keys().front();
        if (kind == FailureKind::ScheduleConflict) {
            Query trigger{fixture.database};
            REQUIRE(trigger.exec(
                "CREATE TRIGGER inject_recurring_successor_conflict AFTER UPDATE OF state ON jobu_runs "
                "WHEN OLD.state = 'running' AND NEW.state = 'succeeded' BEGIN "
                "INSERT INTO jobu_runs(id, job_id, job_revision, queue_id, origin, schedule_owned, planned_at_us, "
                "runnable_at_us, started_at_us, completed_at_us, type, priority, attributes_json, payload_json, "
                "state, result_json) VALUES(zeroblob(16), NEW.job_id, NEW.job_revision, NEW.queue_id, 'scheduled', "
                "1, NEW.completed_at_us + 1, NEW.completed_at_us + 1, NULL, NULL, NEW.type, NEW.priority, "
                "NEW.attributes_json, NEW.payload_json, 'scheduled', NULL); END"));
        }

        fixture.time.set_utc(at(150));
        REQUIRE(fixture.executor.complete(key, success(key)));
        auto failed = core.process_cycle();
        REQUIRE_FALSE(failed);
        CHECK(failed.error().code == expected_code);
        auto run = fixture.runs.find_by_id(run_id);
        REQUIRE(run);
        REQUIRE(run->has_value());
        CHECK(run->value().state == RunState::Running);
        CHECK_FALSE(run->value().completed_at);
        auto attempt = fixture.attempts.find(run_id, key.attempt_number);
        REQUIRE(attempt);
        REQUIRE(attempt->has_value());
        CHECK(attempt->value().state == AttemptState::Running);
        auto current = fixture.runs.find_schedule_owned(job_id);
        REQUIRE(current);
        REQUIRE(current->has_value());
        CHECK(current->value().id == run_id);
    };

    SECTION("cron validation")
    {
        verify_failure(FailureKind::CronValidation);
    }
    SECTION("cron next occurrence")
    {
        verify_failure(FailureKind::CronNext);
    }
    SECTION("UUID exhaustion")
    {
        verify_failure(FailureKind::Uuid);
    }
    SECTION("schedule-owned uniqueness conflict")
    {
        verify_failure(FailureKind::ScheduleConflict);
    }
}

TEST_CASE("Scheduler core retries the same run and terminally exhausts its policy",
          "[jobu][scheduler][core][completion][retry][sqlite]")
{
    CoreFixture fixture;
    auto const  queue_id = id(105);
    insert_queue(fixture.database, queue_id, 1);
    auto const retry_attributes =
        attribute_document(fixture.registry, "reschedule", 2, std::chrono::duration_cast<Duration>(10us));
    insert_scheduled(fixture, queue_id, 106, JobType::Cli, 0, 100, 100, retry_attributes);
    fixture.executor.set_available(JobType::Cli, true);
    SchedulerCore core{
        fixture.database,
        fixture.registry,
        fixture.cron,
        fixture.generator,
        fixture.time,
        fixture.executor,
        {.cli_concurrency = 1, .http_concurrency = 1, .candidate_batch_size = 1}
    };

    REQUIRE(core.process_cycle());
    auto const first_key = fixture.executor.pending_keys().front();
    fixture.time.set_utc(at(200));
    REQUIRE(fixture.executor.complete(first_key, failure(first_key, FailureDisposition::Retryable)));

    auto waiting = fixture.runs.find_by_id(first_key.run_id);
    REQUIRE(waiting);
    REQUIRE(waiting->has_value());
    CHECK(waiting->value().state == RunState::RetryWait);
    CHECK(waiting->value().runnable_at == at(210));
    CHECK_FALSE(waiting->value().completed_at);
    CHECK_FALSE(waiting->value().result);

    fixture.time.set_utc(at(209));
    REQUIRE(core.process_cycle());
    CHECK(fixture.executor.pending_keys().empty());
    fixture.time.set_utc(at(210));
    REQUIRE(core.process_cycle());
    REQUIRE(fixture.executor.pending_keys().size() == 1U);
    auto const second_key = fixture.executor.pending_keys().front();
    CHECK(second_key.run_id == first_key.run_id);
    CHECK(second_key.attempt_number == 2U);

    fixture.time.set_utc(at(220));
    REQUIRE(fixture.executor.complete(second_key, failure(second_key, FailureDisposition::Retryable)));
    auto terminal = fixture.runs.find_by_id(first_key.run_id);
    REQUIRE(terminal);
    REQUIRE(terminal->has_value());
    CHECK(terminal->value().state == RunState::Failed);
    CHECK(terminal->value().completed_at == at(220));
    auto attempts = fixture.attempts.list_for_run(first_key.run_id, 10);
    REQUIRE(attempts);
    REQUIRE(attempts->size() == 2U);
    CHECK((*attempts)[0].outcome == AttemptOutcome::Failed);
    CHECK((*attempts)[1].outcome == AttemptOutcome::Failed);
}

TEST_CASE("Scheduler core combines policy delays with executor retry deadlines",
          "[jobu][scheduler][core][completion][retry][sqlite]")
{
    for (auto const [deadline, expected] : std::vector<std::pair<UtcTimePoint, UtcTimePoint>>{
             {at(205), at(210)},
             {at(250), at(250)},
    }) {
        CoreFixture fixture;
        auto const  queue_id = id(110);
        insert_queue(fixture.database, queue_id, 1);
        auto const retry_attributes =
            attribute_document(fixture.registry, "reschedule", 3, std::chrono::duration_cast<Duration>(10us));
        insert_scheduled(fixture, queue_id, 111, JobType::Cli, 0, 100, 100, retry_attributes);
        fixture.executor.set_available(JobType::Cli, true);
        SchedulerCore core{
            fixture.database,
            fixture.registry,
            fixture.cron,
            fixture.generator,
            fixture.time,
            fixture.executor,
            {.cli_concurrency = 1, .http_concurrency = 1, .candidate_batch_size = 1}
        };

        REQUIRE(core.process_cycle());
        auto const key = fixture.executor.pending_keys().front();
        fixture.time.set_utc(at(200));
        REQUIRE(fixture.executor.complete(key, failure(key, FailureDisposition::Retryable, deadline)));
        auto run = fixture.runs.find_by_id(key.run_id);
        REQUIRE(run);
        REQUIRE(run->has_value());
        CHECK(run->value().state == RunState::RetryWait);
        CHECK(run->value().runnable_at == expected);
    }
}

TEST_CASE("Scheduler core accounts for blocking and rescheduled retry waits",
          "[jobu][scheduler][core][completion][retry][capacity][sqlite]")
{
    for (auto const& mode : {std::string{"blocking"}, std::string{"reschedule"}}) {
        CoreFixture fixture;
        auto const  queue_id = id(115);
        insert_queue(fixture.database, queue_id, 1);
        auto const retry_attributes =
            attribute_document(fixture.registry, mode, 3, std::chrono::duration_cast<Duration>(100us));
        insert_scheduled(fixture, queue_id, 116, JobType::Cli, 1, 100, 100, retry_attributes);
        insert_scheduled(fixture, queue_id, 117, JobType::Cli);
        fixture.executor.set_available(JobType::Cli, true);
        SchedulerCore core{
            fixture.database,
            fixture.registry,
            fixture.cron,
            fixture.generator,
            fixture.time,
            fixture.executor,
            {.cli_concurrency = 1, .http_concurrency = 1, .candidate_batch_size = 1}
        };

        REQUIRE(core.process_cycle());
        auto const first_key = fixture.executor.pending_keys().front();
        CHECK(first_key.run_id == id(116));
        fixture.time.set_utc(at(200));
        REQUIRE(fixture.executor.complete(first_key, failure(first_key, FailureDisposition::Retryable)));

        fixture.time.set_utc(at(250));
        REQUIRE(core.process_cycle());
        if (mode == "blocking") {
            CHECK(fixture.executor.pending_keys().empty());
        }
        else {
            REQUIRE(fixture.executor.pending_keys().size() == 1U);
            auto const other_key = fixture.executor.pending_keys().front();
            CHECK(other_key.run_id == id(117));
            fixture.time.set_utc(at(251));
            REQUIRE(fixture.executor.complete(other_key, success(other_key)));
        }

        fixture.time.set_utc(at(300));
        REQUIRE(core.process_cycle());
        REQUIRE(fixture.executor.pending_keys().size() == 1U);
        auto const retry_key = fixture.executor.pending_keys().front();
        CHECK(retry_key.run_id == first_key.run_id);
        CHECK(retry_key.attempt_number == 2U);
        fixture.time.set_utc(at(301));
        REQUIRE(fixture.executor.complete(retry_key, success(retry_key)));

        if (mode == "blocking") {
            REQUIRE(core.process_cycle());
            REQUIRE(fixture.executor.pending_keys().size() == 1U);
            auto const other_key = fixture.executor.pending_keys().front();
            CHECK(other_key.run_id == id(117));
            fixture.time.set_utc(at(302));
            REQUIRE(fixture.executor.complete(other_key, success(other_key)));
        }
    }
}

TEST_CASE("Scheduler core releases and reacquires blocking retry occupancy across job suspension",
          "[jobu][scheduler][core][completion][retry][capacity][suspension][management][sqlite]")
{
    CoreFixture fixture;
    auto const  queue_id     = id(1);
    auto const  retry_job_id = id(110);
    insert_queue(fixture.database, queue_id, 1);
    auto const retry_attributes =
        attribute_document(fixture.registry, "blocking", 3, std::chrono::duration_cast<Duration>(100us));
    insert_scheduled(fixture, queue_id, 10, JobType::Cli, 10, 100, 100, retry_attributes);
    insert_scheduled(fixture, queue_id, 11, JobType::Cli, 5);
    insert_scheduled(fixture, queue_id, 12, JobType::Cli);
    fixture.executor.set_available(JobType::Cli, true);
    SchedulerCore core{
        fixture.database,
        fixture.registry,
        fixture.cron,
        fixture.generator,
        fixture.time,
        fixture.executor,
        {.cli_concurrency = 1, .http_concurrency = 1, .candidate_batch_size = 2}
    };
    ManagementService service{fixture.database, fixture.registry, fixture.cron, fixture.generator, fixture.time};

    REQUIRE(core.process_cycle());
    auto const first_key = fixture.executor.pending_keys().front();
    CHECK(first_key.run_id == id(10));
    fixture.time.set_utc(at(130));
    auto draining = service.suspend_job(retry_job_id);
    REQUIRE(draining);
    CHECK(draining->state == JobState::Suspending);

    fixture.time.set_utc(at(200));
    REQUIRE(fixture.executor.complete(first_key, failure(first_key, FailureDisposition::Retryable)));
    auto suspended = service.get_job(retry_job_id);
    REQUIRE(suspended);
    CHECK(suspended->state == JobState::Suspended);
    auto retry_wait = fixture.runs.find_by_id(first_key.run_id);
    REQUIRE(retry_wait);
    REQUIRE(retry_wait->has_value());
    CHECK(retry_wait->value().state == RunState::RetryWait);
    CHECK(retry_wait->value().runnable_at == at(300));

    fixture.time.set_utc(at(250));
    REQUIRE(core.process_cycle());
    REQUIRE(fixture.executor.pending_keys().size() == 1U);
    auto const second_key = fixture.executor.pending_keys().front();
    CHECK(second_key.run_id == id(11));
    fixture.time.set_utc(at(251));
    REQUIRE(fixture.executor.complete(second_key, success(second_key)));

    fixture.time.set_utc(at(260));
    auto resumed = service.resume_job(retry_job_id);
    REQUIRE(resumed);
    CHECK(resumed->state == JobState::Active);
    REQUIRE(core.process_cycle());
    CHECK(fixture.executor.start_requests().size() == 2U);

    fixture.time.set_utc(at(300));
    REQUIRE(core.process_cycle());
    REQUIRE(fixture.executor.pending_keys().size() == 1U);
    auto const retry_key = fixture.executor.pending_keys().front();
    CHECK(retry_key.run_id == first_key.run_id);
    CHECK(retry_key.attempt_number == 2U);
    fixture.time.set_utc(at(301));
    REQUIRE(fixture.executor.complete(retry_key, success(retry_key)));

    REQUIRE(core.process_cycle());
    REQUIRE(fixture.executor.pending_keys().size() == 1U);
    CHECK(fixture.executor.pending_keys().front().run_id == id(12));
}

TEST_CASE("Scheduler core completes executor start errors through the normal terminal path",
          "[jobu][scheduler][core][completion][start-error][sqlite]")
{
    CoreFixture fixture;
    auto const  queue_id = id(120);
    insert_queue(fixture.database, queue_id, 1);
    insert_scheduled(fixture, queue_id, 121, JobType::Cli);
    insert_scheduled(fixture, queue_id, 122, JobType::Cli);
    fixture.executor.set_available(JobType::Cli, true);
    fixture.executor.set_start_error(Error{
        .category = ErrorCategory::Unavailable,
        .code     = "test.executor.start_failed",
        .message  = "Configured start failure",
    });
    SchedulerCore core{
        fixture.database,
        fixture.registry,
        fixture.cron,
        fixture.generator,
        fixture.time,
        fixture.executor,
        {.cli_concurrency = 1, .http_concurrency = 1, .candidate_batch_size = 2}
    };

    REQUIRE(core.process_cycle());
    REQUIRE(fixture.executor.start_requests().size() == 2U);
    CHECK(fixture.executor.pending_keys().empty());
    for (auto const run_id : {id(121), id(122)}) {
        auto run = fixture.runs.find_by_id(run_id);
        REQUIRE(run);
        REQUIRE(run->has_value());
        CHECK(run->value().state == RunState::Failed);
        REQUIRE(run->value().result);
        CHECK(run->value().result->as_object().at("error_code").as_string() == "test.executor.start_failed");
        auto attempts = fixture.attempts.list_for_run(run_id, 10);
        REQUIRE(attempts);
        REQUIRE(attempts->size() == 1U);
        CHECK(attempts->front().state == AttemptState::Completed);
        CHECK(attempts->front().outcome == AttemptOutcome::Failed);
    }
}

TEST_CASE("Scheduler core rejects invalid executor completion protocol and fails closed",
          "[jobu][scheduler][core][completion][protocol][sqlite]")
{
    SECTION("completion key mismatch")
    {
        CoreFixture        fixture;
        RawAttemptExecutor executor;
        auto const         queue_id = id(125);
        insert_queue(fixture.database, queue_id, 2);
        insert_scheduled(fixture, queue_id, 126, JobType::Cli);
        SchedulerCore core{
            fixture.database,
            fixture.registry,
            fixture.cron,
            fixture.generator,
            fixture.time,
            executor,
            {.cli_concurrency = 1, .http_concurrency = 1, .candidate_batch_size = 1}
        };

        REQUIRE(core.process_cycle());
        REQUIRE(executor.requests.size() == 1U);
        auto invalid       = success(executor.requests.front().key);
        invalid.key.run_id = id(127);
        executor.emit(std::move(invalid));
        auto failed = core.process_cycle();
        REQUIRE_FALSE(failed);
        CHECK(failed.error().code == "jobu.executor.invalid_completion");
        auto attempt = fixture.attempts.find(id(126), 1);
        REQUIRE(attempt);
        REQUIRE(attempt->has_value());
        CHECK(attempt->value().state == AttemptState::Running);
    }

    SECTION("non-object result")
    {
        CoreFixture        fixture;
        RawAttemptExecutor executor;
        auto const         queue_id = id(130);
        insert_queue(fixture.database, queue_id, 2);
        insert_scheduled(fixture, queue_id, 131, JobType::Cli);
        SchedulerCore core{
            fixture.database,
            fixture.registry,
            fixture.cron,
            fixture.generator,
            fixture.time,
            executor,
            {.cli_concurrency = 1, .http_concurrency = 1, .candidate_batch_size = 1}
        };

        REQUIRE(core.process_cycle());
        auto invalid        = success(executor.requests.front().key);
        invalid.result.data = std::string{"not-an-object"};
        executor.emit(std::move(invalid));
        auto failed = core.process_cycle();
        REQUIRE_FALSE(failed);
        CHECK(failed.error().code == "jobu.executor.invalid_completion");
    }

    SECTION("oversized result")
    {
        CoreFixture        fixture;
        RawAttemptExecutor executor;
        auto const         queue_id = id(132);
        insert_queue(fixture.database, queue_id, 2);
        insert_scheduled(fixture, queue_id, 133, JobType::Cli);
        SchedulerCore core{
            fixture.database,
            fixture.registry,
            fixture.cron,
            fixture.generator,
            fixture.time,
            executor,
            {.cli_concurrency = 1, .http_concurrency = 1, .candidate_batch_size = 1}
        };

        REQUIRE(core.process_cycle());
        auto invalid                                                       = success(executor.requests.front().key);
        std::get<JsonValue::Object>(invalid.result.data).at("status").data = std::string(std::size_t{256} * 1024U, 'x');
        executor.emit(std::move(invalid));
        auto failed = core.process_cycle();
        REQUIRE_FALSE(failed);
        CHECK(failed.error().code == "jobu.executor.invalid_completion");
    }

    SECTION("invalid output invariants")
    {
        CoreFixture        fixture;
        RawAttemptExecutor executor;
        auto const         queue_id = id(137);
        insert_queue(fixture.database, queue_id, 1);
        insert_scheduled(fixture, queue_id, 138, JobType::Cli);
        SchedulerCore core{
            fixture.database,
            fixture.registry,
            fixture.cron,
            fixture.generator,
            fixture.time,
            executor,
            {.cli_concurrency = 1, .http_concurrency = 1, .candidate_batch_size = 1}
        };

        REQUIRE(core.process_cycle());
        auto const key             = executor.requests.front().key;
        auto       invalid         = success(key);
        auto       invalid_channel = jb::jobu::AttemptOutputChannel{
            .bytes       = ByteBuffer{std::byte{0x01}, std::byte{0x02}},
            .total_bytes = 1,
            .truncated   = false,
        };
        invalid.output = jb::jobu::AttemptOutput{
            .primary = std::move(invalid_channel),
        };
        executor.emit(std::move(invalid));

        auto failed = core.process_cycle();
        REQUIRE_FALSE(failed);
        CHECK(failed.error().code == "jobu.executor.invalid_completion");
        CHECK(failed.error().detail == "reason=primary_total_below_retained");
        auto attempt = fixture.attempts.find(key.run_id, key.attempt_number);
        REQUIRE(attempt);
        REQUIRE(attempt->has_value());
        CHECK(attempt->value().state == AttemptState::Running);
    }

    SECTION("disabled output capture")
    {
        CoreFixture        fixture;
        RawAttemptExecutor executor;
        auto const         queue_id = id(139);
        insert_queue(fixture.database, queue_id, 1);
        auto const attributes = attribute_document(fixture.registry, "reschedule", 3, Duration::zero(), "none");
        insert_scheduled(fixture, queue_id, 140, JobType::Cli, 0, 100, 100, attributes);
        SchedulerCore core{
            fixture.database,
            fixture.registry,
            fixture.cron,
            fixture.generator,
            fixture.time,
            executor,
            {.cli_concurrency = 1, .http_concurrency = 1, .candidate_batch_size = 1}
        };

        REQUIRE(core.process_cycle());
        auto const key     = executor.requests.front().key;
        auto       invalid = success(key);
        invalid.output     = captured_output();
        executor.emit(std::move(invalid));

        auto failed = core.process_cycle();
        REQUIRE_FALSE(failed);
        CHECK(failed.error().code == "jobu.executor.invalid_completion");
        CHECK(failed.error().detail == "reason=output_capture_disabled");
        auto attempt = fixture.attempts.find(key.run_id, key.attempt_number);
        REQUIRE(attempt);
        REQUIRE(attempt->has_value());
        CHECK(attempt->value().state == AttemptState::Running);
        auto output = fixture.attempts.find_output(key.run_id, key.attempt_number);
        REQUIRE(output);
        CHECK_FALSE(output->has_value());
    }
}

TEST_CASE("Scheduler core rejects completion invoked synchronously from start",
          "[jobu][scheduler][core][completion][protocol][sqlite]")
{
    CoreFixture                   fixture;
    SynchronousCompletionExecutor executor;
    auto const                    queue_id = id(135);
    insert_queue(fixture.database, queue_id, 1);
    insert_scheduled(fixture, queue_id, 136, JobType::Cli);
    SchedulerCore core{
        fixture.database,
        fixture.registry,
        fixture.cron,
        fixture.generator,
        fixture.time,
        executor,
        {.cli_concurrency = 1, .http_concurrency = 1, .candidate_batch_size = 1}
    };

    auto failed = core.process_cycle();
    REQUIRE_FALSE(failed);
    CHECK(failed.error().code == "jobu.executor.invalid_completion");
    auto attempt = fixture.attempts.find(id(136), 1);
    REQUIRE(attempt);
    REQUIRE(attempt->has_value());
    CHECK(attempt->value().state == AttemptState::Running);
}

TEST_CASE("Scheduler core rolls completion writes back and stops later dispatch after persistence failure",
          "[jobu][scheduler][core][completion][rollback][sqlite]")
{
    CoreFixture fixture;
    auto const  queue_id = id(140);
    insert_queue(fixture.database, queue_id, 2);
    insert_scheduled(fixture, queue_id, 141, JobType::Cli);
    fixture.executor.set_available(JobType::Cli, true);
    SchedulerCore core{
        fixture.database,
        fixture.registry,
        fixture.cron,
        fixture.generator,
        fixture.time,
        fixture.executor,
        {.cli_concurrency = 1, .http_concurrency = 1, .candidate_batch_size = 2}
    };

    REQUIRE(core.process_cycle());
    auto const first_key = fixture.executor.pending_keys().front();
    insert_scheduled(fixture, queue_id, 142, JobType::Cli);
    Query trigger{fixture.database};
    REQUIRE(trigger.exec("CREATE TRIGGER fail_scheduler_completion BEFORE UPDATE OF state ON jobu_runs "
                         "WHEN NEW.state IN ('succeeded', 'failed', 'retry_wait') "
                         "BEGIN SELECT RAISE(ABORT, 'injected scheduler completion failure'); END"));

    fixture.time.set_utc(at(200));
    REQUIRE(fixture.executor.complete(first_key, success(first_key)));
    auto attempt = fixture.attempts.find(first_key.run_id, first_key.attempt_number);
    REQUIRE(attempt);
    REQUIRE(attempt->has_value());
    CHECK(attempt->value().state == AttemptState::Running);
    auto run = fixture.runs.find_by_id(first_key.run_id);
    REQUIRE(run);
    REQUIRE(run->has_value());
    CHECK(run->value().state == RunState::Running);

    auto failed = core.process_cycle();
    REQUIRE_FALSE(failed);
    CHECK(fixture.executor.start_requests().size() == 1U);
}

TEST_CASE("Scheduler core rolls output writes back and retains capacity on failure",
          "[jobu][scheduler][core][completion][output][rollback][capacity][sqlite]")
{
    CoreFixture fixture;
    auto const  queue_id = id(143);
    insert_queue(fixture.database, queue_id, 2);
    auto const attributes = attribute_document(fixture.registry, "reschedule", 3, Duration::zero(), "always");
    insert_scheduled(fixture, queue_id, 144, JobType::Cli, 0, 100, 100, attributes);
    insert_scheduled(fixture, queue_id, 145, JobType::Cli, 0, 100, 100, attributes);
    fixture.executor.set_available(JobType::Cli, true);
    auto          reported = std::vector<Error>{};
    SchedulerCore core{
        fixture.database,
        fixture.registry,
        fixture.cron,
        fixture.generator,
        fixture.time,
        fixture.executor,
        {.cli_concurrency = 1, .http_concurrency = 1, .candidate_batch_size = 2},
        {.failure_reported = [&](Error const& error) { reported.push_back(error); }}
    };

    REQUIRE(core.process_cycle());
    auto const key = fixture.executor.pending_keys().front();
    Query      trigger{fixture.database};
    REQUIRE(trigger.exec("CREATE TRIGGER fail_attempt_output BEFORE INSERT ON jobu_attempt_output "
                         "BEGIN SELECT RAISE(ABORT, 'injected output failure'); END"));

    auto completion   = success(key);
    completion.output = captured_output();
    fixture.time.set_utc(at(200));
    REQUIRE(fixture.executor.complete(key, std::move(completion)));
    REQUIRE(reported.size() == 1U);
    CHECK(reported.front().code == "db.constraint");

    auto attempt = fixture.attempts.find(key.run_id, key.attempt_number);
    REQUIRE(attempt);
    REQUIRE(attempt->has_value());
    CHECK(attempt->value().state == AttemptState::Running);
    CHECK_FALSE(attempt->value().completed_at);
    auto run = fixture.runs.find_by_id(key.run_id);
    REQUIRE(run);
    REQUIRE(run->has_value());
    CHECK(run->value().state == RunState::Running);
    CHECK_FALSE(run->value().completed_at);
    auto output = fixture.attempts.find_output(key.run_id, key.attempt_number);
    REQUIRE(output);
    CHECK_FALSE(output->has_value());

    auto failed = core.process_cycle();
    REQUIRE_FALSE(failed);
    CHECK(failed.error().code == "db.constraint");
    CHECK(reported.size() == 1U);
    CHECK(fixture.executor.start_requests().size() == 1U);
    auto waiting = fixture.runs.find_by_id(id(145));
    REQUIRE(waiting);
    REQUIRE(waiting->has_value());
    CHECK(waiting->value().state == RunState::Scheduled);
}

TEST_CASE("Scheduler core rolls suspension-drain failures back and fails closed",
          "[jobu][scheduler][core][completion][suspension][rollback][management][sqlite]")
{
    enum class FailureKind : std::uint8_t {
        Persistence,
        RevisionExhaustion,
    };

    auto const verify_failure = [](FailureKind kind) {
        CoreFixture fixture;
        auto const  queue_id = id(1);
        auto const  job_id   = id(110);
        auto const  run_id   = id(10);
        insert_queue(fixture.database, queue_id, 2);
        insert_scheduled(fixture, queue_id, 10, JobType::Cli);
        fixture.executor.set_available(JobType::Cli, true);
        SchedulerCore core{
            fixture.database,
            fixture.registry,
            fixture.cron,
            fixture.generator,
            fixture.time,
            fixture.executor,
            {.cli_concurrency = 1, .http_concurrency = 1, .candidate_batch_size = 2}
        };
        ManagementService service{fixture.database, fixture.registry, fixture.cron, fixture.generator, fixture.time};

        REQUIRE(core.process_cycle());
        auto const key = fixture.executor.pending_keys().front();
        insert_scheduled(fixture, queue_id, 11, JobType::Cli);
        fixture.time.set_utc(at(130));
        auto draining = service.suspend_job(job_id);
        REQUIRE(draining);
        CHECK(draining->state == JobState::Suspending);
        CHECK(draining->revision == 2);

        if (kind == FailureKind::Persistence) {
            Query trigger{fixture.database};
            REQUIRE(trigger.exec("CREATE TRIGGER fail_scheduler_job_drain BEFORE UPDATE OF state ON jobu_jobs "
                                 "WHEN OLD.state = 'suspending' AND NEW.state = 'suspended' "
                                 "BEGIN SELECT RAISE(ABORT, 'injected scheduler drain failure'); END"));
        }
        else {
            Query revision{fixture.database};
            REQUIRE(revision.prepare("UPDATE jobu_jobs SET revision = :revision WHERE id = :id"));
            REQUIRE(revision.bind_value(":revision", std::numeric_limits<std::int64_t>::max()));
            REQUIRE(revision.bind_value(":id", uuid_to_storage(job_id)));
            REQUIRE(revision.exec());
            REQUIRE(revision.num_rows_affected() == 1);
        }

        fixture.time.set_utc(at(200));
        REQUIRE(fixture.executor.complete(key, success(key)));
        auto attempt = fixture.attempts.find(run_id, key.attempt_number);
        REQUIRE(attempt);
        REQUIRE(attempt->has_value());
        CHECK(attempt->value().state == AttemptState::Running);
        auto run = fixture.runs.find_by_id(run_id);
        REQUIRE(run);
        REQUIRE(run->has_value());
        CHECK(run->value().state == RunState::Running);
        auto still_draining = service.get_job(job_id);
        REQUIRE(still_draining);
        CHECK(still_draining->state == JobState::Suspending);

        auto failed = core.process_cycle();
        REQUIRE_FALSE(failed);
        if (kind == FailureKind::RevisionExhaustion) {
            CHECK(failed.error().code == "jobu.job.revision_exhausted");
        }
        CHECK(fixture.executor.start_requests().size() == 1U);
    };

    SECTION("persistence error")
    {
        verify_failure(FailureKind::Persistence);
    }
    SECTION("job revision exhaustion")
    {
        verify_failure(FailureKind::RevisionExhaustion);
    }
}

TEST_CASE("Scheduler core runs a service-created manual retry before releasing its scheduled barrier",
          "[jobu][scheduler][core][run-now][retry][suspension][management][sqlite]")
{
    auto const  queue_id     = id(1);
    auto const  job_id       = id(2);
    auto const  scheduled_id = id(3);
    auto const  manual_id    = id(4);
    auto const  schedule     = CronSchedule{.expression = "*/5 * * * *", .timezone = "UTC"};
    CoreFixture fixture{
        {queue_id, job_id, scheduled_id, manual_id}
    };
    fixture.cron.set_occurrences(schedule, {at(200), at(300)});
    fixture.executor.set_available(JobType::Cli, true);
    ManagementService service{fixture.database, fixture.registry, fixture.cron, fixture.generator, fixture.time};
    REQUIRE(service.create_queue({.name = "manual", .concurrency_limit = 1}));
    REQUIRE(service.create_job({
        .queue    = queue_id,
        .schedule = schedule,
        .attributes =
            {
                         {"retry.initial_delay", {.data = std::chrono::duration_cast<Duration>(10us)}},
                         {"retry.max_attempts", {.data = std::int64_t{2}}},
                         },
        .payload = cli_payload("manual"),
    }));
    auto suspended = service.suspend_job(job_id);
    REQUIRE(suspended);
    CHECK(suspended->state == JobState::Suspended);
    auto manual = service.run_now({.job_id = job_id});
    REQUIRE(manual);
    CHECK(manual->id == manual_id);

    SchedulerCore core{
        fixture.database,
        fixture.registry,
        fixture.cron,
        fixture.generator,
        fixture.time,
        fixture.executor,
        {.cli_concurrency = 1, .http_concurrency = 1, .candidate_batch_size = 1}
    };
    REQUIRE(core.process_cycle());
    REQUIRE(fixture.executor.pending_keys().size() == 1U);
    auto const first_key = fixture.executor.pending_keys().front();
    CHECK(first_key.run_id == manual_id);

    fixture.time.set_utc(at(130));
    REQUIRE(fixture.executor.complete(first_key, failure(first_key, FailureDisposition::Retryable)));
    auto waiting = fixture.runs.find_by_id(manual_id);
    REQUIRE(waiting);
    REQUIRE(waiting->has_value());
    CHECK(waiting->value().state == RunState::RetryWait);
    CHECK(waiting->value().runnable_at == at(140));

    fixture.time.set_utc(at(139));
    REQUIRE(core.process_cycle());
    CHECK(fixture.executor.pending_keys().empty());
    fixture.time.set_utc(at(200));
    REQUIRE(core.process_cycle());
    REQUIRE(fixture.executor.pending_keys().size() == 1U);
    auto const retry_key = fixture.executor.pending_keys().front();
    CHECK(retry_key.run_id == manual_id);
    CHECK(retry_key.attempt_number == 2U);
    fixture.time.set_utc(at(201));
    REQUIRE(fixture.executor.complete(retry_key, success(retry_key)));

    auto completed = fixture.runs.find_by_id(manual_id);
    REQUIRE(completed);
    REQUIRE(completed->has_value());
    CHECK(completed->value().state == RunState::Succeeded);
    auto scheduled = fixture.runs.find_schedule_owned(job_id);
    REQUIRE(scheduled);
    REQUIRE(scheduled->has_value());
    CHECK(scheduled->value().id == scheduled_id);
    CHECK(scheduled->value().planned_at == at(200));
    CHECK(scheduled->value().runnable_at == at(200));
    auto resumed = service.resume_job(job_id);
    REQUIRE(resumed);
    CHECK(resumed->state == JobState::Active);

    REQUIRE(core.process_cycle());
    REQUIRE(fixture.executor.pending_keys().size() == 1U);
    CHECK(fixture.executor.pending_keys().front().run_id == scheduled_id);
}

TEST_CASE("Scheduler core delays a service-created manual run until its queue resumes",
          "[jobu][scheduler][core][run-now][suspension][management][sqlite]")
{
    auto const  queue_id     = id(1);
    auto const  job_id       = id(2);
    auto const  scheduled_id = id(3);
    auto const  manual_id    = id(4);
    auto const  schedule     = CronSchedule{.expression = "*/5 * * * *", .timezone = "UTC"};
    CoreFixture fixture{
        {queue_id, job_id, scheduled_id, manual_id}
    };
    fixture.cron.set_occurrences(schedule, {at(300)});
    fixture.executor.set_available(JobType::Cli, true);
    ManagementService service{fixture.database, fixture.registry, fixture.cron, fixture.generator, fixture.time};
    REQUIRE(service.create_queue({.name = "suspended"}));
    REQUIRE(service.create_job({.queue = queue_id, .schedule = schedule, .payload = cli_payload("manual")}));
    auto suspended = service.suspend_queue(queue_id);
    REQUIRE(suspended);
    CHECK(suspended->state == QueueState::Suspended);
    auto manual = service.run_now({.job_id = job_id});
    REQUIRE(manual);
    CHECK(manual->id == manual_id);

    SchedulerCore core{
        fixture.database,
        fixture.registry,
        fixture.cron,
        fixture.generator,
        fixture.time,
        fixture.executor,
        {.cli_concurrency = 1, .http_concurrency = 1, .candidate_batch_size = 1}
    };
    REQUIRE(core.process_cycle());
    CHECK(fixture.executor.start_requests().empty());
    auto resumed = service.resume_queue(queue_id);
    REQUIRE(resumed);
    CHECK(resumed->state == QueueState::Active);
    REQUIRE(core.process_cycle());
    REQUIRE(fixture.executor.start_requests().size() == 1U);
    CHECK(fixture.executor.start_requests().front().key.run_id == manual_id);
}

TEST_CASE("Scheduler core admits manual work through weighted fairness and queue capacity",
          "[jobu][scheduler][core][run-now][fairness][capacity][management][sqlite]")
{
    auto const  manual_queue = id(1);
    auto const  job_id       = id(2);
    auto const  scheduled_id = id(3);
    auto const  manual_id    = id(4);
    auto const  other_queue  = id(5);
    auto const  schedule     = CronSchedule{.expression = "*/5 * * * *", .timezone = "UTC"};
    CoreFixture fixture{
        {manual_queue, job_id, scheduled_id, manual_id}
    };
    fixture.cron.set_occurrences(schedule, {at(300)});
    ManagementService service{fixture.database, fixture.registry, fixture.cron, fixture.generator, fixture.time};
    REQUIRE(service.create_queue({.name = "manual", .weight = 1, .concurrency_limit = 1}));
    REQUIRE(service.create_job({.queue = manual_queue, .schedule = schedule, .payload = cli_payload("manual")}));
    REQUIRE(service.run_now({.job_id = job_id}));
    insert_queue(fixture.database, other_queue, 2, 2);
    insert_scheduled(fixture, other_queue, 10, JobType::Cli);
    insert_scheduled(fixture, other_queue, 11, JobType::Cli);
    fixture.executor.set_available(JobType::Cli, true);

    REQUIRE(fixture.process({.cli_concurrency = 3, .http_concurrency = 1, .candidate_batch_size = 2}));
    REQUIRE(fixture.executor.start_requests().size() == 3U);
    CHECK(starts_for(fixture.executor, manual_queue, JobType::Cli) == 1U);
    CHECK(starts_for(fixture.executor, other_queue, JobType::Cli) == 2U);
    CHECK(fixture.executor.start_requests()[1].key.run_id == manual_id);
}

TEST_CASE("Scheduler core reconstructs manual barriers before dispatch",
          "[jobu][scheduler][core][run-now][invariant][sqlite]")
{
    CoreFixture fixture;
    auto const  queue_id = id(1);
    auto const  job_id   = id(2);
    insert_queue(fixture.database, queue_id, 3);
    insert_job(fixture.database,
               job_id,
               queue_id,
               JobType::Cli,
               JobState::Active,
               CronSchedule{.expression = "*/5 * * * *", .timezone = "UTC"});
    auto scheduled        = default_run(fixture, id(3), job_id, queue_id, JobType::Cli);
    scheduled.planned_at  = at(300);
    scheduled.runnable_at = at(300);
    insert_run(fixture.database, scheduled);
    auto manual           = default_run(fixture, id(4), job_id, queue_id, JobType::Cli);
    manual.origin         = RunOrigin::Manual;
    manual.schedule_owned = false;
    insert_run(fixture.database, manual);
    manual.id = id(5);
    insert_run(fixture.database, manual);
    fixture.executor.set_available(JobType::Cli, true);

    auto result = fixture.process({.cli_concurrency = 3, .http_concurrency = 1, .candidate_batch_size = 1});
    REQUIRE_FALSE(result);
    CHECK(result.error().code == "jobu.storage.invariant");
    CHECK(result.error().detail == "reason=manual_barrier_relationship");
    CHECK(fixture.executor.start_requests().empty());
}

TEST_CASE("Scheduler core completes pending cancellation atomically",
          "[jobu][scheduler][core][cancellation][capacity][sqlite]")
{
    SECTION("scheduled run")
    {
        CoreFixture fixture;
        auto const  queue_id = id(10);
        auto const  job_id   = id(110);
        auto const  run_id   = id(11);
        insert_queue(fixture.database, queue_id, 1);
        insert_job(fixture.database, job_id, queue_id, JobType::Cli);
        insert_run(fixture.database, default_run(fixture, run_id, job_id, queue_id, JobType::Cli));
        SchedulerCore core{
            fixture.database,
            fixture.registry,
            fixture.cron,
            fixture.generator,
            fixture.time,
            fixture.executor,
            {.cli_concurrency = 1, .http_concurrency = 1, .candidate_batch_size = 1}
        };

        fixture.time.set_utc(at(200));
        auto cancelled = core.cancel_run(run_id);
        REQUIRE(cancelled);
        CHECK(cancelled->disposition == CancelDisposition::Completed);
        CHECK(cancelled->run.state == RunState::Cancelled);
        CHECK(cancelled->run.completed_at == at(200));
        REQUIRE(cancelled->run.result);
        CHECK(cancelled->run.result->as_object().at("reason").as_string() == "cancelled");
        CHECK(fixture.executor.cancel_calls().empty());

        auto persisted = fixture.runs.find_by_id(run_id);
        REQUIRE(persisted);
        REQUIRE(persisted->has_value());
        CHECK(persisted->value().state == RunState::Cancelled);
        CHECK(persisted->value().completed_at == at(200));
        REQUIRE(persisted->value().result);
        CHECK(persisted->value().result->as_object().at("reason").as_string() == "cancelled");
    }

    SECTION("blocking retry releases its queue slot after commit")
    {
        CoreFixture fixture;
        auto const  queue_id = id(20);
        insert_queue(fixture.database, queue_id, 1);
        insert_blocking_retry(fixture, queue_id, 21);
        insert_scheduled(fixture, queue_id, 22, JobType::Cli);
        fixture.executor.set_available(JobType::Cli, true);
        SchedulerCore core{
            fixture.database,
            fixture.registry,
            fixture.cron,
            fixture.generator,
            fixture.time,
            fixture.executor,
            {.cli_concurrency = 1, .http_concurrency = 1, .candidate_batch_size = 2}
        };

        auto cancelled = core.cancel_run(id(21));
        REQUIRE(cancelled);
        CHECK(cancelled->disposition == CancelDisposition::Completed);
        CHECK(cancelled->run.state == RunState::Cancelled);
        CHECK(cancelled->run.started_at == at(70));

        REQUIRE(core.process_cycle());
        REQUIRE(fixture.executor.pending_keys().size() == 1U);
        CHECK(fixture.executor.pending_keys().front().run_id == id(22));
    }
}

TEST_CASE("Scheduler core retains running cancellation until forced terminal completion",
          "[jobu][scheduler][core][cancellation][completion][capacity][sqlite]")
{
    auto const verify_completion = [](bool retryable_completion) {
        CoreFixture fixture;
        auto const  queue_id = id(30);
        insert_queue(fixture.database, queue_id, 1);
        insert_scheduled(fixture, queue_id, 31, JobType::Cli);
        insert_scheduled(fixture, queue_id, 32, JobType::Cli);
        fixture.executor.set_available(JobType::Cli, true);
        SchedulerCore core{
            fixture.database,
            fixture.registry,
            fixture.cron,
            fixture.generator,
            fixture.time,
            fixture.executor,
            {.cli_concurrency = 1, .http_concurrency = 1, .candidate_batch_size = 2}
        };

        REQUIRE(core.process_cycle());
        REQUIRE(fixture.executor.pending_keys().size() == 1U);
        auto const key = fixture.executor.pending_keys().front();
        REQUIRE(key.run_id == id(31));

        auto requested = core.cancel_run(key.run_id);
        REQUIRE(requested);
        CHECK(requested->disposition == CancelDisposition::Requested);
        CHECK(requested->run.state == RunState::Running);
        REQUIRE(fixture.executor.cancel_calls().size() == 1U);
        CHECK(fixture.executor.cancel_calls().front() == key);

        auto repeated = core.cancel_run(key.run_id);
        REQUIRE(repeated);
        CHECK(repeated->disposition == CancelDisposition::Requested);
        CHECK(fixture.executor.cancel_calls().size() == 1U);

        REQUIRE(core.process_cycle());
        CHECK(fixture.executor.start_requests().size() == 1U);

        fixture.time.set_utc(at(200));
        auto completion   = retryable_completion ? failure(key, FailureDisposition::Retryable, at(400))
                                                 : success(key, "finished-during-cancel");
        completion.output = captured_output();
        REQUIRE(fixture.executor.complete(key, std::move(completion)));

        auto run = fixture.runs.find_by_id(key.run_id);
        REQUIRE(run);
        REQUIRE(run->has_value());
        CHECK(run->value().state == RunState::Cancelled);
        CHECK(run->value().completed_at == at(200));
        REQUIRE(run->value().result);
        CHECK(run->value().result->as_object().at("reason").as_string() == "cancelled");
        auto attempt = fixture.attempts.find(key.run_id, key.attempt_number);
        REQUIRE(attempt);
        REQUIRE(attempt->has_value());
        CHECK(attempt->value().state == AttemptState::Completed);
        CHECK(attempt->value().outcome == AttemptOutcome::Cancelled);
        REQUIRE(attempt->value().result);
        CHECK(attempt->value().result->as_object().at("reason").as_string() == "cancelled");
        check_captured_output(fixture.attempts, key);

        REQUIRE(core.process_cycle());
        REQUIRE(fixture.executor.pending_keys().size() == 1U);
        CHECK(fixture.executor.pending_keys().front().run_id == id(32));
    };

    SECTION("successful executor completion")
    {
        verify_completion(false);
    }
    SECTION("retryable executor completion")
    {
        verify_completion(true);
    }
}

TEST_CASE("Scheduler core handles cancellation errors and invalid run states",
          "[jobu][scheduler][core][cancellation][errors][sqlite]")
{
    SECTION("executor refusal leaves cancellation retryable")
    {
        CoreFixture fixture;
        auto const  queue_id = id(40);
        insert_queue(fixture.database, queue_id, 1);
        insert_scheduled(fixture, queue_id, 41, JobType::Cli);
        fixture.executor.set_available(JobType::Cli, true);
        SchedulerCore core{
            fixture.database,
            fixture.registry,
            fixture.cron,
            fixture.generator,
            fixture.time,
            fixture.executor,
            {.cli_concurrency = 1, .http_concurrency = 1, .candidate_batch_size = 1}
        };
        REQUIRE(core.process_cycle());
        auto const key = fixture.executor.pending_keys().front();
        fixture.executor.set_cancel_error(Error{
            .category = ErrorCategory::Unavailable,
            .code     = "test.executor.cancel_unavailable",
            .message  = "Configured cancellation failure",
        });

        auto refused = core.cancel_run(key.run_id);
        REQUIRE_FALSE(refused);
        CHECK(refused.error().code == "test.executor.cancel_unavailable");
        CHECK(fixture.executor.cancel_calls().size() == 1U);
        auto running = fixture.runs.find_by_id(key.run_id);
        REQUIRE(running);
        REQUIRE(running->has_value());
        CHECK(running->value().state == RunState::Running);

        fixture.executor.set_cancel_error(std::nullopt);
        auto requested = core.cancel_run(key.run_id);
        REQUIRE(requested);
        CHECK(requested->disposition == CancelDisposition::Requested);
        CHECK(fixture.executor.cancel_calls().size() == 2U);
        REQUIRE(fixture.executor.complete(key, success(key)));
        auto cancelled = fixture.runs.find_by_id(key.run_id);
        REQUIRE(cancelled);
        REQUIRE(cancelled->has_value());
        CHECK(cancelled->value().state == RunState::Cancelled);
    }

    SECTION("unknown run")
    {
        CoreFixture   fixture;
        SchedulerCore core{
            fixture.database,
            fixture.registry,
            fixture.cron,
            fixture.generator,
            fixture.time,
            fixture.executor,
        };
        auto missing = core.cancel_run(id(42));
        REQUIRE_FALSE(missing);
        CHECK(missing.error().category == ErrorCategory::NotFound);
        CHECK(missing.error().code == "jobu.run.not_found");
    }

    SECTION("terminal run")
    {
        CoreFixture fixture;
        auto const  queue_id = id(43);
        insert_queue(fixture.database, queue_id, 1);
        insert_scheduled(fixture, queue_id, 44, JobType::Cli);
        fixture.executor.set_available(JobType::Cli, true);
        SchedulerCore core{
            fixture.database,
            fixture.registry,
            fixture.cron,
            fixture.generator,
            fixture.time,
            fixture.executor,
            {.cli_concurrency = 1, .http_concurrency = 1, .candidate_batch_size = 1}
        };
        REQUIRE(core.process_cycle());
        auto const key = fixture.executor.pending_keys().front();
        REQUIRE(fixture.executor.complete(key, success(key)));

        auto conflict = core.cancel_run(key.run_id);
        REQUIRE_FALSE(conflict);
        CHECK(conflict.error().category == ErrorCategory::Conflict);
        CHECK(conflict.error().code == "jobu.run.state_conflict");
    }

    SECTION("running work from another scheduler instance")
    {
        CoreFixture fixture;
        auto const  queue_id = id(45);
        insert_queue(fixture.database, queue_id, 1);
        insert_running(fixture, queue_id, 46, JobType::Cli);
        SchedulerCore core{
            fixture.database,
            fixture.registry,
            fixture.cron,
            fixture.generator,
            fixture.time,
            fixture.executor,
        };

        auto recovered = core.cancel_run(id(46));
        REQUIRE_FALSE(recovered);
        CHECK(recovered.error().category == ErrorCategory::Conflict);
        CHECK(recovered.error().code == "jobu.scheduler.recovery_required");
        CHECK(fixture.executor.cancel_calls().empty());
    }

    SECTION("contradictory scheduled attempt relationship")
    {
        CoreFixture fixture;
        auto const  queue_id = id(47);
        insert_queue(fixture.database, queue_id, 1);
        insert_scheduled(fixture, queue_id, 48, JobType::Cli);
        REQUIRE(fixture.attempts.insert_attempt(JobAttempt{
            .run_id         = id(48),
            .attempt_number = 1,
            .due_at         = at(80),
            .started_at     = at(90),
            .state          = AttemptState::Running,
        }));
        SchedulerCore core{
            fixture.database,
            fixture.registry,
            fixture.cron,
            fixture.generator,
            fixture.time,
            fixture.executor,
        };

        auto invalid = core.cancel_run(id(48));
        REQUIRE_FALSE(invalid);
        CHECK(invalid.error().category == ErrorCategory::Internal);
        CHECK(invalid.error().code == "jobu.storage.invariant");
        CHECK(fixture.executor.cancel_calls().empty());
    }
}

TEST_CASE("Scheduler core retains cancelled running state when completion persistence fails",
          "[jobu][scheduler][core][cancellation][completion][rollback][capacity][sqlite]")
{
    CoreFixture fixture;
    auto const  queue_id = id(49);
    insert_queue(fixture.database, queue_id, 2);
    insert_scheduled(fixture, queue_id, 50, JobType::Cli);
    insert_scheduled(fixture, queue_id, 51, JobType::Cli);
    fixture.executor.set_available(JobType::Cli, true);
    SchedulerCore core{
        fixture.database,
        fixture.registry,
        fixture.cron,
        fixture.generator,
        fixture.time,
        fixture.executor,
        {.cli_concurrency = 1, .http_concurrency = 1, .candidate_batch_size = 2}
    };

    REQUIRE(core.process_cycle());
    auto const key = fixture.executor.pending_keys().front();
    REQUIRE(core.cancel_run(key.run_id));
    Query trigger{fixture.database};
    REQUIRE(trigger.exec("CREATE TRIGGER fail_cancelled_completion BEFORE UPDATE OF state ON jobu_runs "
                         "WHEN OLD.state = 'running' AND NEW.state = 'cancelled' "
                         "BEGIN SELECT RAISE(ABORT, 'injected cancelled completion failure'); END"));

    fixture.time.set_utc(at(200));
    REQUIRE(fixture.executor.complete(key, success(key)));
    auto run = fixture.runs.find_by_id(key.run_id);
    REQUIRE(run);
    REQUIRE(run->has_value());
    CHECK(run->value().state == RunState::Running);
    auto attempt = fixture.attempts.find(key.run_id, key.attempt_number);
    REQUIRE(attempt);
    REQUIRE(attempt->has_value());
    CHECK(attempt->value().state == AttemptState::Running);

    auto failed = core.process_cycle();
    REQUIRE_FALSE(failed);
    CHECK(failed.error().code == "db.constraint");
    CHECK(fixture.executor.start_requests().size() == 1U);
}

TEST_CASE("Scheduler core still validates executor protocol after accepting cancellation",
          "[jobu][scheduler][core][cancellation][completion][protocol][sqlite]")
{
    CoreFixture fixture;
    auto const  queue_id = id(52);
    insert_queue(fixture.database, queue_id, 1);
    insert_scheduled(fixture, queue_id, 53, JobType::Cli);
    RawAttemptExecutor executor;
    SchedulerCore      core{
        fixture.database,
        fixture.registry,
        fixture.cron,
        fixture.generator,
        fixture.time,
        executor,
        {.cli_concurrency = 1, .http_concurrency = 1, .candidate_batch_size = 1}
    };

    REQUIRE(core.process_cycle());
    REQUIRE(executor.requests.size() == 1U);
    auto const key = executor.requests.front().key;
    REQUIRE(core.cancel_run(key.run_id));
    auto invalid                = success(key);
    invalid.failure_disposition = FailureDisposition::Terminal;
    executor.emit(std::move(invalid));

    auto failed = core.process_cycle();
    REQUIRE_FALSE(failed);
    CHECK(failed.error().code == "jobu.executor.invalid_completion");
    auto run = fixture.runs.find_by_id(key.run_id);
    REQUIRE(run);
    REQUIRE(run->has_value());
    CHECK(run->value().state == RunState::Running);
}

TEST_CASE("Scheduler core creates recurring successors during immediate cancellation",
          "[jobu][scheduler][core][cancellation][recurrence][sqlite]")
{
    auto const  schedule     = CronSchedule{.expression = "*/5 * * * *", .timezone = "UTC"};
    auto const  queue_id     = id(50);
    auto const  job_id       = id(150);
    auto const  run_id       = id(51);
    auto const  successor_id = id(52);
    CoreFixture fixture{{successor_id}};
    insert_queue(fixture.database, queue_id, 1);
    insert_job(fixture.database, job_id, queue_id, JobType::Cli, JobState::Active, schedule);
    auto run        = default_run(fixture, run_id, job_id, queue_id, JobType::Cli);
    run.planned_at  = at(150);
    run.runnable_at = at(300);
    insert_run(fixture.database, run);
    fixture.cron.set_occurrences(schedule, {at(140), at(160), at(200)});
    SchedulerCore core{
        fixture.database,
        fixture.registry,
        fixture.cron,
        fixture.generator,
        fixture.time,
        fixture.executor,
    };

    fixture.time.set_utc(at(130));
    auto cancelled = core.cancel_run(run_id);
    REQUIRE(cancelled);
    CHECK(cancelled->disposition == CancelDisposition::Completed);
    CHECK(cancelled->run.state == RunState::Cancelled);
    auto successor = fixture.runs.find_schedule_owned(job_id);
    REQUIRE(successor);
    REQUIRE(successor->has_value());
    CHECK(successor->value().id == successor_id);
    CHECK(successor->value().planned_at == at(160));
    REQUIRE(fixture.cron.next_calls().size() == 1U);
    CHECK(fixture.cron.next_calls().front().exclusive_lower_bound == at(150));
}

TEST_CASE("Scheduler core rolls immediate recurring cancellation failures back",
          "[jobu][scheduler][core][cancellation][recurrence][rollback][sqlite]")
{
    enum class FailureKind : std::uint8_t {
        Cron,
        Uuid,
        Persistence,
        ScheduleConflict,
    };

    auto const verify_failure = [](FailureKind kind) {
        auto        successor_ids = kind == FailureKind::Uuid ? std::vector<Uuid>{} : std::vector<Uuid>{id(62)};
        CoreFixture fixture{std::move(successor_ids)};
        auto const  schedule = CronSchedule{.expression = "*/5 * * * *", .timezone = "UTC"};
        auto const  queue_id = id(60);
        auto const  job_id   = id(160);
        auto const  run_id   = id(61);
        insert_queue(fixture.database, queue_id, 1);
        insert_job(fixture.database, job_id, queue_id, JobType::Cli, JobState::Active, schedule);
        insert_run(fixture.database, default_run(fixture, run_id, job_id, queue_id, JobType::Cli));
        fixture.cron.set_occurrences(schedule, {at(200)});

        auto expected_code = std::string{};
        if (kind == FailureKind::Cron) {
            expected_code = "test.cron.next_failed";
            fixture.cron.set_next_error(Error{
                .category = ErrorCategory::ResourceExhausted,
                .code     = expected_code,
                .message  = "Configured cron next-occurrence failure",
            });
        }
        else if (kind == FailureKind::Uuid) {
            expected_code = "test.uuid.sequence_exhausted";
        }
        else if (kind == FailureKind::Persistence) {
            expected_code = "db.constraint";
            Query trigger{fixture.database};
            REQUIRE(trigger.exec("CREATE TRIGGER fail_run_cancellation BEFORE UPDATE OF state ON jobu_runs "
                                 "WHEN OLD.state = 'scheduled' AND NEW.state = 'cancelled' "
                                 "BEGIN SELECT RAISE(ABORT, 'injected cancellation failure'); END"));
        }
        else {
            expected_code = "jobu.run.schedule_conflict";
            Query trigger{fixture.database};
            REQUIRE(trigger.exec(
                "CREATE TRIGGER inject_cancellation_successor AFTER UPDATE OF state ON jobu_runs "
                "WHEN OLD.state = 'scheduled' AND NEW.state = 'cancelled' BEGIN "
                "INSERT INTO jobu_runs(id, job_id, job_revision, queue_id, origin, schedule_owned, planned_at_us, "
                "runnable_at_us, started_at_us, completed_at_us, type, priority, attributes_json, payload_json, "
                "state, result_json) VALUES(zeroblob(16), NEW.job_id, NEW.job_revision, NEW.queue_id, 'scheduled', "
                "1, NEW.completed_at_us + 1, NEW.completed_at_us + 1, NULL, NULL, NEW.type, NEW.priority, "
                "NEW.attributes_json, NEW.payload_json, 'scheduled', NULL); END"));
        }

        fixture.time.set_utc(at(150));
        SchedulerCore core{
            fixture.database,
            fixture.registry,
            fixture.cron,
            fixture.generator,
            fixture.time,
            fixture.executor,
        };
        auto failed = core.cancel_run(run_id);
        REQUIRE_FALSE(failed);
        CHECK(failed.error().code == expected_code);
        auto preserved = fixture.runs.find_by_id(run_id);
        REQUIRE(preserved);
        REQUIRE(preserved->has_value());
        CHECK(preserved->value().state == RunState::Scheduled);
        CHECK_FALSE(preserved->value().completed_at);
        CHECK_FALSE(preserved->value().result);
        auto current = fixture.runs.find_schedule_owned(job_id);
        REQUIRE(current);
        REQUIRE(current->has_value());
        CHECK(current->value().id == run_id);
    };

    SECTION("cron occurrence failure")
    {
        verify_failure(FailureKind::Cron);
    }
    SECTION("UUID exhaustion")
    {
        verify_failure(FailureKind::Uuid);
    }
    SECTION("persistence failure")
    {
        verify_failure(FailureKind::Persistence);
    }
    SECTION("schedule-owned uniqueness conflict")
    {
        verify_failure(FailureKind::ScheduleConflict);
    }
}

TEST_CASE("Scheduler cancellation releases manual barriers and completes suspension drains",
          "[jobu][scheduler][core][cancellation][run-now][suspension][management][sqlite]")
{
    SECTION("pending manual run")
    {
        CoreFixture fixture;
        auto const  schedule         = CronSchedule{.expression = "*/5 * * * *", .timezone = "UTC"};
        auto const  queue_id         = id(70);
        auto const  job_id           = id(170);
        auto const  scheduled_run_id = id(71);
        auto const  manual_run_id    = id(72);
        insert_queue(fixture.database, queue_id, 1);
        insert_job(fixture.database, job_id, queue_id, JobType::Cli, JobState::Active, schedule);
        insert_run(fixture.database, default_run(fixture, scheduled_run_id, job_id, queue_id, JobType::Cli));
        auto manual           = default_run(fixture, manual_run_id, job_id, queue_id, JobType::Cli);
        manual.origin         = RunOrigin::Manual;
        manual.schedule_owned = false;
        insert_run(fixture.database, manual);
        fixture.executor.set_available(JobType::Cli, true);
        SchedulerCore core{
            fixture.database,
            fixture.registry,
            fixture.cron,
            fixture.generator,
            fixture.time,
            fixture.executor,
            {.cli_concurrency = 1, .http_concurrency = 1, .candidate_batch_size = 2}
        };

        auto cancelled = core.cancel_run(manual_run_id);
        REQUIRE(cancelled);
        CHECK(cancelled->disposition == CancelDisposition::Completed);
        CHECK(fixture.cron.next_calls().empty());
        REQUIRE(core.process_cycle());
        REQUIRE(fixture.executor.pending_keys().size() == 1U);
        CHECK(fixture.executor.pending_keys().front().run_id == scheduled_run_id);
    }

    SECTION("running job drain")
    {
        CoreFixture fixture;
        auto const  queue_id = id(73);
        auto const  job_id   = id(174);
        auto const  run_id   = id(74);
        insert_queue(fixture.database, queue_id, 1);
        insert_job(fixture.database, job_id, queue_id, JobType::Cli);
        insert_run(fixture.database, default_run(fixture, run_id, job_id, queue_id, JobType::Cli));
        fixture.executor.set_available(JobType::Cli, true);
        SchedulerCore core{
            fixture.database,
            fixture.registry,
            fixture.cron,
            fixture.generator,
            fixture.time,
            fixture.executor,
            {.cli_concurrency = 1, .http_concurrency = 1, .candidate_batch_size = 1}
        };
        ManagementService service{fixture.database, fixture.registry, fixture.cron, fixture.generator, fixture.time};

        REQUIRE(core.process_cycle());
        auto const key = fixture.executor.pending_keys().front();
        fixture.time.set_utc(at(130));
        auto draining = service.suspend_job(job_id);
        REQUIRE(draining);
        CHECK(draining->state == JobState::Suspending);
        auto requested = core.cancel_run(run_id);
        REQUIRE(requested);
        CHECK(requested->disposition == CancelDisposition::Requested);

        fixture.time.set_utc(at(200));
        REQUIRE(fixture.executor.complete(key, success(key)));
        auto suspended = service.get_job(job_id);
        REQUIRE(suspended);
        CHECK(suspended->state == JobState::Suspended);
        auto cancelled = fixture.runs.find_by_id(run_id);
        REQUIRE(cancelled);
        REQUIRE(cancelled->has_value());
        CHECK(cancelled->value().state == RunState::Cancelled);
    }
}

TEST_CASE("Single-queue scheduler core rejects zero limits and batch size", "[jobu][scheduler][core][sqlite]")
{
    CoreFixture fixture;

    for (auto const options : std::vector<SchedulerCoreOptions>{
             {.cli_concurrency = 0, .http_concurrency = 1, .candidate_batch_size = 1},
             {.cli_concurrency = 1, .http_concurrency = 0, .candidate_batch_size = 1},
             {.cli_concurrency = 1, .http_concurrency = 1, .candidate_batch_size = 0},
    }) {
        auto result = fixture.process(options);
        REQUIRE_FALSE(result);
        CHECK(result.error().category == ErrorCategory::InvalidArgument);
        CHECK(result.error().code == "jobu.scheduler.invalid_options");
    }
}

TEST_CASE("Scheduler core returns the earliest future wake for available executor types",
          "[jobu][scheduler][core][wake][sqlite]")
{
    CoreFixture fixture;
    auto const  queue_id = id(90);
    insert_queue(fixture.database, queue_id, 2);
    insert_scheduled(fixture, queue_id, 91, JobType::Cli, 0, 150, 150);
    insert_scheduled(fixture, queue_id, 92, JobType::Http, 0, 140, 140);
    fixture.executor.set_available(JobType::Cli, true);
    fixture.executor.set_available(JobType::Http, false);
    SchedulerCore core{
        fixture.database,
        fixture.registry,
        fixture.cron,
        fixture.generator,
        fixture.time,
        fixture.executor,
        {.cli_concurrency = 1, .http_concurrency = 1, .candidate_batch_size = 2}
    };

    auto cycle = core.process_cycle();
    REQUIRE(cycle);
    CHECK(cycle->sampled_utc_now == at(120));
    REQUIRE(cycle->next_wake);
    CHECK(*cycle->next_wake == at(150));

    fixture.executor.set_available(JobType::Http, true);
    cycle = core.process_cycle();
    REQUIRE(cycle);
    REQUIRE(cycle->next_wake);
    CHECK(*cycle->next_wake == at(140));
}

TEST_CASE("Scheduler core notifies its adapter after asynchronous completion",
          "[jobu][scheduler][core][completion][notification][sqlite]")
{
    CoreFixture fixture;
    auto const  queue_id = id(93);
    insert_queue(fixture.database, queue_id, 1);
    insert_scheduled(fixture, queue_id, 94, JobType::Cli);
    fixture.executor.set_available(JobType::Cli, true);
    auto          rescan_count = std::size_t{0};
    auto          failures     = std::vector<Error>{};
    SchedulerCore core{
        fixture.database,
        fixture.registry,
        fixture.cron,
        fixture.generator,
        fixture.time,
        fixture.executor,
        {.cli_concurrency = 1, .http_concurrency = 1, .candidate_batch_size = 1},
        {.rescan_requested = [&rescan_count]() -> void { ++rescan_count; },
                   .failure_reported = [&failures](Error const& error) -> void { failures.push_back(error); }}
    };

    REQUIRE(core.process_cycle());
    REQUIRE(fixture.executor.pending_keys().size() == 1U);
    auto const key = fixture.executor.pending_keys().front();
    REQUIRE(fixture.executor.complete(key, success(key)));
    CHECK(rescan_count == 1U);
    CHECK(failures.empty());
}

TEST_CASE("Scheduler core reports one sticky asynchronous completion failure",
          "[jobu][scheduler][core][completion][notification][failure][sqlite]")
{
    CoreFixture        fixture;
    RawAttemptExecutor executor;
    auto const         queue_id = id(95);
    insert_queue(fixture.database, queue_id, 1);
    insert_scheduled(fixture, queue_id, 96, JobType::Cli);
    auto          reported = std::vector<Error>{};
    SchedulerCore core{
        fixture.database,
        fixture.registry,
        fixture.cron,
        fixture.generator,
        fixture.time,
        executor,
        {.cli_concurrency = 1, .http_concurrency = 1, .candidate_batch_size = 1},
        {.failure_reported = [&reported](Error const& error) -> void { reported.push_back(error); }}
    };

    REQUIRE(core.process_cycle());
    REQUIRE(executor.requests.size() == 1U);
    auto invalid       = success(executor.requests.front().key);
    invalid.key.run_id = id(97);
    executor.emit(std::move(invalid));
    REQUIRE(reported.size() == 1U);
    CHECK(reported.front().code == "jobu.executor.invalid_completion");

    auto failed = core.process_cycle();
    REQUIRE_FALSE(failed);
    CHECK(failed.error() == reported.front());
    CHECK(reported.size() == 1U);
}
