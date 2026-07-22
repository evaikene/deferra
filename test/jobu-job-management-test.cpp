#include "management.hpp"

#include "attribute_codec_priv.hpp"
#include "attribute_registry.hpp"
#include "database.hpp"
#include "domain_storage_priv.hpp"
#include "query.hpp"
#include "run_repository_priv.hpp"
#include "sqlite/sqlite_driver.hpp"
#include "sqlite/sqlite_schema.hpp"
#include "support/fake_time_source.hpp"
#include "support/sequence_uuid_generator.hpp"
#include "support/temporary_directory.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
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

auto uuid(std::string_view text) -> Uuid
{
    auto parsed = Uuid::parse(text);
    REQUIRE(parsed);
    return *parsed;
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
    SequenceUuidGenerator     generator;
    FakeTimeSource            time;
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

auto json_string(std::string value) -> jb::rpc::JsonValue
{
    auto json = jb::rpc::JsonValue{};
    json.data = std::move(value);
    return json;
}

auto json_bool(bool value) -> jb::rpc::JsonValue
{
    auto json = jb::rpc::JsonValue{};
    json.data = value;
    return json;
}

auto json_array(jb::rpc::JsonValue::Array value) -> jb::rpc::JsonValue
{
    auto json = jb::rpc::JsonValue{};
    json.data = std::move(value);
    return json;
}

auto json_object(jb::rpc::JsonValue::Object value) -> jb::rpc::JsonValue
{
    auto json = jb::rpc::JsonValue{};
    json.data = std::move(value);
    return json;
}

auto cli_payload(std::string command, std::vector<std::string> arguments = {}) -> jb::rpc::JsonValue
{
    auto argument_values = jb::rpc::JsonValue::Array{};
    argument_values.reserve(arguments.size());
    for (auto& argument : arguments) {
        argument_values.push_back(json_string(std::move(argument)));
    }
    return json_object({
        {"arguments", json_array(std::move(argument_values))},
        {"command",   json_string(std::move(command))       },
        {"future",    json_bool(true)                       },
    });
}

auto http_payload(std::string url, std::optional<std::string> method = std::nullopt) -> jb::rpc::JsonValue
{
    auto object = jb::rpc::JsonValue::Object{
        {"future", json_bool(true)            },
        {"url",    json_string(std::move(url))},
    };
    if (method) {
        object.emplace("method", json_string(std::move(*method)));
    }
    return json_object(std::move(object));
}

auto max_attempts(std::int64_t value) -> AttributeSet
{
    return {
        {"retry.max_attempts", {.data = value}}
    };
}

auto once_at(UtcTimePoint time) -> JobSchedule
{
    return OnceSchedule{.planned_at = time};
}

auto attributes_json(AttributeSet const& values, AttributeRegistry const& registry) -> jb::rpc::JsonValue
{
    auto encoded = attribute_set_to_json(values, registry, AttributeScope::Job);
    REQUIRE(encoded);
    return std::move(encoded).value();
}

struct StoredJobRunDocuments {
    std::string job_attributes;
    std::string run_attributes;
    std::string job_payload;
    std::string run_payload;
};

auto stored_documents(Database& database, Uuid const& job_id) -> StoredJobRunDocuments
{
    Query query{database};
    REQUIRE(query.prepare("SELECT jobs.attributes_json AS job_attributes, runs.attributes_json AS run_attributes, "
                          "jobs.payload_json AS job_payload, runs.payload_json AS run_payload FROM jobu_jobs AS jobs "
                          "JOIN jobu_runs AS runs ON runs.job_id = jobs.id WHERE jobs.id = :job_id"));
    REQUIRE(query.bind_value(":job_id", jb::jobu::detail::uuid_to_storage(job_id)));
    REQUIRE(query.exec());
    REQUIRE(query.next());
    auto text = [&query](std::string_view field) {
        auto const* value = query.value(field);
        REQUIRE(value != nullptr);
        auto const* stored = std::get_if<std::string>(value);
        REQUIRE(stored != nullptr);
        return *stored;
    };
    return {
        .job_attributes = text("job_attributes"),
        .run_attributes = text("run_attributes"),
        .job_payload    = text("job_payload"),
        .run_payload    = text("run_payload"),
    };
}

class LargeAttributeRegistry final : public AttributeRegistry {
public:
    [[nodiscard]] auto find(std::string_view name) const noexcept -> AttributeDefinition const* override
    {
        for (auto const& definition : _definitions) {
            if (definition.name == name) {
                return &definition;
            }
        }
        return nullptr;
    }

    [[nodiscard]] auto validate(std::string_view name, AttributeValue const& value, AttributeScope scope) const
        -> Result<void, Error> override
    {
        auto const* definition = find(name);
        if (definition == nullptr) {
            return Result<void, Error>::failure({
                .category = ErrorCategory::InvalidArgument,
                .code     = "jobu.attribute.unknown",
                .message  = "Unknown test attribute",
            });
        }
        if (!definition->scopes.test(scope) || !std::holds_alternative<std::string>(value.data)) {
            return Result<void, Error>::failure({
                .category = ErrorCategory::InvalidArgument,
                .code     = "jobu.attribute.invalid_value",
                .message  = "Invalid test attribute",
            });
        }
        return Result<void, Error>::success();
    }

    [[nodiscard]] auto definitions() const -> std::span<AttributeDefinition const> override { return {_definitions}; }

private:
    static auto definition(std::string name) -> AttributeDefinition
    {
        return {
            .name             = std::move(name),
            .type             = AttributeType::String,
            .scopes           = {AttributeScope::DaemonDefault, AttributeScope::QueueDefault, AttributeScope::Job},
            .built_in_default = {.data = std::string{}},
            .description      = "Large string used for materialized size tests",
        };
    }

    std::array<AttributeDefinition, 2> _definitions{definition("test.queue_large"), definition("test.job_large")};
};

} // anonymous namespace

