#include "management.hpp"

#include "attribute_registry.hpp"
#include "database.hpp"
#include "query.hpp"
#include "run_repository_priv.hpp"
#include "sqlite/sqlite_driver.hpp"
#include "sqlite/sqlite_schema.hpp"
#include "support/fake_cron_engine.hpp"
#include "support/fake_time_source.hpp"
#include "support/sequence_uuid_generator.hpp"
#include "support/temporary_directory.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace jb::core;
using namespace jb::db;
using namespace jb::jobu;
using namespace jb::test;
using namespace std::chrono_literals;

namespace {

auto sequence_id(std::uint8_t suffix) -> Uuid
{
    auto bytes = Uuid::Storage{};
    bytes[6]   = std::byte{0x70};
    bytes[8]   = std::byte{0x80};
    bytes[15]  = static_cast<std::byte>(suffix);
    return Uuid{bytes};
}

auto make_database(std::filesystem::path database_file) -> Database
{
    return Database{std::make_unique<jb::db::sqlite::Driver>(jb::db::sqlite::Options{
        .database_file = std::move(database_file),
        .busy_timeout  = 1000ms,
        .durability    = jb::db::sqlite::Durability::Normal,
    })};
}

struct ServiceFixture {
    explicit ServiceFixture(std::vector<Uuid> ids)
        : generator{std::move(ids)}
    {
        REQUIRE(database.open());
        REQUIRE(jb::jobu::sqlite::ensure_schema(database));
        time.set_utc(UtcTimePoint{10s});
    }

    TemporaryDirectory        directory;
    std::filesystem::path     database_file{directory.path() / "jobu.sqlite"};
    Database                  database{make_database(database_file)};
    StandardAttributeRegistry registry;
    FakeCronEngine            cron;
    SequenceUuidGenerator     generator;
    FakeTimeSource            time;
};

auto cron_schedule(std::string expression = "*/5 * * * *", std::string timezone = "UTC") -> CronSchedule
{
    return {.expression = std::move(expression), .timezone = std::move(timezone)};
}

auto cli_payload(std::string command) -> jb::rpc::JsonValue
{
    auto command_value = jb::rpc::JsonValue{};
    command_value.data = std::move(command);
    auto payload       = jb::rpc::JsonValue{};
    payload.data       = jb::rpc::JsonValue::Object{
        {"command", std::move(command_value)}
    };
    return payload;
}

auto max_attempts(std::int64_t value) -> AttributeSet
{
    return {
        {"retry.max_attempts", {.data = value}}
    };
}

auto injected_error(ErrorCategory category, std::string code) -> Error
{
    return {
        .category = category,
        .code     = std::move(code),
        .message  = "Injected cron test failure",
    };
}

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

auto count_rows(Database& database, std::string_view table) -> std::int64_t
{
    Query query{database};
    REQUIRE(query.exec("SELECT COUNT(*) AS row_count FROM " + std::string{table}));
    REQUIRE(query.next());
    auto const* value = query.record().value("row_count");
    REQUIRE(value != nullptr);
    auto const* count = std::get_if<std::int64_t>(value);
    REQUIRE(count != nullptr);
    return *count;
}

void check_schedule(JobSchedule const& value, CronSchedule const& expected)
{
    auto const* cron = std::get_if<CronSchedule>(&value);
    REQUIRE(cron != nullptr);
    CHECK(cron->expression == expected.expression);
    CHECK(cron->timezone == expected.timezone);
}

} // anonymous namespace

