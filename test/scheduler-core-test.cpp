#include "scheduler_core_priv.hpp"

#include "attempt_repository_priv.hpp"
#include "attribute_codec_priv.hpp"
#include "attribute_registry.hpp"
#include "database.hpp"
#include "domain_storage_priv.hpp"
#include "query.hpp"
#include "sqlite/sqlite_driver.hpp"
#include "sqlite/sqlite_schema.hpp"
#include "support/fake_attempt_executor.hpp"
#include "support/fake_time_source.hpp"
#include "support/temporary_directory.hpp"

#include <catch2/catch_test_macros.hpp>

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
};

void insert_queue(Database& database, Uuid const& queue_id, std::uint32_t concurrency_limit)
{
    Query query{database};
    REQUIRE(query.prepare(
        "INSERT INTO jobu_queues(id, name, deleted_name, state, weight, concurrency_limit, recovery_policy, "
        "defaults_json, retention_seconds, runnable_wait_warning_ms, created_at_us, updated_at_us, deleted_at_us) "
        "VALUES(:id, :name, NULL, 'active', 1, :concurrency_limit, 'fail_interrupted', "
        "'{\"version\":1,\"values\":{}}', NULL, 10000, 0, 0, NULL)"));
    REQUIRE(query.bind_value(":id", uuid_to_storage(queue_id)));
    REQUIRE(
        query.bind_value(":name",
                         make_text("queue-" + std::to_string(std::to_integer<unsigned int>(queue_id.bytes().back())))));
    REQUIRE(query.bind_value(":concurrency_limit", static_cast<std::int64_t>(concurrency_limit)));
    REQUIRE(query.exec());
}

void insert_job(Database& database, Uuid const& job_id, Uuid const& queue_id, JobType type)
{
    Query query{database};
    REQUIRE(query.prepare(
        "INSERT INTO jobu_jobs(id, queue_id, revision, name, state, type, schedule_kind, scheduled_at_us, "
        "cron_expression, cron_timezone, priority, attributes_json, payload_json, created_at_us, updated_at_us, "
        "deleted_at_us) VALUES(:id, :queue_id, 1, NULL, 'active', :type, 'once', 0, NULL, NULL, 0, "
        "'{\"version\":1,\"values\":{}}', '{}', 0, 0, NULL)"));
    REQUIRE(query.bind_value(":id", uuid_to_storage(job_id)));
    REQUIRE(query.bind_value(":queue_id", uuid_to_storage(queue_id)));
    REQUIRE(query.bind_value(":type", make_text(storage_text(type))));
    REQUIRE(query.exec());
}

auto attribute_document(StandardAttributeRegistry const& registry, std::string retry_mode = "reschedule") -> std::string
{
    auto attributes = materialize_attributes(registry,
                                             {
    },
                                             {},
                                             {
                                                 {"retry.max_attempts", {.data = std::int64_t{3}}},
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
                      std::int32_t priority = 0,
                      std::int64_t runnable = 100,
                      std::int64_t planned  = 100)
{
    auto const job_id = id(static_cast<std::uint8_t>(run_suffix + 100));
    insert_job(fixture.database, job_id, queue_id, type);
    auto run        = default_run(fixture, id(run_suffix), job_id, queue_id, type);
    run.priority    = priority;
    run.runnable_at = at(runnable);
    run.planned_at  = at(planned);
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

void insert_blocking_retry(CoreFixture& fixture, Uuid const& queue_id, std::uint8_t run_suffix)
{
    auto const job_id = id(static_cast<std::uint8_t>(run_suffix + 100));
    insert_job(fixture.database, job_id, queue_id, JobType::Cli);
    auto run            = default_run(fixture, id(run_suffix), job_id, queue_id, JobType::Cli);
    run.state           = RunState::RetryWait;
    run.started_at      = at(70);
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