TEST_CASE("Job management creates durable one-time definitions and immutable run snapshots", "[jobu][job][sqlite]")
{
    auto const     queue_id = uuid("00000000-0000-7000-8000-000000000001");
    auto const     cli_id   = uuid("00000000-0000-7000-8000-000000000002");
    auto const     cli_run  = uuid("00000000-0000-7000-8000-000000000003");
    auto const     http_id  = uuid("00000000-0000-7000-8000-000000000004");
    auto const     http_run = uuid("00000000-0000-7000-8000-000000000005");
    ServiceFixture fixture{
        {queue_id, cli_id, cli_run, http_id, http_run}
    };
    ManagementService service{fixture.database, fixture.registry, fixture.generator, fixture.time, max_attempts(2)};

    auto queue = service.create_queue({.name = "jobs", .defaults = max_attempts(3)});
    REQUIRE(queue);

    auto const planned        = UtcTimePoint{-5s};
    auto const payload        = cli_payload("/usr/bin/example", {"--dry-run", "value"});
    auto       create_request = CreateJobRequest{
        .queue           = std::string{"jobs"},
        .name            = "cli-job",
        .type            = JobType::Cli,
        .schedule        = once_at(planned),
        .priority        = -7,
        .attributes      = max_attempts(4),
        .payload         = payload,
        .idempotency_key = "create-cli",
    };
    auto cli = service.create_job(create_request);
    REQUIRE(cli);
    CHECK(create_request.name == "cli-job");
    CHECK(create_request.payload == payload);
    CHECK(cli->id == cli_id);
    CHECK(cli->queue_id == queue_id);
    CHECK(cli->revision == 1);
    CHECK(cli->state == JobState::Active);
    CHECK(cli->type == JobType::Cli);
    CHECK(cli->priority == -7);
    CHECK(cli->payload == payload);
    CHECK(cli->created_at == UtcTimePoint{10s});
    CHECK(cli->updated_at == cli->created_at);
    CHECK_FALSE(cli->deleted_at);
    CHECK(std::get<OnceSchedule>(cli->schedule).planned_at == planned);
    CHECK(cli->attributes.size() == fixture.registry.definitions().size());
    CHECK(std::get<std::int64_t>(cli->attributes.at("retry.max_attempts").data) == 4);

    auto const documents = stored_documents(fixture.database, cli_id);
    CHECK(documents.job_attributes == documents.run_attributes);
    CHECK(documents.job_payload == documents.run_payload);
    auto parsed_attributes = jb::rpc::parse_json(documents.job_attributes);
    REQUIRE(parsed_attributes);
    auto decoded_attributes = jb::jobu::detail::decode_attribute_document(fixture.registry,
                                                                          *parsed_attributes,
                                                                          AttributeScope::Job,
                                                                          detail::AttributeDocumentMode::Materialized);
    REQUIRE(decoded_attributes);
    CHECK(attributes_json(*decoded_attributes, fixture.registry) == attributes_json(cli->attributes, fixture.registry));

    auto fetched = service.get_job(cli_id);
    REQUIRE(fetched);
    CHECK(fetched->id == cli->id);
    CHECK(fetched->queue_id == cli->queue_id);
    CHECK(fetched->revision == cli->revision);
    CHECK(fetched->name == cli->name);
    CHECK(fetched->state == cli->state);
    CHECK(fetched->type == cli->type);
    CHECK(std::get<OnceSchedule>(fetched->schedule).planned_at == planned);
    CHECK(fetched->priority == cli->priority);
    CHECK(attributes_json(fetched->attributes, fixture.registry) == attributes_json(cli->attributes, fixture.registry));
    CHECK(fetched->payload == cli->payload);
    CHECK(fetched->created_at == cli->created_at);
    CHECK(fetched->updated_at == cli->updated_at);
    CHECK(fetched->deleted_at == cli->deleted_at);

    detail::RunRepository runs{fixture.database, fixture.registry};
    auto                  stored_run = runs.find_schedule_owned(cli_id);
    REQUIRE(stored_run);
    REQUIRE(stored_run->has_value());
    CHECK((**stored_run).id == cli_run);
    CHECK((**stored_run).job_revision == cli->revision);
    CHECK((**stored_run).queue_id == cli->queue_id);
    CHECK((**stored_run).origin == RunOrigin::Scheduled);
    CHECK((**stored_run).schedule_owned);
    CHECK((**stored_run).planned_at == planned);
    CHECK((**stored_run).runnable_at == planned);
    CHECK((**stored_run).type == cli->type);
    CHECK((**stored_run).priority == cli->priority);
    CHECK(attributes_json((**stored_run).attributes, fixture.registry) ==
          attributes_json(cli->attributes, fixture.registry));
    CHECK((**stored_run).payload == cli->payload);
    CHECK((**stored_run).state == RunState::Scheduled);
    CHECK_FALSE((**stored_run).started_at);
    CHECK_FALSE((**stored_run).completed_at);
    CHECK_FALSE((**stored_run).result);

    fixture.time.advance(1s);
    auto http = service.create_job({
        .queue    = queue_id,
        .name     = "http-job",
        .type     = JobType::Http,
        .schedule = once_at(UtcTimePoint{20s}),
        .priority = 9,
        .payload  = http_payload("https://example.test/path"),
    });
    REQUIRE(http);
    CHECK(http->id == http_id);
    CHECK(http->created_at == UtcTimePoint{11s});
    CHECK(std::get<std::int64_t>(http->attributes.at("retry.max_attempts").data) == 3);

    auto http_stored_run = runs.find_schedule_owned(http_id);
    REQUIRE(http_stored_run);
    REQUIRE(http_stored_run->has_value());
    CHECK((**http_stored_run).id == http_run);
    CHECK((**http_stored_run).payload == http->payload);
    CHECK(count_rows(fixture.database, "jobu_jobs") == 2);
    CHECK(count_rows(fixture.database, "jobu_runs") == 2);
    CHECK(count_rows(fixture.database, "jobu_attempts") == 0);
}

