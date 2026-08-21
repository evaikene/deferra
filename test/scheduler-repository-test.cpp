#include "scheduler_repository_priv.hpp"

#include "attempt_repository_priv.hpp"
#include "attribute_codec_priv.hpp"
#include "attribute_registry.hpp"
#include "database.hpp"
#include "domain_storage_priv.hpp"
#include "query.hpp"
#include "sqlite/sqlite_driver.hpp"
#include "sqlite/sqlite_schema.hpp"
#include "support/temporary_directory.hpp"

#include <catch2/catch_test_macros.hpp>

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

struct RepositoryFixture {
    RepositoryFixture()
        : repository{database, registry}
        , attempts{database}
    {
        REQUIRE(database.open());
        REQUIRE(jb::jobu::sqlite::ensure_schema(database));
    }

    TemporaryDirectory        directory;
    std::filesystem::path     database_file{directory.path() / "jobu.sqlite"};
    Database                  database{make_database(database_file)};
    StandardAttributeRegistry registry;
    SchedulerRepository       repository;
    AttemptRepository         attempts;
};

auto require_error(auto const& result, ErrorCategory category, std::string_view code) -> Error
{
    REQUIRE_FALSE(result);
    CHECK(result.error().category == category);
    CHECK(result.error().code == code);
    return result.error();
}

void execute(Database& database, std::string_view sql)
{
    Query query{database};
    REQUIRE(query.exec(sql));
}

void insert_queue(Database&     database,
                  Uuid const&   queue_id,
                  QueueState    state             = QueueState::Active,
                  std::uint32_t weight            = 1,
                  std::uint32_t concurrency_limit = 1)
{
    Query query{database};
    REQUIRE(query.prepare(
        "INSERT INTO jobu_queues(id, name, deleted_name, state, weight, concurrency_limit, recovery_policy, "
        "defaults_json, retention_seconds, runnable_wait_warning_ms, created_at_us, updated_at_us, deleted_at_us) "
        "VALUES(:id, :name, NULL, :state, :weight, :concurrency_limit, 'fail_interrupted', "
        "'{\"version\":1,\"values\":{}}', NULL, 10000, 0, 0, NULL)"));
    REQUIRE(query.bind_value(":id", uuid_to_storage(queue_id)));
    REQUIRE(
        query.bind_value(":name",
                         make_text("queue-" + std::to_string(std::to_integer<unsigned int>(queue_id.bytes().back())))));
    REQUIRE(query.bind_value(":state", make_text(storage_text(state))));
    REQUIRE(query.bind_value(":weight", static_cast<std::int64_t>(weight)));
    REQUIRE(query.bind_value(":concurrency_limit", static_cast<std::int64_t>(concurrency_limit)));
    REQUIRE(query.exec());
}

void insert_job(Database&   database,
                Uuid const& job_id,
                Uuid const& queue_id,
                JobState    state = JobState::Active,
                JobType     type  = JobType::Cli)
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
                        std::string                      retry_mode = "reschedule",
                        bool                             legacy     = false) -> std::string
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
    auto mode = AttributeDocumentMode::Materialized;
    if (legacy) {
        REQUIRE(attributes->erase("retry.jitter") == 1U);
        REQUIRE(attributes->erase("retry.multiplier") == 1U);
        REQUIRE(attributes->size() == 9U);
        mode = AttributeDocumentMode::Partial;
    }
    auto document = encode_and_serialize_attribute_document(registry, *attributes, AttributeScope::Job, mode);
    REQUIRE(document);
    return std::string{document->serialized()};
}

struct RunSpec {
    Uuid                        id;
    Uuid                        job_id;
    Uuid                        queue_id;
    RunOrigin                   origin{RunOrigin::Scheduled};
    bool                        schedule_owned{true};
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
        "result_json) VALUES(:id, :job_id, 1, :queue_id, :origin, :schedule_owned, :planned_at_us, "
        ":runnable_at_us, :started_at_us, NULL, :type, :priority, :attributes_json, '{}', :state, NULL)"));
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
    REQUIRE(query.bind_value(":state", make_text(storage_text(run.state))));
    REQUIRE(query.exec());
}