TEST_CASE("Recurring create persists one future schedule-owned snapshot", "[jobu][cron][management][sqlite]")
{
    auto const     queue_id = sequence_id(1);
    auto const     job_id   = sequence_id(2);
    auto const     run_id   = sequence_id(3);
    ServiceFixture fixture{
        {queue_id, job_id, run_id}
    };
    auto const schedule = cron_schedule();
    fixture.cron.set_occurrences(schedule, {UtcTimePoint{5s}, UtcTimePoint{60s}, UtcTimePoint{120s}});

    ManagementService service{fixture.database,
                              fixture.registry,
                              fixture.cron,
                              fixture.generator,
                              fixture.time,
                              max_attempts(2)};
    auto              queue = service.create_queue({.name = "recurring", .defaults = max_attempts(3)});
    REQUIRE(queue);

    auto const payload = cli_payload("/usr/bin/true");
    auto       created = service.create_job({
        .queue           = queue_id,
        .name            = "five-minute",
        .type            = JobType::Cli,
        .schedule        = schedule,
        .priority        = 7,
        .payload         = payload,
        .idempotency_key = "cron-create",
    });
    REQUIRE(created);
    CHECK(created->id == job_id);
    CHECK(created->queue_id == queue_id);
    CHECK(created->revision == 1);
    CHECK(created->priority == 7);
    CHECK(created->payload == payload);
    CHECK(created->created_at == UtcTimePoint{10s});
    check_schedule(created->schedule, schedule);
    CHECK(std::get<std::int64_t>(created->attributes.at("retry.max_attempts").data) == 3);

    REQUIRE(fixture.cron.validation_calls().size() == 1);
    CHECK(fixture.cron.validation_calls().front().expression == schedule.expression);
    REQUIRE(fixture.cron.next_calls().size() == 1);
    CHECK(fixture.cron.next_calls().front().exclusive_lower_bound == UtcTimePoint{10s});

    detail::RunRepository runs{fixture.database, fixture.registry};
    auto                  stored = runs.find_schedule_owned(job_id);
    REQUIRE(stored);
    REQUIRE(stored->has_value());
    CHECK((**stored).id == run_id);
    CHECK((**stored).job_revision == 1);
    CHECK((**stored).queue_id == queue_id);
    CHECK((**stored).origin == RunOrigin::Scheduled);
    CHECK((**stored).schedule_owned);
    CHECK((**stored).planned_at == UtcTimePoint{60s});
    CHECK((**stored).runnable_at == UtcTimePoint{60s});
    CHECK((**stored).state == RunState::Scheduled);
    CHECK((**stored).priority == 7);
    CHECK((**stored).payload == payload);
    CHECK(std::get<std::int64_t>((**stored).attributes.at("retry.max_attempts").data) == 3);

    CHECK(count_rows(fixture.database, "jobu_jobs") == 1);
    CHECK(count_rows(fixture.database, "jobu_runs") == 1);
    CHECK(count_rows(fixture.database, "jobu_attempts") == 0);
    CHECK(count_rows(fixture.database, "jobu_idempotency") == 1);
}

TEST_CASE("Recurring create rejects cron failures and rolls back all durable rows",
          "[jobu][cron][management][transaction][sqlite]")
{
    auto ids = std::vector<Uuid>{};
    for (std::uint8_t suffix = 1; suffix <= 9; ++suffix) {
        ids.push_back(sequence_id(suffix));
    }
    ServiceFixture    fixture{std::move(ids)};
    ManagementService service{fixture.database, fixture.registry, fixture.cron, fixture.generator, fixture.time};
    auto              queue = service.create_queue({.name = "recurring"});
    REQUIRE(queue);
    auto const schedule = cron_schedule();

    fixture.cron.set_validation_error(
        injected_error(ErrorCategory::InvalidArgument, "jobu.schedule.invalid_expression"));
    require_error(service.create_job({.queue = queue->id, .schedule = schedule, .payload = cli_payload("true")}),
                  ErrorCategory::InvalidArgument,
                  "jobu.schedule.invalid_expression");
    CHECK(count_rows(fixture.database, "jobu_jobs") == 0);
    CHECK(count_rows(fixture.database, "jobu_runs") == 0);

    fixture.cron.set_validation_error(injected_error(ErrorCategory::InvalidArgument, "jobu.schedule.invalid_timezone"));
    require_error(service.create_job({.queue    = queue->id,
                                      .schedule = cron_schedule("*/5 * * * *", "Missing/Zone"),
                                      .payload  = cli_payload("true")}),
                  ErrorCategory::InvalidArgument,
                  "jobu.schedule.invalid_timezone");
    CHECK(count_rows(fixture.database, "jobu_jobs") == 0);
    CHECK(count_rows(fixture.database, "jobu_runs") == 0);

    fixture.cron.set_validation_error(std::nullopt);
    fixture.cron.set_occurrences(schedule, {UtcTimePoint{60s}});
    fixture.cron.set_next_error(injected_error(ErrorCategory::InvalidArgument, "jobu.schedule.no_future_occurrence"));
    require_error(service.create_job({.queue = queue->id, .schedule = schedule, .payload = cli_payload("true")}),
                  ErrorCategory::InvalidArgument,
                  "jobu.schedule.no_future_occurrence");
    CHECK(count_rows(fixture.database, "jobu_jobs") == 0);
    CHECK(count_rows(fixture.database, "jobu_runs") == 0);

    fixture.cron.set_next_error(std::nullopt);
    execute(fixture.database,
            "CREATE TRIGGER fail_cron_idempotency_insert BEFORE INSERT ON jobu_idempotency "
            "WHEN NEW.key = 'fail-key' BEGIN SELECT RAISE(ABORT, 'injected failure'); END");
    require_error(service.create_job({
                      .queue           = queue->id,
                      .schedule        = schedule,
                      .payload         = cli_payload("true"),
                      .idempotency_key = "fail-key",
                  }),
                  ErrorCategory::Conflict,
                  "db.constraint");
    CHECK(count_rows(fixture.database, "jobu_jobs") == 0);
    CHECK(count_rows(fixture.database, "jobu_runs") == 0);
    CHECK(count_rows(fixture.database, "jobu_idempotency") == 0);
}