TEST_CASE("Job management lists filtered keyset pages and controls deleted visibility", "[jobu][job][sqlite]")
{
    auto const     first_queue  = uuid("00000000-0000-7000-8000-000000000010");
    auto const     second_queue = uuid("00000000-0000-7000-8000-000000000011");
    auto const     first_job    = uuid("00000000-0000-7000-8000-000000000012");
    auto const     first_run    = uuid("00000000-0000-7000-8000-000000000013");
    auto const     second_job   = uuid("00000000-0000-7000-8000-000000000014");
    auto const     second_run   = uuid("00000000-0000-7000-8000-000000000015");
    auto const     third_job    = uuid("00000000-0000-7000-8000-000000000016");
    auto const     third_run    = uuid("00000000-0000-7000-8000-000000000017");
    ServiceFixture fixture{
        {first_queue, second_queue, first_job, first_run, second_job, second_run, third_job, third_run}
    };
    ManagementService service{fixture.database, fixture.registry, fixture.generator, fixture.time};
    REQUIRE(service.create_queue({.name = "first"}));
    REQUIRE(service.create_queue({.name = "second"}));

    REQUIRE(service.create_job({.queue    = std::string{"first"},
                                .type     = JobType::Cli,
                                .schedule = once_at(UtcTimePoint{1s}),
                                .payload  = cli_payload("one")}));
    REQUIRE(service.create_job({.queue    = std::string{"first"},
                                .type     = JobType::Http,
                                .schedule = once_at(UtcTimePoint{2s}),
                                .payload  = http_payload("https://two.test", "POST")}));
    REQUIRE(service.create_job({.queue    = std::string{"second"},
                                .type     = JobType::Cli,
                                .schedule = once_at(UtcTimePoint{3s}),
                                .payload  = cli_payload("three")}));

    auto page = service.list_jobs({.page = {.limit = 2}});
    REQUIRE(page);
    REQUIRE(page->items.size() == 2);
    CHECK(page->items[0].id == first_job);
    CHECK(page->items[1].id == second_job);
    REQUIRE(page->next_after_id);
    CHECK(*page->next_after_id == second_job);

    auto tail = service.list_jobs({
        .page = {.limit = 2, .after_id = page->next_after_id}
    });
    REQUIRE(tail);
    REQUIRE(tail->items.size() == 1);
    CHECK(tail->items.front().id == third_job);
    CHECK_FALSE(tail->next_after_id);

    auto first_queue_jobs = service.list_jobs({.queue = std::string{"first"}, .page = {.limit = 10}});
    REQUIRE(first_queue_jobs);
    CHECK(first_queue_jobs->items.size() == 2);
    auto cli_jobs = service.list_jobs({.type = JobType::Cli, .page = {.limit = 10}});
    REQUIRE(cli_jobs);
    CHECK(cli_jobs->items.size() == 2);
    auto active_jobs = service.list_jobs({.state = JobState::Active, .page = {.limit = 10}});
    REQUIRE(active_jobs);
    CHECK(active_jobs->items.size() == 3);

    execute(fixture.database,
            "UPDATE jobu_jobs SET revision = revision + 1, state = 'deleted', updated_at_us = 20, "
            "deleted_at_us = 20 WHERE queue_id = X'00000000000070008000000000000010'");
    execute(fixture.database,
            "UPDATE jobu_runs SET state = 'cancelled', completed_at_us = 20, "
            "result_json = '{\"reason\":\"queue_deleted\"}' WHERE queue_id = "
            "X'00000000000070008000000000000010'");
    execute(fixture.database,
            "UPDATE jobu_queues SET name = 'first-deleted#00000000-0000-7000-8000-000000000010', "
            "deleted_name = 'first', state = 'deleted', deleted_at_us = 20 WHERE id = "
            "X'00000000000070008000000000000010'");

    require_error(service.get_job(first_job), ErrorCategory::NotFound, "jobu.job.not_found");
    auto deleted = service.get_job(first_job, true);
    REQUIRE(deleted);
    CHECK(deleted->state == JobState::Deleted);
    REQUIRE(deleted->deleted_at);

    require_error(service.list_jobs({.queue = first_queue, .page = {.limit = 10}}),
                  ErrorCategory::NotFound,
                  "jobu.queue.not_found");
    auto historical = service.list_jobs(
        {.queue = std::string{"first"}, .include_deleted = true, .state = JobState::Deleted, .page = {.limit = 10}});
    REQUIRE(historical);
    REQUIRE(historical->items.size() == 2);
    CHECK(historical->items.front().id == first_job);
}