void insert_attempt(AttemptRepository& attempts, Uuid const& run_id, AttemptNumber number, AttemptState state)
{
    auto attempt = JobAttempt{
        .run_id         = run_id,
        .attempt_number = number,
        .due_at         = at(90),
        .state          = state,
    };
    if (state == AttemptState::Running) {
        attempt.started_at = at(95);
    }
    else if (state == AttemptState::Completed) {
        attempt.started_at   = at(80);
        attempt.completed_at = at(90);
        attempt.outcome      = AttemptOutcome::Failed;
        auto result          = JsonValue{};
        result.data          = JsonValue::Object{};
        attempt.result       = std::move(result);
    }
    REQUIRE(attempts.insert_attempt(attempt));
}

void set_attempt_outcome(Database& database, Uuid const& run_id, std::optional<AttemptOutcome> outcome)
{
    Query query{database};
    REQUIRE(query.prepare("UPDATE jobu_attempts SET outcome = :outcome WHERE run_id = :run_id"));
    REQUIRE(query.bind_value(":run_id", uuid_to_storage(run_id)));
    if (outcome) {
        REQUIRE(query.bind_value(":outcome", make_text(storage_text(*outcome))));
    }
    else {
        REQUIRE(query.bind_value(":outcome", Null{}));
    }
    REQUIRE(query.exec());
}

auto default_run(RepositoryFixture const& fixture, Uuid run_id, Uuid job_id, Uuid queue_id) -> RunSpec
{
    return {
        .id              = run_id,
        .job_id          = job_id,
        .queue_id        = queue_id,
        .attributes_json = attribute_document(fixture.registry),
    };
}

} // anonymous namespace

TEST_CASE("Scheduler repository pages active runtime queues by binary UUID", "[jobu][scheduler][repository][sqlite]")
{
    RepositoryFixture fixture;
    insert_queue(fixture.database, id(3), QueueState::Active, 3, 7);
    insert_queue(fixture.database, id(1), QueueState::Active, 2, 5);
    insert_queue(fixture.database, id(2), QueueState::Suspended, 9, 9);

    auto first = fixture.repository.list_runtime_queues(1, std::nullopt);
    REQUIRE(first);
    REQUIRE(first->size() == 1U);
    CHECK(first->front() == QueueRuntime{.id = id(1), .weight = 2, .concurrency_limit = 5});

    auto second = fixture.repository.list_runtime_queues(2, first->back().id);
    REQUIRE(second);
    REQUIRE(second->size() == 1U);
    CHECK(second->front() == QueueRuntime{.id = id(3), .weight = 3, .concurrency_limit = 7});

    require_error(fixture.repository.list_runtime_queues(0, std::nullopt),
                  ErrorCategory::InvalidArgument,
                  "jobu.storage.invalid_limit");
    require_error(fixture.repository.list_runtime_queues(std::numeric_limits<std::size_t>::max(), std::nullopt),
                  ErrorCategory::InvalidArgument,
                  "jobu.storage.invalid_limit");

    execute(fixture.database, "UPDATE jobu_queues SET weight = 4294967296 WHERE state = 'active'");
    require_error(fixture.repository.list_runtime_queues(10, std::nullopt),
                  ErrorCategory::Internal,
                  "jobu.storage.invariant");
}

