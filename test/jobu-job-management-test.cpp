#include "management.hpp"

#include "attempt_repository_priv.hpp"
#include "attribute_codec_priv.hpp"
#include "attribute_registry.hpp"
#include "database.hpp"
#include "domain_storage_priv.hpp"
#include "query.hpp"
#include "run_repository_priv.hpp"
#include "sqlite/sqlite_driver.hpp"
#include "sqlite/sqlite_schema.hpp"
#include "support/fake_cron_engine.hpp"
#include "support/fake_time_source.hpp"
#include "support/sequence_uuid_generator.hpp"
#include "support/temporary_directory.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
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
using namespace jb::test;
using namespace std::chrono_literals;

namespace {

auto uuid(std::string_view text) -> Uuid
{
    auto parsed = Uuid::parse(text);
    REQUIRE(parsed);
    return *parsed;
}

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
    ManagementService service{fixture.database,
                              fixture.registry,
                              fixture.cron,
                              fixture.generator,
                              fixture.time,
                              max_attempts(2)};

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

TEST_CASE("Job create idempotency is queue-scoped and replays the original definition", "[jobu][job][idempotency]")
{
    auto ids = std::vector<Uuid>{};
    for (std::uint8_t suffix = 1; suffix <= 14; ++suffix) {
        ids.push_back(sequence_id(suffix));
    }
    ServiceFixture    fixture{std::move(ids)};
    ManagementService service{fixture.database, fixture.registry, fixture.cron, fixture.generator, fixture.time};

    auto first_queue = service.create_queue({.name = "first"});
    REQUIRE(first_queue);
    auto request = CreateJobRequest{
        .queue           = first_queue->id,
        .name            = "replay",
        .type            = JobType::Cli,
        .schedule        = once_at(UtcTimePoint{20s}),
        .priority        = 0,
        .attributes      = {},
        .payload         = cli_payload("true"),
        .idempotency_key = "job-key",
    };
    auto original = service.create_job(request);
    REQUIRE(original);
    CHECK(original->id == sequence_id(2));
    auto updated = service.update_job({.job_id = original->id, .expected_revision = 1, .priority = 7});
    REQUIRE(updated);
    CHECK(updated->revision == 2);

    auto replay = service.create_job(request);
    REQUIRE(replay);
    CHECK(replay->id == original->id);
    CHECK(replay->revision == 1);
    CHECK(replay->priority == 0);
    CHECK(count_rows(fixture.database, "jobu_jobs") == 1);
    CHECK(count_rows(fixture.database, "jobu_runs") == 1);

    auto different     = request;
    different.priority = 1;
    require_error(service.create_job(std::move(different)), ErrorCategory::Conflict, "jobu.idempotency.conflict");

    auto second_queue = service.create_queue({.name = "second"});
    REQUIRE(second_queue);
    auto second_scope  = request;
    second_scope.queue = second_queue->id;
    auto second_job    = service.create_job(std::move(second_scope));
    REQUIRE(second_job);
    CHECK(second_job->id != original->id);
    CHECK(count_rows(fixture.database, "jobu_idempotency") == 2);

    execute(fixture.database,
            "UPDATE jobu_idempotency SET result_json = '{}' WHERE method = 'job.create' AND key = 'job-key' "
            "AND resource_id = X'00000000000070008000000000000002'");
    require_error(service.create_job(request), ErrorCategory::Internal, "jobu.idempotency.invalid_record");

    execute(fixture.database,
            "CREATE TRIGGER fail_job_idempotency_insert BEFORE INSERT ON jobu_idempotency "
            "WHEN NEW.key = 'fail-key' BEGIN SELECT RAISE(ABORT, 'injected failure'); END");
    auto rollback_request            = request;
    rollback_request.idempotency_key = "fail-key";
    require_error(service.create_job(std::move(rollback_request)), ErrorCategory::Conflict, "db.constraint");
    CHECK(count_rows(fixture.database, "jobu_jobs") == 2);
    CHECK(count_rows(fixture.database, "jobu_runs") == 2);
    CHECK(count_rows(fixture.database, "jobu_idempotency") == 2);
}

TEST_CASE("Job create idempotency replay does not require fresh UUIDs", "[jobu][job][idempotency]")
{
    ServiceFixture fixture{
        {sequence_id(1), sequence_id(2), sequence_id(3)}
    };
    ManagementService service{fixture.database, fixture.registry, fixture.cron, fixture.generator, fixture.time};

    auto queue = service.create_queue({.name = "queue"});
    REQUIRE(queue);
    auto const request = CreateJobRequest{
        .queue           = queue->id,
        .name            = "replay",
        .type            = JobType::Cli,
        .schedule        = once_at(UtcTimePoint{20s}),
        .payload         = cli_payload("true"),
        .idempotency_key = "job-key",
    };

    auto original = service.create_job(request);
    REQUIRE(original);
    CHECK(original->id == sequence_id(2));

    auto replay = service.create_job(request);
    REQUIRE(replay);
    CHECK(replay->id == original->id);

    auto different     = request;
    different.priority = 1;
    require_error(service.create_job(std::move(different)), ErrorCategory::Conflict, "jobu.idempotency.conflict");
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
    ManagementService service{fixture.database, fixture.registry, fixture.cron, fixture.generator, fixture.time};
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
    ManagementService service{fixture.database, fixture.registry, fixture.cron, fixture.generator, fixture.time};
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
    ManagementService service{fixture.database, fixture.registry, fixture.cron, fixture.generator, fixture.time};
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
    ManagementService      service{fixture.database, registry, fixture.cron, fixture.generator, fixture.time};

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

TEST_CASE("Job update patches one-time definitions and their pending run snapshots", "[jobu][job][update][sqlite]")
{
    auto const     queue_id = uuid("00000000-0000-7000-8000-000000000070");
    auto const     job_id   = uuid("00000000-0000-7000-8000-000000000071");
    auto const     run_id   = uuid("00000000-0000-7000-8000-000000000072");
    ServiceFixture fixture{
        {queue_id, job_id, run_id}
    };
    ManagementService service{fixture.database,
                              fixture.registry,
                              fixture.cron,
                              fixture.generator,
                              fixture.time,
                              max_attempts(2)};
    REQUIRE(service.create_queue({.name = "jobs", .defaults = max_attempts(3)}));
    auto created = service.create_job({
        .queue      = queue_id,
        .name       = "before",
        .schedule   = once_at(UtcTimePoint{5s}),
        .priority   = 1,
        .attributes = max_attempts(4),
        .payload    = cli_payload("before"),
    });
    REQUIRE(created);
    REQUIRE(service.update_queue({.queue = queue_id, .defaults = max_attempts(8)}));
    fixture.time.advance(1s);

    auto const replacement_payload = http_payload("https://updated.test/path", "PATCH");
    auto       request             = UpdateJobRequest{
        .job_id            = job_id,
        .expected_revision = 1,
        .name              = std::optional<std::optional<std::string>>{std::in_place, std::nullopt},
        .type              = JobType::Http,
        .schedule          = once_at(UtcTimePoint{20s}
                       ),
        .priority          = 9,
        .attribute_changes = {{"job.timeout", {.data = 30s}}},
        .payload           = replacement_payload,
    };
    auto updated = service.update_job(request);
    REQUIRE(updated);
    CHECK(request.expected_revision == 1);
    CHECK(request.payload == replacement_payload);
    CHECK(updated->id == job_id);
    CHECK(updated->queue_id == queue_id);
    CHECK(updated->revision == 2);
    CHECK_FALSE(updated->name);
    CHECK(updated->state == JobState::Active);
    CHECK(updated->type == JobType::Http);
    CHECK(std::get<OnceSchedule>(updated->schedule).planned_at == UtcTimePoint{20s});
    CHECK(updated->priority == 9);
    CHECK(std::get<Duration>(updated->attributes.at("job.timeout").data) == 30s);
    CHECK(std::get<std::int64_t>(updated->attributes.at("retry.max_attempts").data) == 4);
    CHECK(updated->payload == replacement_payload);
    CHECK(updated->created_at == UtcTimePoint{10s});
    CHECK(updated->updated_at == UtcTimePoint{11s});

    auto persisted = service.get_job(job_id);
    REQUIRE(persisted);
    CHECK(persisted->revision == updated->revision);
    CHECK(attributes_json(persisted->attributes, fixture.registry) ==
          attributes_json(updated->attributes, fixture.registry));
    CHECK(persisted->payload == updated->payload);

    detail::RunRepository runs{fixture.database, fixture.registry};
    auto                  stored_run = runs.find_by_id(run_id);
    REQUIRE(stored_run);
    REQUIRE(stored_run->has_value());
    CHECK((**stored_run).job_revision == updated->revision);
    CHECK((**stored_run).queue_id == updated->queue_id);
    CHECK((**stored_run).planned_at == UtcTimePoint{20s});
    CHECK((**stored_run).runnable_at == UtcTimePoint{20s});
    CHECK((**stored_run).type == updated->type);
    CHECK((**stored_run).priority == updated->priority);
    CHECK(attributes_json((**stored_run).attributes, fixture.registry) ==
          attributes_json(updated->attributes, fixture.registry));
    CHECK((**stored_run).payload == updated->payload);
    auto const documents = stored_documents(fixture.database, job_id);
    CHECK(documents.job_attributes == documents.run_attributes);
    CHECK(documents.job_payload == documents.run_payload);

    require_error(service.update_job({.job_id = job_id, .expected_revision = 1, .priority = 10}),
                  ErrorCategory::Conflict,
                  "jobu.job.revision_conflict");

    execute(fixture.database, "UPDATE jobu_jobs SET state = 'suspended' WHERE id IS NOT NULL");
    fixture.time.advance(1s);
    auto suspended = service.update_job({.job_id = job_id, .expected_revision = 2, .priority = 10});
    REQUIRE(suspended);
    CHECK(suspended->state == JobState::Suspended);
    CHECK(suspended->revision == 3);
    CHECK(suspended->priority == 10);
    CHECK(suspended->updated_at == UtcTimePoint{12s});
}

TEST_CASE("Job lifecycle persists revisions draining suspension and idempotent no-ops", "[jobu][job][lifecycle]")
{
    auto const     queue_id      = uuid("00000000-0000-7000-8000-000000000100");
    auto const     immediate_job = uuid("00000000-0000-7000-8000-000000000101");
    auto const     immediate_run = uuid("00000000-0000-7000-8000-000000000102");
    auto const     busy_job      = uuid("00000000-0000-7000-8000-000000000103");
    auto const     busy_run      = uuid("00000000-0000-7000-8000-000000000104");
    ServiceFixture fixture{
        {queue_id, immediate_job, immediate_run, busy_job, busy_run}
    };
    ManagementService service{fixture.database, fixture.registry, fixture.cron, fixture.generator, fixture.time};
    REQUIRE(service.create_queue({.name = "jobs"}));
    REQUIRE(service.create_job({.queue    = queue_id,
                                .name     = "immediate",
                                .schedule = once_at(UtcTimePoint{20s}),
                                .payload  = cli_payload("immediate")}));
    REQUIRE(service.create_job(
        {.queue = queue_id, .name = "busy", .schedule = once_at(UtcTimePoint{30s}), .payload = cli_payload("busy")}));

    detail::RunRepository runs{fixture.database, fixture.registry};
    auto                  original_run = runs.find_schedule_owned(immediate_job);
    REQUIRE(original_run);
    REQUIRE(original_run->has_value());

    fixture.time.advance(1s);
    auto immediate = service.suspend_job(immediate_job);
    REQUIRE(immediate);
    CHECK(immediate->state == JobState::Suspended);
    CHECK(immediate->revision == 3);
    CHECK(immediate->updated_at == UtcTimePoint{11s});

    auto unchanged_run = runs.find_schedule_owned(immediate_job);
    REQUIRE(unchanged_run);
    REQUIRE(unchanged_run->has_value());
    CHECK((**unchanged_run).job_revision == (**original_run).job_revision);
    CHECK((**unchanged_run).state == RunState::Scheduled);
    CHECK((**unchanged_run).planned_at == (**original_run).planned_at);
    CHECK((**unchanged_run).runnable_at == (**original_run).runnable_at);
    CHECK((**unchanged_run).payload == (**original_run).payload);

    fixture.time.advance(1s);
    auto unchanged_suspended = service.suspend_job(immediate_job);
    REQUIRE(unchanged_suspended);
    CHECK(unchanged_suspended->revision == 3);
    CHECK(unchanged_suspended->updated_at == UtcTimePoint{11s});

    auto resumed = service.resume_job(immediate_job);
    REQUIRE(resumed);
    CHECK(resumed->state == JobState::Active);
    CHECK(resumed->revision == 4);
    CHECK(resumed->updated_at == UtcTimePoint{12s});

    fixture.time.advance(1s);
    auto unchanged_active = service.resume_job(immediate_job);
    REQUIRE(unchanged_active);
    CHECK(unchanged_active->revision == 4);
    CHECK(unchanged_active->updated_at == UtcTimePoint{12s});

    execute(fixture.database,
            "UPDATE jobu_runs SET state = 'running', started_at_us = 10000000 WHERE id = "
            "X'00000000000070008000000000000104'");
    detail::AttemptRepository attempts{fixture.database};
    REQUIRE(attempts.insert_attempt({
        .run_id         = busy_run,
        .attempt_number = 1,
        .due_at         = UtcTimePoint{10s},
        .started_at     = UtcTimePoint{10s},
        .state          = AttemptState::Running,
    }));

    fixture.time.advance(1s);
    auto draining = service.suspend_job(busy_job);
    REQUIRE(draining);
    CHECK(draining->state == JobState::Suspending);
    CHECK(draining->revision == 2);
    CHECK(draining->updated_at == UtcTimePoint{14s});

    fixture.time.advance(1s);
    auto unchanged_draining = service.suspend_job(busy_job);
    REQUIRE(unchanged_draining);
    CHECK(unchanged_draining->state == JobState::Suspending);
    CHECK(unchanged_draining->revision == 2);
    CHECK(unchanged_draining->updated_at == UtcTimePoint{14s});

    auto resumed_draining = service.resume_job(busy_job);
    REQUIRE(resumed_draining);
    CHECK(resumed_draining->state == JobState::Active);
    CHECK(resumed_draining->revision == 3);
    CHECK(resumed_draining->updated_at == UtcTimePoint{15s});

    fixture.time.advance(1s);
    draining = service.suspend_job(busy_job);
    REQUIRE(draining);
    CHECK(draining->state == JobState::Suspending);
    CHECK(draining->revision == 4);
    CHECK(draining->updated_at == UtcTimePoint{16s});

    auto running_snapshot = runs.find_schedule_owned(busy_job);
    REQUIRE(running_snapshot);
    REQUIRE(running_snapshot->has_value());
    CHECK((**running_snapshot).state == RunState::Running);
    CHECK((**running_snapshot).job_revision == 1);

    execute(fixture.database,
            "UPDATE jobu_attempts SET state = 'completed', completed_at_us = 17000000, outcome = 'succeeded', "
            "result_json = '{}' WHERE run_id = X'00000000000070008000000000000104'");
    execute(fixture.database,
            "UPDATE jobu_runs SET state = 'succeeded', completed_at_us = 17000000, result_json = '{}' WHERE id = "
            "X'00000000000070008000000000000104'");
    fixture.time.advance(1s);
    auto suspended = service.suspend_job(busy_job);
    REQUIRE(suspended);
    CHECK(suspended->state == JobState::Suspended);
    CHECK(suspended->revision == 5);
    CHECK(suspended->updated_at == UtcTimePoint{17s});

    auto terminal_snapshot = runs.find_by_id(busy_run);
    REQUIRE(terminal_snapshot);
    REQUIRE(terminal_snapshot->has_value());
    CHECK((**terminal_snapshot).state == RunState::Succeeded);
    CHECK((**terminal_snapshot).job_revision == 1);
}

TEST_CASE("Job lifecycle rolls back revision exhaustion and rejects deleted definitions", "[jobu][job][lifecycle]")
{
    auto const     queue_id = uuid("00000000-0000-7000-8000-000000000110");
    auto const     job_id   = uuid("00000000-0000-7000-8000-000000000111");
    auto const     run_id   = uuid("00000000-0000-7000-8000-000000000112");
    ServiceFixture fixture{
        {queue_id, job_id, run_id}
    };
    ManagementService service{fixture.database, fixture.registry, fixture.cron, fixture.generator, fixture.time};
    REQUIRE(service.create_queue({.name = "jobs"}));
    REQUIRE(
        service.create_job({.queue = queue_id, .schedule = once_at(UtcTimePoint{20s}), .payload = cli_payload("job")}));

    execute(fixture.database,
            "UPDATE jobu_jobs SET revision = 9223372036854775806 WHERE id = "
            "X'00000000000070008000000000000111'");
    require_error(service.suspend_job(job_id), ErrorCategory::ResourceExhausted, "jobu.job.revision_exhausted");
    auto rolled_back = service.get_job(job_id);
    REQUIRE(rolled_back);
    CHECK(rolled_back->state == JobState::Active);
    CHECK(rolled_back->revision == static_cast<JobRevision>(std::numeric_limits<std::int64_t>::max()) - 1);
    CHECK(rolled_back->updated_at == UtcTimePoint{10s});

    execute(fixture.database,
            "UPDATE jobu_jobs SET state = 'suspended', revision = 9223372036854775807 WHERE id = "
            "X'00000000000070008000000000000111'");
    require_error(service.resume_job(job_id), ErrorCategory::ResourceExhausted, "jobu.job.revision_exhausted");
    auto exhausted = service.get_job(job_id);
    REQUIRE(exhausted);
    CHECK(exhausted->state == JobState::Suspended);
    CHECK(exhausted->revision == static_cast<JobRevision>(std::numeric_limits<std::int64_t>::max()));

    execute(fixture.database,
            "UPDATE jobu_jobs SET state = 'deleted', updated_at_us = 12000000, deleted_at_us = 12000000 WHERE id = "
            "X'00000000000070008000000000000111'");
    require_error(service.suspend_job(job_id), ErrorCategory::Conflict, "jobu.job.deleted");
    require_error(service.resume_job(job_id), ErrorCategory::Conflict, "jobu.job.deleted");
    require_error(service.suspend_job(uuid("00000000-0000-7000-8000-000000000199")),
                  ErrorCategory::NotFound,
                  "jobu.job.not_found");
}

TEST_CASE("Job move preserves definitions and terminal history across guarded targets", "[jobu][job][move]")
{
    auto const     source_queue     = uuid("00000000-0000-7000-8000-000000000200");
    auto const     active_target    = uuid("00000000-0000-7000-8000-000000000201");
    auto const     suspended_target = uuid("00000000-0000-7000-8000-000000000202");
    auto const     deleted_target   = uuid("00000000-0000-7000-8000-000000000203");
    auto const     moving_job       = uuid("00000000-0000-7000-8000-000000000204");
    auto const     moving_run       = uuid("00000000-0000-7000-8000-000000000205");
    auto const     history_job      = uuid("00000000-0000-7000-8000-000000000206");
    auto const     history_run      = uuid("00000000-0000-7000-8000-000000000207");
    ServiceFixture fixture{
        {source_queue,
         active_target, suspended_target,
         deleted_target, moving_job,
         moving_run, history_job,
         history_run}
    };
    ManagementService service{fixture.database, fixture.registry, fixture.cron, fixture.generator, fixture.time};
    REQUIRE(service.create_queue({.name = "source", .defaults = max_attempts(2)}));
    REQUIRE(service.create_queue({.name = "active-target", .defaults = max_attempts(99)}));
    REQUIRE(service.create_queue({.name = "suspended-target", .defaults = max_attempts(88)}));
    REQUIRE(service.create_queue({.name = "deleted-target"}));
    REQUIRE(service.suspend_queue(suspended_target));
    execute(fixture.database,
            "UPDATE jobu_queues SET name = "
            "'deleted-target-deleted#00000000-0000-7000-8000-000000000203', deleted_name = 'deleted-target', "
            "state = 'deleted', updated_at_us = 10000000, deleted_at_us = 10000000 WHERE id = "
            "X'00000000000070008000000000000203'");

    auto created = service.create_job({.queue      = source_queue,
                                       .name       = "movable",
                                       .type       = JobType::Cli,
                                       .schedule   = once_at(UtcTimePoint{30s}),
                                       .priority   = 7,
                                       .attributes = max_attempts(7),
                                       .payload    = cli_payload("move", {"one"})});
    REQUIRE(created);
    auto historical = service.create_job({.queue      = source_queue,
                                          .name       = "history",
                                          .type       = JobType::Http,
                                          .schedule   = once_at(UtcTimePoint{40s}),
                                          .priority   = -4,
                                          .attributes = max_attempts(6),
                                          .payload    = http_payload("https://history.test", "POST")});
    REQUIRE(historical);
    auto const original_documents = stored_documents(fixture.database, moving_job);
    execute(fixture.database,
            "UPDATE jobu_runs SET state = 'succeeded', started_at_us = 9000000, completed_at_us = 10000000, "
            "result_json = '{}' WHERE id = X'00000000000070008000000000000207'");

    require_error(service.move_job({.job_id = moving_job, .target_queue = active_target}),
                  ErrorCategory::InvalidArgument,
                  "jobu.job.invalid_configuration");
    require_error(service.move_job({.job_id = moving_job, .expected_revision = 1, .target_queue = active_target}),
                  ErrorCategory::Conflict,
                  "jobu.job.not_suspended");
    auto suspended_moving = service.suspend_job(moving_job);
    REQUIRE(suspended_moving);
    auto suspended_history = service.suspend_job(history_job);
    REQUIRE(suspended_history);
    CHECK(suspended_moving->revision == 3);
    CHECK(suspended_history->revision == 3);

    require_error(service.move_job({.job_id = moving_job, .expected_revision = 2, .target_queue = active_target}),
                  ErrorCategory::Conflict,
                  "jobu.job.revision_conflict");
    require_error(service.move_job({.job_id = moving_job, .expected_revision = 3, .target_queue = source_queue}),
                  ErrorCategory::Conflict,
                  "jobu.queue.state_conflict");
    execute(fixture.database,
            "UPDATE jobu_queues SET state = 'suspending' WHERE id = X'00000000000070008000000000000201'");
    require_error(service.move_job({.job_id = moving_job, .expected_revision = 3, .target_queue = active_target}),
                  ErrorCategory::Conflict,
                  "jobu.queue.state_conflict");
    execute(fixture.database, "UPDATE jobu_queues SET state = 'active' WHERE id = X'00000000000070008000000000000201'");
    require_error(service.move_job({.job_id = moving_job, .expected_revision = 3, .target_queue = deleted_target}),
                  ErrorCategory::NotFound,
                  "jobu.queue.not_found");

    fixture.time.advance(1s);
    auto moved = service.move_job({.job_id = moving_job, .expected_revision = 3, .target_queue = active_target});
    REQUIRE(moved);
    CHECK(moved->queue_id == active_target);
    CHECK(moved->revision == 4);
    CHECK(moved->state == JobState::Suspended);
    CHECK(moved->name == created->name);
    CHECK(moved->type == created->type);
    CHECK(moved->priority == created->priority);
    CHECK(std::get<std::int64_t>(moved->attributes.at("retry.max_attempts").data) == 7);
    CHECK(moved->payload == created->payload);
    CHECK(moved->created_at == created->created_at);
    CHECK(moved->updated_at == UtcTimePoint{11s});

    detail::RunRepository runs{fixture.database, fixture.registry};
    auto                  current = runs.find_by_id(moving_run);
    REQUIRE(current);
    REQUIRE(current->has_value());
    CHECK((**current).queue_id == active_target);
    CHECK((**current).job_revision == 4);
    auto moved_documents = stored_documents(fixture.database, moving_job);
    CHECK(moved_documents.job_attributes == original_documents.job_attributes);
    CHECK(moved_documents.run_attributes == original_documents.run_attributes);
    CHECK(moved_documents.job_payload == original_documents.job_payload);
    CHECK(moved_documents.run_payload == original_documents.run_payload);

    fixture.time.advance(1s);
    moved = service.move_job({.job_id = moving_job, .expected_revision = 4, .target_queue = suspended_target});
    REQUIRE(moved);
    CHECK(moved->queue_id == suspended_target);
    CHECK(moved->revision == 5);
    current = runs.find_by_id(moving_run);
    REQUIRE(current);
    REQUIRE(current->has_value());
    CHECK((**current).queue_id == suspended_target);
    CHECK((**current).job_revision == 5);

    auto moved_history =
        service.move_job({.job_id = history_job, .expected_revision = 3, .target_queue = active_target});
    REQUIRE(moved_history);
    CHECK(moved_history->queue_id == active_target);
    CHECK(moved_history->revision == 4);
    auto terminal = runs.find_by_id(history_run);
    REQUIRE(terminal);
    REQUIRE(terminal->has_value());
    CHECK((**terminal).state == RunState::Succeeded);
    CHECK((**terminal).queue_id == source_queue);
    CHECK((**terminal).job_revision == 1);

    execute(fixture.database,
            "UPDATE jobu_jobs SET revision = 9223372036854775807 WHERE id = "
            "X'00000000000070008000000000000204'");
    require_error(
        service.move_job({.job_id            = moving_job,
                          .expected_revision = static_cast<JobRevision>(std::numeric_limits<std::int64_t>::max()),
                          .target_queue      = source_queue}),
        ErrorCategory::ResourceExhausted,
        "jobu.job.revision_exhausted");
    current = runs.find_by_id(moving_run);
    REQUIRE(current);
    REQUIRE(current->has_value());
    CHECK((**current).queue_id == suspended_target);
    CHECK((**current).job_revision == 5);
}

TEST_CASE("Job deletion cancels pending work cleans references and enforces prerequisites", "[jobu][job][delete]")
{
    auto const     queue_id      = uuid("00000000-0000-7000-8000-000000000210");
    auto const     deleted_job   = uuid("00000000-0000-7000-8000-000000000211");
    auto const     deleted_run   = uuid("00000000-0000-7000-8000-000000000212");
    auto const     running_job   = uuid("00000000-0000-7000-8000-000000000213");
    auto const     running_run   = uuid("00000000-0000-7000-8000-000000000214");
    auto const     exhausted_job = uuid("00000000-0000-7000-8000-000000000215");
    auto const     exhausted_run = uuid("00000000-0000-7000-8000-000000000216");
    ServiceFixture fixture{
        {queue_id, deleted_job, deleted_run, running_job, running_run, exhausted_job, exhausted_run}
    };
    ManagementService service{fixture.database, fixture.registry, fixture.cron, fixture.generator, fixture.time};
    REQUIRE(service.create_queue({.name = "jobs"}));
    REQUIRE(service.create_job({.queue    = queue_id,
                                .name     = "delete",
                                .schedule = once_at(UtcTimePoint{20s}),
                                .payload  = cli_payload("delete")}));
    REQUIRE(service.create_job({.queue    = queue_id,
                                .name     = "running",
                                .schedule = once_at(UtcTimePoint{30s}),
                                .payload  = cli_payload("running")}));
    REQUIRE(service.create_job({.queue    = queue_id,
                                .name     = "exhausted",
                                .schedule = once_at(UtcTimePoint{40s}),
                                .payload  = cli_payload("exhausted")}));

    require_error(service.delete_job({.job_id = deleted_job}),
                  ErrorCategory::InvalidArgument,
                  "jobu.job.invalid_configuration");
    require_error(service.delete_job({.job_id = uuid("00000000-0000-7000-8000-000000000299"), .expected_revision = 1}),
                  ErrorCategory::NotFound,
                  "jobu.job.not_found");
    require_error(service.delete_job({.job_id = deleted_job, .expected_revision = 1}),
                  ErrorCategory::Conflict,
                  "jobu.job.not_suspended");
    auto suspended = service.suspend_job(deleted_job);
    REQUIRE(suspended);
    REQUIRE(suspended->revision == 3);
    require_error(service.delete_job({.job_id = deleted_job, .expected_revision = 2}),
                  ErrorCategory::Conflict,
                  "jobu.job.revision_conflict");
    execute(fixture.database,
            "INSERT INTO jobu_secrets(name, value_blob, created_at_us, updated_at_us) "
            "VALUES ('delete.secret', X'0102', 1, 1)");
    execute(fixture.database,
            "INSERT INTO jobu_secret_refs(secret_name, job_id, field_path) VALUES "
            "('delete.secret', X'00000000000070008000000000000211', 'payload.token')");

    fixture.time.advance(1s);
    REQUIRE(service.delete_job({.job_id = deleted_job, .expected_revision = 3}));
    require_error(service.delete_job({.job_id = deleted_job, .expected_revision = 4}),
                  ErrorCategory::Conflict,
                  "jobu.job.deleted");
    auto stored_deleted = service.get_job(deleted_job, true);
    REQUIRE(stored_deleted);
    CHECK(stored_deleted->state == JobState::Deleted);
    CHECK(stored_deleted->revision == 4);
    CHECK(stored_deleted->updated_at == UtcTimePoint{11s});
    CHECK(stored_deleted->deleted_at == UtcTimePoint{11s});

    detail::RunRepository runs{fixture.database, fixture.registry};
    auto                  cancelled = runs.find_by_id(deleted_run);
    REQUIRE(cancelled);
    REQUIRE(cancelled->has_value());
    CHECK((**cancelled).state == RunState::Cancelled);
    CHECK((**cancelled).completed_at == UtcTimePoint{11s});
    REQUIRE((**cancelled).result);
    CHECK((**cancelled).result->as_object().at("reason").as_string() == "job_deleted");
    CHECK(count_rows(fixture.database, "jobu_secret_refs") == 0);
    CHECK(count_rows(fixture.database, "jobu_secrets") == 1);

    execute(fixture.database,
            "UPDATE jobu_runs SET state = 'running', started_at_us = 10000000 WHERE id = "
            "X'00000000000070008000000000000214'");
    detail::AttemptRepository attempts{fixture.database};
    REQUIRE(attempts.insert_attempt({
        .run_id         = running_run,
        .attempt_number = 1,
        .due_at         = UtcTimePoint{10s},
        .started_at     = UtcTimePoint{10s},
        .state          = AttemptState::Running,
    }));
    execute(fixture.database,
            "UPDATE jobu_jobs SET state = 'suspended', revision = 3 WHERE id = "
            "X'00000000000070008000000000000213'");
    require_error(service.delete_job({.job_id = running_job, .expected_revision = 3}),
                  ErrorCategory::Conflict,
                  "jobu.job.has_running_attempt");
    auto preserved_running = service.get_job(running_job);
    REQUIRE(preserved_running);
    CHECK(preserved_running->state == JobState::Suspended);
    CHECK(preserved_running->revision == 3);

    auto exhausted = service.suspend_job(exhausted_job);
    REQUIRE(exhausted);
    execute(fixture.database,
            "UPDATE jobu_jobs SET revision = 9223372036854775807 WHERE id = "
            "X'00000000000070008000000000000215'");
    require_error(service.delete_job({
                      .job_id            = exhausted_job,
                      .expected_revision = static_cast<JobRevision>(std::numeric_limits<std::int64_t>::max()),
                  }),
                  ErrorCategory::ResourceExhausted,
                  "jobu.job.revision_exhausted");
    auto preserved_exhausted = service.get_job(exhausted_job);
    REQUIRE(preserved_exhausted);
    CHECK(preserved_exhausted->state == JobState::Suspended);
    auto pending = runs.find_by_id(exhausted_run);
    REQUIRE(pending);
    REQUIRE(pending->has_value());
    CHECK((**pending).state == RunState::Scheduled);
}

TEST_CASE("Job update validates revisions state and replacement fields", "[jobu][job][update][validation]")
{
    auto const     queue_id = uuid("00000000-0000-7000-8000-000000000080");
    auto const     job_id   = uuid("00000000-0000-7000-8000-000000000081");
    ServiceFixture fixture{
        {queue_id, job_id, uuid("00000000-0000-7000-8000-000000000082")}
    };
    ManagementService service{fixture.database, fixture.registry, fixture.cron, fixture.generator, fixture.time};
    REQUIRE(service.create_queue({.name = "jobs"}));
    REQUIRE(
        service.create_job({.queue = queue_id, .schedule = once_at(UtcTimePoint{1s}), .payload = cli_payload("true")}));

    require_error(service.update_job({.job_id = job_id, .priority = 1}),
                  ErrorCategory::InvalidArgument,
                  "jobu.job.invalid_configuration");
    require_error(service.update_job({.job_id = job_id, .expected_revision = 1}),
                  ErrorCategory::InvalidArgument,
                  "jobu.job.invalid_configuration");
    require_error(service.update_job({
                      .job_id            = job_id,
                      .expected_revision = 1,
                      .name = std::optional<std::optional<std::string>>{std::in_place, std::string{"bad\x01name", 8}}
    }),
                  ErrorCategory::InvalidArgument,
                  "jobu.job.invalid_name");
    require_error(service.update_job(
                      {.job_id = job_id, .expected_revision = 1, .attribute_changes = {{"unknown", {.data = true}}}}),
                  ErrorCategory::InvalidArgument,
                  "jobu.attribute.unknown");
    require_error(service.update_job({.job_id = job_id, .expected_revision = 1, .type = JobType::Http}),
                  ErrorCategory::InvalidArgument,
                  "jobu.job.invalid_payload");
    require_error(service.update_job(
                      {.job_id = uuid("00000000-0000-7000-8000-000000000099"), .expected_revision = 1, .priority = 1}),
                  ErrorCategory::NotFound,
                  "jobu.job.not_found");

    execute(fixture.database, "UPDATE jobu_jobs SET state = 'suspending' WHERE id IS NOT NULL");
    require_error(service.update_job({.job_id = job_id, .expected_revision = 1, .priority = 1}),
                  ErrorCategory::Conflict,
                  "jobu.job.state_conflict");
    execute(fixture.database, "UPDATE jobu_jobs SET state = 'deleted', deleted_at_us = 11 WHERE id IS NOT NULL");
    require_error(service.update_job({.job_id = job_id, .expected_revision = 1, .priority = 1}),
                  ErrorCategory::Conflict,
                  "jobu.job.deleted");
    execute(fixture.database,
            "UPDATE jobu_jobs SET state = 'active', deleted_at_us = NULL, revision = 9223372036854775807 "
            "WHERE id IS NOT NULL");
    require_error(
        service.update_job({.job_id            = job_id,
                            .expected_revision = static_cast<JobRevision>(std::numeric_limits<std::int64_t>::max()),
                            .priority          = 1}),
        ErrorCategory::ResourceExhausted,
        "jobu.job.revision_exhausted");
}

TEST_CASE("Job update rolls back when attempts prevent snapshot refresh", "[jobu][job][update][transaction]")
{
    auto const     queue_id    = uuid("00000000-0000-7000-8000-000000000090");
    auto const     pending_job = uuid("00000000-0000-7000-8000-000000000091");
    auto const     pending_run = uuid("00000000-0000-7000-8000-000000000092");
    auto const     started_job = uuid("00000000-0000-7000-8000-000000000093");
    auto const     started_run = uuid("00000000-0000-7000-8000-000000000094");
    ServiceFixture fixture{
        {queue_id, pending_job, pending_run, started_job, started_run}
    };
    ManagementService service{fixture.database, fixture.registry, fixture.cron, fixture.generator, fixture.time};
    REQUIRE(service.create_queue({.name = "jobs"}));
    REQUIRE(service.create_job({.queue    = queue_id,
                                .name     = "pending",
                                .schedule = once_at(UtcTimePoint{1s}),
                                .payload  = cli_payload("pending")}));
    REQUIRE(service.create_job({.queue    = queue_id,
                                .name     = "started",
                                .schedule = once_at(UtcTimePoint{2s}),
                                .payload  = cli_payload("started")}));

    detail::AttemptRepository attempts{fixture.database};
    REQUIRE(attempts.insert_attempt({
        .run_id         = pending_run,
        .attempt_number = 1,
        .due_at         = UtcTimePoint{1s},
    }));
    REQUIRE(attempts.insert_attempt({
        .run_id         = started_run,
        .attempt_number = 1,
        .due_at         = UtcTimePoint{2s},
        .started_at     = UtcTimePoint{3s},
        .state          = AttemptState::Running,
    }));

    require_error(service.update_job({.job_id = pending_job, .expected_revision = 1, .priority = 7}),
                  ErrorCategory::Conflict,
                  "jobu.run.schedule_conflict");
    auto pending = service.get_job(pending_job);
    REQUIRE(pending);
    CHECK(pending->revision == 1);
    CHECK(pending->priority == 0);
    CHECK(pending->updated_at == UtcTimePoint{10s});

    require_error(service.update_job({.job_id = started_job, .expected_revision = 1, .priority = 8}),
                  ErrorCategory::Conflict,
                  "jobu.job.immutable");
    auto started = service.get_job(started_job);
    REQUIRE(started);
    CHECK(started->revision == 1);
    CHECK(started->priority == 0);
}

TEST_CASE("Job management rejects malformed persisted definitions", "[jobu][job][sqlite]")
{
    auto const     queue_id = uuid("00000000-0000-7000-8000-000000000040");
    auto const     job_id   = uuid("00000000-0000-7000-8000-000000000041");
    ServiceFixture fixture{
        {queue_id, job_id, uuid("00000000-0000-7000-8000-000000000042")}
    };
    ManagementService service{fixture.database, fixture.registry, fixture.cron, fixture.generator, fixture.time};
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
    ManagementService queue_service{fixture.database, fixture.registry, fixture.cron, fixture.generator, fixture.time};
    auto              queue = queue_service.create_queue({.name = "jobs"});
    REQUIRE(queue);

    ManagementService invalid_service{fixture.database,
                                      fixture.registry,
                                      fixture.cron,
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
    require_error(invalid_service.suspend_job(uuid("00000000-0000-7000-8000-000000000099")),
                  ErrorCategory::InvalidArgument,
                  "jobu.attribute.unknown");
    require_error(invalid_service.resume_job(uuid("00000000-0000-7000-8000-000000000099")),
                  ErrorCategory::InvalidArgument,
                  "jobu.attribute.unknown");
    require_error(invalid_service.move_job({.job_id            = uuid("00000000-0000-7000-8000-000000000099"),
                                            .expected_revision = 1,
                                            .target_queue      = queue->id}),
                  ErrorCategory::InvalidArgument,
                  "jobu.attribute.unknown");
    require_error(
        invalid_service.delete_job({.job_id = uuid("00000000-0000-7000-8000-000000000099"), .expected_revision = 1}),
        ErrorCategory::InvalidArgument,
        "jobu.attribute.unknown");
}