TEST_CASE("Job management rejects invalid requests before durable creation", "[jobu][job][validation]")
{
    auto const     queue_id = uuid("00000000-0000-7000-8000-000000000020");
    ServiceFixture fixture{
        {queue_id,
         uuid("00000000-0000-7000-8000-000000000021"),
         uuid("00000000-0000-7000-8000-000000000022"),
         uuid("00000000-0000-7000-8000-000000000023"),
         uuid("00000000-0000-7000-8000-000000000024"),
         uuid("00000000-0000-7000-8000-000000000025"),
         uuid("00000000-0000-7000-8000-000000000026")}
    };
    ManagementService service{fixture.database, fixture.registry, fixture.generator, fixture.time};
    REQUIRE(service.create_queue({.name = "jobs"}));
    auto const schedule = once_at(UtcTimePoint{1s});

    require_error(service.create_job({
                      .queue    = queue_id,
                      .name     = std::string{"bad\x01name", 8},
                      .schedule = schedule,
                      .payload  = cli_payload("true")
    }),
                  ErrorCategory::InvalidArgument,
                  "jobu.job.invalid_name");
    require_error(
        service.create_job(
            {.queue = queue_id, .name = std::string(257, 'x'), .schedule = schedule, .payload = cli_payload("true")}),
        ErrorCategory::InvalidArgument,
        "jobu.job.invalid_name");
    require_error(service.create_job({
                      .queue    = queue_id,
                      .name     = std::string{"bad\xC3", 4},
                      .schedule = schedule,
                      .payload  = cli_payload("true")
    }),
                  ErrorCategory::InvalidArgument,
                  "jobu.job.invalid_name");
    require_error(
        service.create_job(
            {.queue = queue_id, .schedule = CronSchedule{.expression = "* * * * *"}, .payload = cli_payload("true")}),
        ErrorCategory::Unsupported,
        "jobu.schedule.cron_unavailable");
    require_error(service.create_job({.queue = queue_id, .schedule = schedule, .payload = jb::rpc::JsonValue{}}),
                  ErrorCategory::InvalidArgument,
                  "jobu.job.invalid_payload");
    require_error(service.create_job({.queue = queue_id, .schedule = schedule, .payload = json_object({})}),
                  ErrorCategory::InvalidArgument,
                  "jobu.job.invalid_payload");
    require_error(
        service.create_job({
            .queue    = queue_id,
            .schedule = schedule,
            .payload  = json_object({{"arguments", json_string("not-array")}, {"command", json_string("x")}}
               )
    }),
        ErrorCategory::InvalidArgument,
        "jobu.job.invalid_payload");
    require_error(service.create_job({
                      .queue    = queue_id,
                      .schedule = schedule,
                      .payload  = json_object({
                                               {"arguments", json_array({json_bool(true)})},
                                               {"command", json_string("x")},
                                               }
                         ),
    }),
                  ErrorCategory::InvalidArgument,
                  "jobu.job.invalid_payload");
    require_error(service.create_job({
                      .queue    = queue_id,
                      .type     = JobType::Http,
                      .schedule = schedule,
                      .payload  = json_object({{"method", json_string("GET")}}
                         )
    }),
                  ErrorCategory::InvalidArgument,
                  "jobu.job.invalid_payload");
    require_error(service.create_job({
                      .queue    = queue_id,
                      .type     = JobType::Http,
                      .schedule = schedule,
                      .payload  = json_object({{"method", json_string("")}, {"url", json_string("https://x")}}
                         )
    }),
                  ErrorCategory::InvalidArgument,
                  "jobu.job.invalid_payload");
    require_error(
        service.create_job(
            {.queue = queue_id, .type = static_cast<JobType>(99), .schedule = schedule, .payload = cli_payload("x")}),
        ErrorCategory::InvalidArgument,
        "jobu.job.invalid_payload");
    require_error(
        service.create_job(
            {.queue = queue_id, .schedule = schedule, .payload = cli_payload(std::string(256U * 1024U, 'x'))}),
        ErrorCategory::ResourceExhausted,
        "jobu.protocol.value_too_large");
    require_error(service.create_job(
                      {.queue = queue_id, .schedule = schedule, .payload = cli_payload("x"), .idempotency_key = ""}),
                  ErrorCategory::InvalidArgument,
                  "jobu.idempotency.invalid_key");
    require_error(service.create_job({.queue      = queue_id,
                                      .schedule   = schedule,
                                      .attributes = {{"unknown", {.data = true}}},
                                      .payload    = cli_payload("x")}),
                  ErrorCategory::InvalidArgument,
                  "jobu.attribute.unknown");

    require_error(
        service.create_job(
            {.queue = uuid("00000000-0000-7000-8000-000000000099"), .schedule = schedule, .payload = cli_payload("x")}),
        ErrorCategory::NotFound,
        "jobu.queue.not_found");
    execute(fixture.database, "UPDATE jobu_queues SET state = 'suspending' WHERE name = 'jobs'");
    require_error(service.create_job({.queue = queue_id, .schedule = schedule, .payload = cli_payload("x")}),
                  ErrorCategory::Conflict,
                  "jobu.queue.state_conflict");
    execute(fixture.database, "UPDATE jobu_queues SET state = 'suspended' WHERE name = 'jobs'");
    auto suspended = service.create_job({.queue = queue_id, .schedule = schedule, .payload = cli_payload("x")});
    REQUIRE(suspended);

    require_error(service.list_jobs({.page = {.limit = 0}}),
                  ErrorCategory::InvalidArgument,
                  "jobu.job.invalid_configuration");
    require_error(service.list_jobs({.page = {.limit = 201}}),
                  ErrorCategory::InvalidArgument,
                  "jobu.job.invalid_configuration");
    require_error(service.list_jobs({.state = static_cast<JobState>(99)}),
                  ErrorCategory::InvalidArgument,
                  "jobu.job.invalid_configuration");
    require_error(service.list_jobs({.type = static_cast<JobType>(99)}),
                  ErrorCategory::InvalidArgument,
                  "jobu.job.invalid_configuration");
}