TEST_CASE("Scheduler repository bounds every paged read", "[jobu][scheduler][repository][sqlite]")
{
    RepositoryFixture fixture;
    constexpr auto    maximum_page_rows = std::size_t{1000};

    auto queues = fixture.repository.list_runtime_queues(maximum_page_rows, std::nullopt);
    REQUIRE(queues);
    auto capacity = fixture.repository.list_capacity_rows(maximum_page_rows, std::nullopt);
    REQUIRE(capacity);
    auto barriers = fixture.repository.list_manual_barriers(maximum_page_rows, std::nullopt);
    REQUIRE(barriers);
    auto runnable = fixture.repository.list_runnable(id(1), JobType::Cli, at(100), maximum_page_rows);
    REQUIRE(runnable);

    constexpr auto oversized_page = maximum_page_rows + 1U;
    require_error(fixture.repository.list_runtime_queues(oversized_page, std::nullopt),
                  ErrorCategory::InvalidArgument,
                  "jobu.storage.invalid_limit");
    require_error(fixture.repository.list_capacity_rows(oversized_page, std::nullopt),
                  ErrorCategory::InvalidArgument,
                  "jobu.storage.invalid_limit");
    require_error(fixture.repository.list_manual_barriers(oversized_page, std::nullopt),
                  ErrorCategory::InvalidArgument,
                  "jobu.storage.invalid_limit");
    require_error(fixture.repository.list_runnable(id(1), JobType::Cli, at(100), oversized_page),
                  ErrorCategory::InvalidArgument,
                  "jobu.storage.invalid_limit");
}

TEST_CASE("Scheduler repository reconstructs global and combined queue capacity",
          "[jobu][scheduler][repository][sqlite]")
{
    RepositoryFixture fixture;
    auto const        active_queue    = id(10);
    auto const        suspended_queue = id(11);
    insert_queue(fixture.database, active_queue);
    insert_queue(fixture.database, suspended_queue, QueueState::Suspended);

    auto add_running = [&](std::uint8_t suffix, JobType type) {
        auto const job_id = id(static_cast<std::uint8_t>(suffix + 40));
        auto const run_id = id(suffix);
        insert_job(fixture.database, job_id, active_queue, JobState::Active, type);
        auto run       = default_run(fixture, run_id, job_id, active_queue);
        run.type       = type;
        run.state      = RunState::Running;
        run.started_at = at(90);
        insert_run(fixture.database, run);
        insert_attempt(fixture.attempts, run_id, 1, AttemptState::Running);
    };
    add_running(20, JobType::Cli);
    add_running(21, JobType::Http);

    auto add_retry =
        [&](std::uint8_t suffix, Uuid const& queue_id, JobState job_state, std::string mode, bool legacy = false) {
            auto const job_id = id(static_cast<std::uint8_t>(suffix + 40));
            auto const run_id = id(suffix);
            insert_job(fixture.database, job_id, queue_id, job_state);
            auto run            = default_run(fixture, run_id, job_id, queue_id);
            run.state           = RunState::RetryWait;
            run.started_at      = at(80);
            run.runnable_at     = at(200);
            run.attributes_json = attribute_document(fixture.registry, std::move(mode), legacy);
            insert_run(fixture.database, run);
            insert_attempt(fixture.attempts, run_id, 1, AttemptState::Completed);
        };
    add_retry(22, active_queue, JobState::Active, "blocking");
    add_retry(23, active_queue, JobState::Active, "reschedule");
    add_retry(24, active_queue, JobState::Suspended, "blocking");
    add_retry(25, suspended_queue, JobState::Active, "blocking");
    add_retry(26, active_queue, JobState::Active, "blocking", true);

    auto rows = std::vector<CapacityRow>{};
    auto page = fixture.repository.list_capacity_rows(3, std::nullopt);
    REQUIRE(page);
    rows.insert(rows.end(), page->begin(), page->end());
    REQUIRE(page->size() == 3U);
    page = fixture.repository.list_capacity_rows(3, page->back().run_id);
    REQUIRE(page);
    rows.insert(rows.end(), page->begin(), page->end());
    REQUIRE(page->size() == 3U);
    page = fixture.repository.list_capacity_rows(3, page->back().run_id);
    REQUIRE(page);
    rows.insert(rows.end(), page->begin(), page->end());
    REQUIRE(rows.size() == 7U);

    CHECK(rows[0].usage == CapacityUsage{.cli_running = 1, .http_running = 0, .queue_slots = 1});
    CHECK(rows[1].usage == CapacityUsage{.cli_running = 0, .http_running = 1, .queue_slots = 1});
    CHECK(rows[2].usage == CapacityUsage{.cli_running = 0, .http_running = 0, .queue_slots = 1});
    CHECK(rows[3].usage == CapacityUsage{});
    CHECK(rows[4].usage == CapacityUsage{});
    CHECK(rows[5].usage == CapacityUsage{});
    CHECK(rows[6].usage == CapacityUsage{.cli_running = 0, .http_running = 0, .queue_slots = 1});
}

