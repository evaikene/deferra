#include "scheduler_core_priv.hpp"

#include "attempt_repository_priv.hpp"
#include "attribute_codec_priv.hpp"
#include "attribute_registry.hpp"
#include "database.hpp"
#include "domain_storage_priv.hpp"
#include "query.hpp"
#include "run_repository_priv.hpp"
#include "sqlite/sqlite_driver.hpp"
#include "sqlite/sqlite_schema.hpp"
#include "support/fake_attempt_executor.hpp"
#include "support/fake_time_source.hpp"
#include "support/temporary_directory.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace jb::core;
using namespace jb::db;
using namespace jb::jobu;
using namespace jb::jobu::detail;
using namespace jb::rpc;
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
    CoreFixture()
        : attempts{database}
        , runs{database, registry}
    {
        REQUIRE(database.open());
        REQUIRE(jb::jobu::sqlite::ensure_schema(database));
        time.set_utc(at(120));
    }

    [[nodiscard]] auto process(SchedulerCoreOptions options) -> Result<void, Error>
    {
        SchedulerCore core{database, registry, time, executor, options};
        return core.process_cycle();
    }

    TemporaryDirectory        directory;
    std::filesystem::path     database_file{directory.path() / "jobu.sqlite"};
    Database                  database{make_database(database_file)};
    StandardAttributeRegistry registry;
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

void insert_job(Database&   database,
                Uuid const& job_id,
                Uuid const& queue_id,
                JobType     type,
                JobState    state = JobState::Active)
{
    Query query{database};
    REQUIRE(query.prepare(
        "INSERT INTO jobu_jobs(id, queue_id, revision, name, state, type, schedule_kind, scheduled_at_us, "
        "cron_expression, cron_timezone, priority, attributes_json, payload_json, created_at_us, updated_at_us, "
        "deleted_at_us) VALUES(:id, :queue_id, 1, NULL, :state, :type, 'once', 0, NULL, NULL, 0, "
        "'{\"version\":1,\"values\":{}}', '{}', 0, 0, NULL)"));
    REQUIRE(query.bind_value(":id", uuid_to_storage(job_id)));
    REQUIRE(query.bind_value(":queue_id", uuid_to_storage(queue_id)));
    REQUIRE(query.bind_value(":state", make_text(storage_text(state))));
    REQUIRE(query.bind_value(":type", make_text(storage_text(type))));
    REQUIRE(query.exec());
}

auto attribute_document(StandardAttributeRegistry const& registry,
                        std::string                      retry_mode    = "reschedule",
                        std::int64_t                     max_attempts  = 3,
                        Duration                         initial_delay = Duration::zero()) -> std::string
{
    auto attributes = materialize_attributes(registry,
                                             {
    },
                                             {},
                                             {
                                                 {"retry.initial_delay", {.data = initial_delay}},
                                                 {"retry.max_attempts", {.data = max_attempts}},
                                                 {"retry.mode", {.data = std::move(retry_mode)}},
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
        "result_json) VALUES(:id, :job_id, 1, :queue_id, 'scheduled', 1, :planned_at_us, :runnable_at_us, "
        ":started_at_us, NULL, :type, :priority, :attributes_json, '{}', :state, NULL)"));
    REQUIRE(query.bind_value(":id", uuid_to_storage(run.id)));
    REQUIRE(query.bind_value(":job_id", uuid_to_storage(run.job_id)));
    REQUIRE(query.bind_value(":queue_id", uuid_to_storage(run.queue_id)));
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

void update_queue_state(Database& database, Uuid const& queue_id, QueueState state)
{
    Query query{database};
    REQUIRE(query.prepare("UPDATE jobu_queues SET state = :state WHERE id = :id"));
    REQUIRE(query.bind_value(":state", make_text(storage_text(state))));
    REQUIRE(query.bind_value(":id", uuid_to_storage(queue_id)));
    REQUIRE(query.exec());
}

void update_queue_scheduling(Database&     database,
                             Uuid const&   queue_id,
                             std::uint32_t weight,
                             std::uint32_t concurrency_limit)
{
    Query query{database};
    REQUIRE(query.prepare(
        "UPDATE jobu_queues SET weight = :weight, concurrency_limit = :concurrency_limit WHERE id = :id"));
    REQUIRE(query.bind_value(":weight", static_cast<std::int64_t>(weight)));
    REQUIRE(query.bind_value(":concurrency_limit", static_cast<std::int64_t>(concurrency_limit)));
    REQUIRE(query.bind_value(":id", uuid_to_storage(queue_id)));
    REQUIRE(query.exec());
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

auto success(AttemptKey key, std::string value = "completed") -> AttemptCompletion
{
    return {
        .key     = key,
        .outcome = AttemptOutcome::Succeeded,
        .result  = result_object(std::move(value)),
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
        fixture.time,
        fixture.executor,
        {.cli_concurrency = 7, .http_concurrency = 1, .candidate_batch_size = 3}
    };

    REQUIRE(core.process_cycle());
    REQUIRE(fixture.executor.start_requests().size() == 1U);

    update_queue_scheduling(fixture.database, second_queue, 2, 8);
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
        fixture.time,
        fixture.executor,
        {.cli_concurrency = 3, .http_concurrency = 1, .candidate_batch_size = 2}
    };

    REQUIRE(core.process_cycle());
    update_queue_state(fixture.database, first_queue, QueueState::Suspended);
    insert_scheduled(fixture, first_queue, 11, JobType::Cli);
    insert_scheduled(fixture, second_queue, 20, JobType::Cli);
    REQUIRE(core.process_cycle());
    update_queue_state(fixture.database, first_queue, QueueState::Active);
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
        fixture.time,
        fixture.executor,
        {.cli_concurrency = 4, .http_concurrency = 1, .candidate_batch_size = 2}
    };

    REQUIRE(core.process_cycle());
    update_queue_scheduling(fixture.database, first_queue, 1, 1);
    insert_scheduled(fixture, first_queue, 11, JobType::Cli);
    insert_scheduled(fixture, second_queue, 20, JobType::Cli);
    REQUIRE(core.process_cycle());
    update_queue_scheduling(fixture.database, first_queue, 1, 2);
    insert_scheduled(fixture, second_queue, 21, JobType::Cli);
    REQUIRE(core.process_cycle());

    REQUIRE(fixture.executor.start_requests().size() == 4U);
    CHECK(fixture.executor.start_requests()[0].queue_id == first_queue);
    CHECK(fixture.executor.start_requests()[1].queue_id == second_queue);
    CHECK(fixture.executor.start_requests()[2].queue_id == first_queue);
    CHECK(fixture.executor.start_requests()[3].queue_id == second_queue);
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

    REQUIRE(core.process_cycle());
    REQUIRE(fixture.executor.pending_keys().size() == 1U);
    auto const second_key = fixture.executor.pending_keys().front();
    CHECK(second_key.run_id == id(102));
    fixture.time.set_utc(at(201));
    REQUIRE(fixture.executor.complete(second_key, success(second_key)));
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