TEST_CASE("Recurring create idempotency preserves its first occurrence across restart",
          "[jobu][cron][management][idempotency][restart][sqlite]")
{
    auto const     queue_id = sequence_id(1);
    auto const     job_id   = sequence_id(2);
    auto const     run_id   = sequence_id(3);
    ServiceFixture fixture{
        {queue_id, job_id, run_id}
    };
    auto const schedule = cron_schedule("15 * * * *", "Europe/Tallinn");
    fixture.cron.set_occurrences(schedule, {UtcTimePoint{60s}, UtcTimePoint{3600s}});

    auto request = CreateJobRequest{
        .queue           = queue_id,
        .name            = "replayed",
        .schedule        = schedule,
        .payload         = cli_payload("true"),
        .idempotency_key = "stable-cron",
    };
    {
        ManagementService service{fixture.database, fixture.registry, fixture.cron, fixture.generator, fixture.time};
        REQUIRE(service.create_queue({.name = "recurring"}));
        auto original = service.create_job(request);
        REQUIRE(original);
        CHECK(original->id == job_id);

        fixture.time.advance(2h);
        fixture.cron.set_occurrences(schedule, {UtcTimePoint{10800s}});
        auto replay = service.create_job(request);
        REQUIRE(replay);
        CHECK(replay->id == original->id);
        CHECK(replay->created_at == original->created_at);
        check_schedule(replay->schedule, schedule);
        CHECK(fixture.cron.validation_calls().size() == 1);
        CHECK(fixture.cron.next_calls().size() == 1);

        auto changed_expression                                        = request;
        std::get<CronSchedule>(changed_expression.schedule).expression = "30 * * * *";
        require_error(service.create_job(std::move(changed_expression)),
                      ErrorCategory::Conflict,
                      "jobu.idempotency.conflict");
        auto changed_timezone                                      = request;
        std::get<CronSchedule>(changed_timezone.schedule).timezone = "UTC";
        require_error(service.create_job(std::move(changed_timezone)),
                      ErrorCategory::Conflict,
                      "jobu.idempotency.conflict");
        CHECK(fixture.cron.validation_calls().size() == 1);
        CHECK(fixture.cron.next_calls().size() == 1);
    }

    REQUIRE(fixture.database.close());
    REQUIRE(fixture.database.open());
    auto schema = jb::jobu::sqlite::ensure_schema(fixture.database);
    REQUIRE(schema);
    CHECK_FALSE(schema->created);

    fixture.cron.set_validation_error(injected_error(ErrorCategory::Internal, "test.cron.must_not_validate_replay"));
    {
        ManagementService service{fixture.database, fixture.registry, fixture.cron, fixture.generator, fixture.time};
        auto              replay = service.create_job(request);
        REQUIRE(replay);
        CHECK(replay->id == job_id);
        CHECK(replay->created_at == UtcTimePoint{10s});
        check_schedule(replay->schedule, schedule);
    }
    CHECK(fixture.cron.validation_calls().size() == 1);
    CHECK(fixture.cron.next_calls().size() == 1);

    detail::RunRepository runs{fixture.database, fixture.registry};
    auto                  stored = runs.find_schedule_owned(job_id);
    REQUIRE(stored);
    REQUIRE(stored->has_value());
    CHECK((**stored).id == run_id);
    CHECK((**stored).planned_at == UtcTimePoint{60s});
    CHECK((**stored).runnable_at == UtcTimePoint{60s});
    CHECK(count_rows(fixture.database, "jobu_jobs") == 1);
    CHECK(count_rows(fixture.database, "jobu_runs") == 1);
    CHECK(count_rows(fixture.database, "jobu_idempotency") == 1);
}