TEST_CASE("Manual blocking retries retain capacity while bypassing job suspension",
          "[jobu][scheduler][repository][sqlite]")
{
    RepositoryFixture fixture;
    auto const        active_queue    = id(120);
    auto const        suspended_queue = id(121);
    insert_queue(fixture.database, active_queue);
    insert_queue(fixture.database, suspended_queue, QueueState::Suspended);

    auto add_retry = [&](std::uint8_t run_suffix,
                         std::uint8_t schedule_suffix,
                         std::uint8_t job_suffix,
                         Uuid const&  queue_id,
                         JobState     job_state) {
        auto const job_id = id(job_suffix);
        insert_job(fixture.database, job_id, queue_id, job_state);
        insert_run(fixture.database, default_run(fixture, id(schedule_suffix), job_id, queue_id));
        auto run            = default_run(fixture, id(run_suffix), job_id, queue_id);
        run.origin          = RunOrigin::Manual;
        run.schedule_owned  = false;
        run.state           = RunState::RetryWait;
        run.started_at      = at(80);
        run.runnable_at     = at(200);
        run.attributes_json = attribute_document(fixture.registry, "blocking");
        insert_run(fixture.database, run);
        insert_attempt(fixture.attempts, run.id, 1, AttemptState::Completed);
    };

    add_retry(130, 140, 150, active_queue, JobState::Active);
    add_retry(131, 141, 151, active_queue, JobState::Suspending);
    add_retry(132, 142, 152, active_queue, JobState::Suspended);
    add_retry(133, 143, 153, suspended_queue, JobState::Active);

    auto rows = fixture.repository.list_capacity_rows(10, std::nullopt);
    REQUIRE(rows);
    REQUIRE(rows->size() == 4U);
    CHECK((*rows)[0].usage.queue_slots == 1U);
    CHECK((*rows)[1].usage.queue_slots == 1U);
    CHECK((*rows)[2].usage.queue_slots == 1U);
    CHECK((*rows)[3].usage.queue_slots == 0U);
}

TEST_CASE("Scheduler repository reconstructs and validates manual barriers", "[jobu][scheduler][repository][sqlite]")
{
    RepositoryFixture fixture;
    auto const        queue_id = id(30);
    insert_queue(fixture.database, queue_id);

    auto add_pair = [&](std::uint8_t job_suffix, std::uint8_t schedule_suffix, std::uint8_t manual_suffix) {
        auto const job_id = id(job_suffix);
        insert_job(fixture.database, job_id, queue_id);
        insert_run(fixture.database, default_run(fixture, id(schedule_suffix), job_id, queue_id));
        auto manual           = default_run(fixture, id(manual_suffix), job_id, queue_id);
        manual.origin         = RunOrigin::Manual;
        manual.schedule_owned = false;
        insert_run(fixture.database, manual);
    };
    add_pair(31, 40, 41);
    add_pair(32, 42, 43);

    auto first = fixture.repository.list_manual_barriers(1, std::nullopt);
    REQUIRE(first);
    REQUIRE(first->size() == 1U);
    CHECK(first->front() == ManualBarrier{.run_id = id(41), .job_id = id(31)});
    auto second = fixture.repository.list_manual_barriers(2, first->back().run_id);
    REQUIRE(second);
    REQUIRE(second->size() == 1U);
    CHECK(second->front() == ManualBarrier{.run_id = id(43), .job_id = id(32)});

    SECTION("duplicate non-terminal manual rows are invariant failures")
    {
        auto duplicate           = default_run(fixture, id(44), id(31), queue_id);
        duplicate.origin         = RunOrigin::Manual;
        duplicate.schedule_owned = false;
        insert_run(fixture.database, duplicate);
        require_error(fixture.repository.list_manual_barriers(10, std::nullopt),
                      ErrorCategory::Internal,
                      "jobu.storage.invariant");
    }

    SECTION("a manual row without its schedule-owned sibling is an invariant failure")
    {
        execute(fixture.database,
                "UPDATE jobu_runs SET state = 'cancelled', completed_at_us = 200, "
                "result_json = '{}' WHERE id = X'00000000000070008000000000000028'");
        require_error(fixture.repository.list_manual_barriers(10, std::nullopt),
                      ErrorCategory::Internal,
                      "jobu.storage.invariant");
    }

    SECTION("a submitted schedule-owned sibling is an invariant failure")
    {
        execute(fixture.database,
                "UPDATE jobu_runs SET origin = 'submitted' WHERE id = "
                "X'00000000000070008000000000000028'");
        require_error(fixture.repository.list_manual_barriers(10, std::nullopt),
                      ErrorCategory::Internal,
                      "jobu.storage.invariant");
    }
}