TEST_CASE("Job creation rolls back the definition when schedule-run insertion fails", "[jobu][job][transaction]")
{
    auto const     queue_id        = uuid("00000000-0000-7000-8000-000000000030");
    auto const     first_job       = uuid("00000000-0000-7000-8000-000000000031");
    auto const     shared_run      = uuid("00000000-0000-7000-8000-000000000032");
    auto const     rolled_back_job = uuid("00000000-0000-7000-8000-000000000033");
    ServiceFixture fixture{
        {queue_id, first_job, shared_run, rolled_back_job, shared_run}
    };
    ManagementService service{fixture.database, fixture.registry, fixture.generator, fixture.time};
    REQUIRE(service.create_queue({.name = "jobs"}));
    REQUIRE(service.create_job(
        {.queue = queue_id, .schedule = once_at(UtcTimePoint{1s}), .payload = cli_payload("first")}));

    require_error(service.create_job(
                      {.queue = queue_id, .schedule = once_at(UtcTimePoint{2s}), .payload = cli_payload("second")}),
                  ErrorCategory::Conflict,
                  "db.constraint.unique");
    require_error(service.get_job(rolled_back_job), ErrorCategory::NotFound, "jobu.job.not_found");
    CHECK(count_rows(fixture.database, "jobu_jobs") == 1);
    CHECK(count_rows(fixture.database, "jobu_runs") == 1);
}