TEST_CASE("Scheduler runnable reads preserve strict priority and time ordering",
          "[jobu][scheduler][repository][sqlite]")
{
    RepositoryFixture fixture;
    auto const        queue_id = id(50);
    insert_queue(fixture.database, queue_id);

    struct OrderedRun {
        std::uint8_t id_suffix;
        std::int32_t priority;
        std::int64_t runnable;
        std::int64_t planned;
    };

    for (auto const spec : std::vector<OrderedRun>{
             {60, 5, 90, 100},
             {61, 6, 90, 100},
             {62, 6, 80, 110},
             {63, 6, 80, 90 },
             {64, 6, 80, 90 },
    }) {
        auto const job_id = id(static_cast<std::uint8_t>(spec.id_suffix + 70));
        insert_job(fixture.database, job_id, queue_id);
        auto run        = default_run(fixture, id(spec.id_suffix), job_id, queue_id);
        run.priority    = spec.priority;
        run.runnable_at = at(spec.runnable);
        run.planned_at  = at(spec.planned);
        insert_run(fixture.database, run);
    }

    auto candidates = fixture.repository.list_runnable(queue_id, JobType::Cli, at(100), 10);
    REQUIRE(candidates);
    REQUIRE(candidates->size() == 5U);
    CHECK((*candidates)[0].run.id == id(63));
    CHECK((*candidates)[1].run.id == id(64));
    CHECK((*candidates)[2].run.id == id(62));
    CHECK((*candidates)[3].run.id == id(61));
    CHECK((*candidates)[4].run.id == id(60));

    auto bounded = fixture.repository.list_runnable(queue_id, JobType::Cli, at(100), 3);
    REQUIRE(bounded);
    REQUIRE(bounded->size() == 3U);
    CHECK((*bounded)[0].run.id == id(63));
    CHECK((*bounded)[1].run.id == id(64));
    CHECK((*bounded)[2].run.id == id(62));
}

TEST_CASE("Scheduler runnable and wake reads apply lifecycle and manual-barrier filters",
          "[jobu][scheduler][repository][sqlite]")
{
    RepositoryFixture fixture;
    auto const        active_queue    = id(70);
    auto const        suspended_queue = id(71);
    insert_queue(fixture.database, active_queue);
    insert_queue(fixture.database, suspended_queue, QueueState::Suspended);

    auto add_scheduled =
        [&](std::uint8_t run_suffix, Uuid const& queue_id, JobState job_state, JobType type, std::int64_t runnable) {
            auto const job_id = id(static_cast<std::uint8_t>(run_suffix + 80));
            insert_job(fixture.database, job_id, queue_id, job_state, type);
            auto run        = default_run(fixture, id(run_suffix), job_id, queue_id);
            run.type        = type;
            run.runnable_at = at(runnable);
            run.planned_at  = at(runnable);
            insert_run(fixture.database, run);
            return job_id;
        };

    add_scheduled(80, active_queue, JobState::Active, JobType::Cli, 90);
    add_scheduled(81, active_queue, JobState::Suspended, JobType::Cli, 90);
    add_scheduled(82, suspended_queue, JobState::Active, JobType::Cli, 90);
    add_scheduled(83, active_queue, JobState::Active, JobType::Http, 90);
    add_scheduled(84, active_queue, JobState::Active, JobType::Cli, 120);

    auto const manual_job = id(165);
    insert_job(fixture.database, manual_job, active_queue, JobState::Suspended);
    auto schedule        = default_run(fixture, id(85), manual_job, active_queue);
    schedule.runnable_at = at(150);
    schedule.planned_at  = at(150);
    insert_run(fixture.database, schedule);
    auto manual           = default_run(fixture, id(86), manual_job, active_queue);
    manual.origin         = RunOrigin::Manual;
    manual.schedule_owned = false;
    manual.runnable_at    = at(90);
    manual.planned_at     = at(90);
    insert_run(fixture.database, manual);

    auto const barrier_job = id(166);
    insert_job(fixture.database, barrier_job, active_queue);
    auto blocked        = default_run(fixture, id(87), barrier_job, active_queue);
    blocked.runnable_at = at(90);
    blocked.planned_at  = at(90);
    insert_run(fixture.database, blocked);
    auto barrier_manual           = default_run(fixture, id(88), barrier_job, active_queue);
    barrier_manual.origin         = RunOrigin::Manual;
    barrier_manual.schedule_owned = false;
    barrier_manual.runnable_at    = at(130);
    barrier_manual.planned_at     = at(130);
    insert_run(fixture.database, barrier_manual);

    auto const retry_job = id(167);
    insert_job(fixture.database, retry_job, active_queue);
    auto retry        = default_run(fixture, id(89), retry_job, active_queue);
    retry.state       = RunState::RetryWait;
    retry.started_at  = at(70);
    retry.runnable_at = at(90);
    insert_run(fixture.database, retry);
    insert_attempt(fixture.attempts, retry.id, 1, AttemptState::Completed);

    auto runnable = fixture.repository.list_runnable(active_queue, JobType::Cli, at(100), 20);
    REQUIRE(runnable);
    REQUIRE(runnable->size() == 3U);
    CHECK((*runnable)[0].run.id == id(80));
    CHECK((*runnable)[1].run.id == id(86));
    CHECK((*runnable)[2].run.id == id(89));
    CHECK((*runnable)[1].job_state == JobState::Suspended);

    auto earliest = fixture.repository.earliest_future_runnable(JobType::Cli, at(100));
    REQUIRE(earliest);
    REQUIRE(earliest->has_value());
    CHECK(**earliest == at(120));
    auto no_http_future = fixture.repository.earliest_future_runnable(JobType::Http, at(100));
    REQUIRE(no_http_future);
    CHECK_FALSE(no_http_future->has_value());
}

TEST_CASE("Scheduler repository detects running state in either durable table", "[jobu][scheduler][repository][sqlite]")
{
    RepositoryFixture fixture;
    auto              running = fixture.repository.has_any_running_state();
    REQUIRE(running);
    CHECK_FALSE(*running);

    auto const queue_id = id(100);
    auto const job_id   = id(101);
    auto const run_id   = id(102);
    insert_queue(fixture.database, queue_id);
    insert_job(fixture.database, job_id, queue_id);
    auto run       = default_run(fixture, run_id, job_id, queue_id);
    run.state      = RunState::Running;
    run.started_at = at(90);
    insert_run(fixture.database, run);
    running = fixture.repository.has_any_running_state();
    REQUIRE(running);
    CHECK(*running);

    execute(fixture.database, "UPDATE jobu_runs SET state = 'scheduled', started_at_us = NULL");
    insert_attempt(fixture.attempts, run_id, 1, AttemptState::Running);
    running = fixture.repository.has_any_running_state();
    REQUIRE(running);
    CHECK(*running);
}