TEST_CASE("Job creation rejects oversized materialized attribute snapshots", "[jobu][job][validation]")
{
    auto const     queue_id = uuid("00000000-0000-7000-8000-000000000060");
    ServiceFixture fixture{
        {queue_id, uuid("00000000-0000-7000-8000-000000000061"), uuid("00000000-0000-7000-8000-000000000062")}
    };
    LargeAttributeRegistry registry;
    ManagementService      service{fixture.database, registry, fixture.generator, fixture.time};

    auto queue_defaults = AttributeSet{
        {"test.queue_large", {.data = std::string(140U * 1024U, 'q')}}
    };
    REQUIRE(service.create_queue({.name = "large", .defaults = std::move(queue_defaults)}));

    auto job_attributes = AttributeSet{
        {"test.job_large", {.data = std::string(140U * 1024U, 'j')}}
    };
    auto oversized_error = require_error(service.create_job({.queue      = queue_id,
                                                             .schedule   = once_at(UtcTimePoint{1s}),
                                                             .attributes = std::move(job_attributes),
                                                             .payload    = cli_payload("true")}),
                                         ErrorCategory::ResourceExhausted,
                                         "jobu.protocol.value_too_large");
    CHECK(oversized_error.message == "Job attribute document exceeds its size limit");
    CHECK(count_rows(fixture.database, "jobu_jobs") == 0);
    CHECK(count_rows(fixture.database, "jobu_runs") == 0);
}