TEST_CASE("Scheduler repository rejects malformed snapshots and contradictory attempt relationships",
          "[jobu][scheduler][repository][sqlite]")
{
    RepositoryFixture fixture;
    auto const        first_queue  = id(110);
    auto const        second_queue = id(111);
    insert_queue(fixture.database, first_queue);
    insert_queue(fixture.database, second_queue);

    SECTION("malformed materialized attributes")
    {
        auto const job_id = id(112);
        auto const run_id = id(113);
        insert_job(fixture.database, job_id, first_queue);
        insert_run(fixture.database, default_run(fixture, run_id, job_id, first_queue));
        execute(fixture.database, "UPDATE jobu_runs SET attributes_json = '{}' WHERE id IS NOT NULL");
        auto error = require_error(fixture.repository.list_runnable(first_queue, JobType::Cli, at(200), 10),
                                   ErrorCategory::Internal,
                                   "jobu.storage.invariant");
        CHECK(error.detail.find("cause=") != std::string::npos);
    }

    SECTION("run and job queue mismatch")
    {
        auto const job_id = id(114);
        auto const run_id = id(115);
        insert_job(fixture.database, job_id, first_queue);
        insert_run(fixture.database, default_run(fixture, run_id, job_id, second_queue));
        require_error(fixture.repository.list_runnable(second_queue, JobType::Cli, at(200), 10),
                      ErrorCategory::Internal,
                      "jobu.storage.invariant");
    }

    SECTION("scheduled candidate with a running attempt")
    {
        auto const job_id = id(116);
        auto const run_id = id(117);
        insert_job(fixture.database, job_id, first_queue);
        insert_run(fixture.database, default_run(fixture, run_id, job_id, first_queue));
        insert_attempt(fixture.attempts, run_id, 1, AttemptState::Running);
        require_error(fixture.repository.list_runnable(first_queue, JobType::Cli, at(200), 10),
                      ErrorCategory::Internal,
                      "jobu.storage.invariant");
    }

    SECTION("running run without one running attempt")
    {
        auto const job_id = id(118);
        auto const run_id = id(119);
        insert_job(fixture.database, job_id, first_queue);
        auto run       = default_run(fixture, run_id, job_id, first_queue);
        run.state      = RunState::Running;
        run.started_at = at(90);
        insert_run(fixture.database, run);
        require_error(fixture.repository.list_capacity_rows(10, std::nullopt),
                      ErrorCategory::Internal,
                      "jobu.storage.invariant");
    }

    SECTION("running run with duplicate running attempts")
    {
        auto const job_id = id(120);
        auto const run_id = id(121);
        insert_job(fixture.database, job_id, first_queue);
        auto run       = default_run(fixture, run_id, job_id, first_queue);
        run.state      = RunState::Running;
        run.started_at = at(90);
        insert_run(fixture.database, run);
        insert_attempt(fixture.attempts, run_id, 1, AttemptState::Running);
        insert_attempt(fixture.attempts, run_id, 2, AttemptState::Running);
        require_error(fixture.repository.list_capacity_rows(10, std::nullopt),
                      ErrorCategory::Internal,
                      "jobu.storage.invariant");
    }

    SECTION("retry-wait run with a non-failed completed attempt")
    {
        auto const job_id = id(122);
        auto const run_id = id(123);
        insert_job(fixture.database, job_id, first_queue);
        auto run        = default_run(fixture, run_id, job_id, first_queue);
        run.state       = RunState::RetryWait;
        run.started_at  = at(80);
        run.runnable_at = at(100);
        insert_run(fixture.database, run);
        insert_attempt(fixture.attempts, run_id, 1, AttemptState::Completed);

        SECTION("succeeded")
        {
            set_attempt_outcome(fixture.database, run_id, AttemptOutcome::Succeeded);
        }
        SECTION("interrupted")
        {
            set_attempt_outcome(fixture.database, run_id, AttemptOutcome::Interrupted);
        }
        SECTION("cancelled")
        {
            set_attempt_outcome(fixture.database, run_id, AttemptOutcome::Cancelled);
        }
        SECTION("missing")
        {
            set_attempt_outcome(fixture.database, run_id, std::nullopt);
        }

        require_error(fixture.repository.list_runnable(first_queue, JobType::Cli, at(200), 10),
                      ErrorCategory::Internal,
                      "jobu.storage.invariant");
        require_error(fixture.repository.list_capacity_rows(10, std::nullopt),
                      ErrorCategory::Internal,
                      "jobu.storage.invariant");
    }
}