TEST_CASE("Job management rejects malformed persisted definitions", "[jobu][job][sqlite]")
{
    auto const     queue_id = uuid("00000000-0000-7000-8000-000000000040");
    auto const     job_id   = uuid("00000000-0000-7000-8000-000000000041");
    ServiceFixture fixture{
        {queue_id, job_id, uuid("00000000-0000-7000-8000-000000000042")}
    };
    ManagementService service{fixture.database, fixture.registry, fixture.generator, fixture.time};
    REQUIRE(service.create_queue({.name = "jobs"}));
    REQUIRE(
        service.create_job({.queue = queue_id, .schedule = once_at(UtcTimePoint{1s}), .payload = cli_payload("true")}));

    execute(fixture.database, "UPDATE jobu_jobs SET payload_json = '{}' WHERE id IS NOT NULL");
    require_error(service.get_job(job_id), ErrorCategory::Internal, "jobu.storage.invariant");
    require_error(service.list_jobs({}), ErrorCategory::Internal, "jobu.storage.invariant");

    execute(fixture.database, "UPDATE jobu_jobs SET payload_json = '{\"command\":\"true\"}', name = char(1)");
    require_error(service.get_job(job_id), ErrorCategory::Internal, "jobu.storage.invariant");
}

TEST_CASE("Job methods report invalid daemon defaults consistently", "[jobu][job][validation]")
{
    ServiceFixture fixture{
        {uuid("00000000-0000-7000-8000-000000000050"),
         uuid("00000000-0000-7000-8000-000000000051"),
         uuid("00000000-0000-7000-8000-000000000052")}
    };
    ManagementService queue_service{fixture.database, fixture.registry, fixture.generator, fixture.time};
    auto              queue = queue_service.create_queue({.name = "jobs"});
    REQUIRE(queue);

    ManagementService invalid_service{fixture.database,
                                      fixture.registry,
                                      fixture.generator,
                                      fixture.time,
                                      {{"unknown", {.data = true}}}};
    require_error(invalid_service.create_job(
                      {.queue = queue->id, .schedule = once_at(UtcTimePoint{1s}), .payload = cli_payload("true")}),
                  ErrorCategory::InvalidArgument,
                  "jobu.attribute.unknown");
    require_error(invalid_service.get_job(uuid("00000000-0000-7000-8000-000000000099")),
                  ErrorCategory::InvalidArgument,
                  "jobu.attribute.unknown");
    require_error(invalid_service.list_jobs({}), ErrorCategory::InvalidArgument, "jobu.attribute.unknown");
}
