#include "management.hpp"

#include "attempt_repository_priv.hpp"
#include "attribute_registry.hpp"
#include "database.hpp"
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
        time.set_utc(UtcTimePoint{1s});
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

auto json_string(std::string value) -> jb::rpc::JsonValue
{
    auto json = jb::rpc::JsonValue{};
    json.data = std::move(value);
    return json;
}

auto cli_payload(std::string command) -> jb::rpc::JsonValue
{
    auto payload = jb::rpc::JsonValue{};
    payload.data = jb::rpc::JsonValue::Object{
        {"command", json_string(std::move(command))}
    };
    return payload;
}

auto max_attempts(std::int64_t value) -> AttributeSet
{
    return {
        {"retry.max_attempts", {.data = value}}
    };
}

auto defaults_json(Database& database, std::string_view queue_name) -> std::string
{
    Query query{database};
    REQUIRE(query.prepare("SELECT defaults_json FROM jobu_queues WHERE name = :name"));
    REQUIRE(query.bind_value(":name", make_text(queue_name)));
    REQUIRE(query.exec());
    REQUIRE(query.next());
    auto const* value = query.value("defaults_json");
    REQUIRE(value != nullptr);
    auto const* text = std::get_if<std::string>(value);
    REQUIRE(text != nullptr);
    return *text;
}

class MapAttributeRegistry final : public AttributeRegistry {
public:
    [[nodiscard]] auto find(std::string_view name) const noexcept -> AttributeDefinition const* override
    {
        return name == _definitions.front().name ? &_definitions.front() : nullptr;
    }

    [[nodiscard]] auto validate(std::string_view name, AttributeValue const& value, AttributeScope scope) const
        -> Result<void, Error> override
    {
        if (find(name) == nullptr) {
            return Result<void, Error>::failure({
                .category = ErrorCategory::InvalidArgument,
                .code     = "jobu.attribute.unknown",
                .message  = "Unknown test attribute",
            });
        }
        if (!_definitions.front().scopes.test(scope) || !std::holds_alternative<AttributeValue::Map>(value.data)) {
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
    std::array<AttributeDefinition, 1> _definitions{{{
        .name             = "test.map",
        .type             = AttributeType::Map,
        .scopes           = {AttributeScope::DaemonDefault, AttributeScope::QueueDefault, AttributeScope::Job},
        .built_in_default = {.data = AttributeValue::Map{}},
        .description      = "Map used for queue serialization tests",
    }}};
};

} // anonymous namespace

TEST_CASE("Queue management creates gets lists and updates durable queues", "[jobu][queue][sqlite]")
{
    auto const     first_id  = uuid("00000000-0000-7000-8000-000000000001");
    auto const     second_id = uuid("00000000-0000-7000-8000-000000000002");
    auto const     third_id  = uuid("00000000-0000-7000-8000-000000000003");
    auto const     spare_id  = uuid("00000000-0000-7000-8000-000000000004");
    ServiceFixture fixture{
        {first_id, second_id, third_id, spare_id}
    };
    ManagementService service{fixture.database, fixture.registry, fixture.generator, fixture.time};

    auto create_request = CreateQueueRequest{
        .name                  = "alpha",
        .weight                = 2,
        .concurrency_limit     = 3,
        .recovery_policy       = RecoveryPolicy::RetryInterrupted,
        .defaults              = max_attempts(4),
        .history_retention     = 0s,
        .runnable_wait_warning = 25ms,
        .idempotency_key       = "create-alpha",
    };
    auto first = service.create_queue(create_request);
    REQUIRE(first);
    CHECK(create_request.name == "alpha");
    CHECK(std::get<std::int64_t>(create_request.defaults.at("retry.max_attempts").data) == 4);
    CHECK(first->id == first_id);
    CHECK(first->state == QueueState::Active);
    CHECK(first->created_at == UtcTimePoint{1s});
    CHECK(first->updated_at == first->created_at);
    CHECK_FALSE(first->deleted_at);
    CHECK(std::get<std::int64_t>(first->defaults.at("retry.max_attempts").data) == 4);
    CHECK(defaults_json(fixture.database, "alpha") ==
          R"({"values":{"retry.max_attempts":{"type":"integer","value":4}},"version":1})");

    auto by_id = service.get_queue(first_id);
    REQUIRE(by_id);
    CHECK(by_id->name == "alpha");
    CHECK(by_id->weight == 2);
    CHECK(by_id->history_retention == 0s);
    CHECK(std::get<std::int64_t>(by_id->defaults.at("retry.max_attempts").data) == 4);
    auto by_name = service.get_queue(std::string{"alpha"});
    REQUIRE(by_name);
    CHECK(by_name->id == first_id);

    auto const noncanonical_defaults =
        R"({ "values": { "retry.max_attempts": { "type": "integer", "value": 4 } }, "version": 1 })";
    execute(fixture.database,
            "UPDATE jobu_queues SET defaults_json = '" + std::string{noncanonical_defaults} + "' WHERE name = 'alpha'");
    REQUIRE(service.update_queue({.queue = first_id, .weight = 5}));
    CHECK(defaults_json(fixture.database, "alpha") == noncanonical_defaults);

    fixture.time.advance(1s);
    REQUIRE(service.create_queue({.name = "beta"}));
    fixture.time.advance(1s);
    REQUIRE(service.create_queue({.name = "gamma"}));

    auto page = service.list_queues({.page = {.limit = 2}});
    REQUIRE(page);
    REQUIRE(page->items.size() == 2);
    CHECK(page->items[0].id == first_id);
    CHECK(page->items[1].id == second_id);
    REQUIRE(page->next_after_id);
    CHECK(*page->next_after_id == second_id);

    auto tail = service.list_queues({
        .page = {.limit = 2, .after_id = page->next_after_id}
    });
    REQUIRE(tail);
    REQUIRE(tail->items.size() == 1);
    CHECK(tail->items.front().id == third_id);
    CHECK_FALSE(tail->next_after_id);

    auto active = service.list_queues({.state = QueueState::Active, .page = {.limit = 3}});
    REQUIRE(active);
    CHECK(active->items.size() == 3);

    fixture.time.advance(1s);
    auto update_request = UpdateQueueRequest{
        .queue                 = first_id,
        .name                  = "renamed-alpha",
        .weight                = 7,
        .concurrency_limit     = 8,
        .recovery_policy       = RecoveryPolicy::FailInterrupted,
        .defaults              = AttributeSet{},
        .history_retention     = std::optional<std::chrono::seconds>{},
        .runnable_wait_warning = 50ms,
    };
    auto updated = service.update_queue(update_request);
    REQUIRE(updated);
    CHECK(*update_request.name == "renamed-alpha");
    CHECK(update_request.defaults->empty());
    CHECK(updated->id == first_id);
    CHECK(updated->name == "renamed-alpha");
    CHECK(updated->weight == 7);
    CHECK(updated->concurrency_limit == 8);
    CHECK(updated->defaults.empty());
    CHECK_FALSE(updated->history_retention);
    CHECK(updated->runnable_wait_warning == 50ms);
    CHECK(updated->created_at == UtcTimePoint{1s});
    CHECK(updated->updated_at == UtcTimePoint{4s});
    CHECK(defaults_json(fixture.database, "renamed-alpha") == R"({"values":{},"version":1})");

    require_error(service.get_queue(std::string{"alpha"}), ErrorCategory::NotFound, "jobu.queue.not_found");
    auto persisted = service.get_queue(std::string{"renamed-alpha"});
    REQUIRE(persisted);
    CHECK(persisted->id == first_id);

    require_error(service.update_queue({.queue = first_id, .name = "beta"}),
                  ErrorCategory::Conflict,
                  "jobu.queue.name_conflict");
    persisted = service.get_queue(first_id);
    REQUIRE(persisted);
    CHECK(persisted->name == "renamed-alpha");

    require_error(service.create_queue({.name = "beta"}), ErrorCategory::Conflict, "jobu.queue.name_conflict");
}

TEST_CASE("Queue lifecycle persists draining suspension resume and idempotent no-ops", "[jobu][queue][lifecycle]")
{
    auto const     immediate_queue = uuid("00000000-0000-7000-8000-000000000050");
    auto const     busy_queue      = uuid("00000000-0000-7000-8000-000000000051");
    auto const     job_id          = uuid("00000000-0000-7000-8000-000000000052");
    auto const     run_id          = uuid("00000000-0000-7000-8000-000000000053");
    ServiceFixture fixture{
        {immediate_queue, busy_queue, job_id, run_id}
    };
    ManagementService service{fixture.database, fixture.registry, fixture.generator, fixture.time};
    REQUIRE(service.create_queue({.name = "immediate"}));
    REQUIRE(service.create_queue({.name = "busy"}));
    REQUIRE(service.create_job({.queue    = busy_queue,
                                .schedule = OnceSchedule{.planned_at = UtcTimePoint{1s}},
                                .payload  = cli_payload("busy")}));

    fixture.time.advance(1s);
    auto immediate = service.suspend_queue(std::string{"immediate"});
    REQUIRE(immediate);
    CHECK(immediate->state == QueueState::Suspended);
    CHECK(immediate->updated_at == UtcTimePoint{2s});

    fixture.time.advance(1s);
    auto unchanged_suspended = service.suspend_queue(immediate_queue);
    REQUIRE(unchanged_suspended);
    CHECK(unchanged_suspended->state == QueueState::Suspended);
    CHECK(unchanged_suspended->updated_at == UtcTimePoint{2s});

    auto resumed = service.resume_queue(immediate_queue);
    REQUIRE(resumed);
    CHECK(resumed->state == QueueState::Active);
    CHECK(resumed->updated_at == UtcTimePoint{3s});

    fixture.time.advance(1s);
    auto unchanged_active = service.resume_queue(immediate_queue);
    REQUIRE(unchanged_active);
    CHECK(unchanged_active->state == QueueState::Active);
    CHECK(unchanged_active->updated_at == UtcTimePoint{3s});

    execute(fixture.database,
            "UPDATE jobu_runs SET state = 'running', started_at_us = 1000000 WHERE id = "
            "X'00000000000070008000000000000053'");
    detail::AttemptRepository attempts{fixture.database};
    REQUIRE(attempts.insert_attempt({
        .run_id         = run_id,
        .attempt_number = 1,
        .due_at         = UtcTimePoint{1s},
        .started_at     = UtcTimePoint{1s},
        .state          = AttemptState::Running,
    }));

    fixture.time.advance(1s);
    auto draining = service.suspend_queue(busy_queue);
    REQUIRE(draining);
    CHECK(draining->state == QueueState::Suspending);
    CHECK(draining->updated_at == UtcTimePoint{5s});

    fixture.time.advance(1s);
    auto unchanged_draining = service.suspend_queue(busy_queue);
    REQUIRE(unchanged_draining);
    CHECK(unchanged_draining->state == QueueState::Suspending);
    CHECK(unchanged_draining->updated_at == UtcTimePoint{5s});

    auto resumed_draining = service.resume_queue(busy_queue);
    REQUIRE(resumed_draining);
    CHECK(resumed_draining->state == QueueState::Active);
    CHECK(resumed_draining->updated_at == UtcTimePoint{6s});

    fixture.time.advance(1s);
    draining = service.suspend_queue(busy_queue);
    REQUIRE(draining);
    CHECK(draining->state == QueueState::Suspending);
    CHECK(draining->updated_at == UtcTimePoint{7s});

    auto job = service.get_job(job_id);
    REQUIRE(job);
    CHECK(job->state == JobState::Active);
    CHECK(job->revision == 1);
    CHECK(job->updated_at == UtcTimePoint{1s});
    detail::RunRepository runs{fixture.database, fixture.registry};
    auto                  stored_run = runs.find_schedule_owned(job_id);
    REQUIRE(stored_run);
    REQUIRE(stored_run->has_value());
    CHECK((**stored_run).state == RunState::Running);
    CHECK((**stored_run).job_revision == 1);

    execute(fixture.database,
            "UPDATE jobu_attempts SET state = 'completed', completed_at_us = 8000000, outcome = 'succeeded', "
            "result_json = '{}' WHERE run_id = X'00000000000070008000000000000053'");
    execute(fixture.database,
            "UPDATE jobu_runs SET state = 'succeeded', completed_at_us = 8000000, result_json = '{}' WHERE id = "
            "X'00000000000070008000000000000053'");
    fixture.time.advance(1s);
    auto suspended = service.suspend_queue(busy_queue);
    REQUIRE(suspended);
    CHECK(suspended->state == QueueState::Suspended);
    CHECK(suspended->updated_at == UtcTimePoint{8s});

    job = service.get_job(job_id);
    REQUIRE(job);
    CHECK(job->state == JobState::Active);
    CHECK(job->revision == 1);

    execute(fixture.database,
            "UPDATE jobu_queues SET name = "
            "'immediate-deleted#00000000-0000-7000-8000-000000000050', deleted_name = 'immediate', "
            "state = 'deleted', updated_at_us = 9000000, deleted_at_us = 9000000 WHERE id = "
            "X'00000000000070008000000000000050'");
    require_error(service.suspend_queue(immediate_queue), ErrorCategory::Conflict, "jobu.queue.state_conflict");
    require_error(service.resume_queue(immediate_queue), ErrorCategory::Conflict, "jobu.queue.state_conflict");
    require_error(service.suspend_queue(uuid("00000000-0000-7000-8000-000000000099")),
                  ErrorCategory::NotFound,
                  "jobu.queue.not_found");
}

TEST_CASE("Queue management rejects invalid input before durable mutation", "[jobu][queue][validation]")
{
    auto const        generated_id = uuid("00000000-0000-7000-8000-000000000010");
    ServiceFixture    fixture{{generated_id}};
    ManagementService service{fixture.database, fixture.registry, fixture.generator, fixture.time};

    require_error(service.create_queue({.name = ""}), ErrorCategory::InvalidArgument, "jobu.queue.invalid_name");
    require_error(service.create_queue({
                      .name = std::string{"bad\x01name", 8}
    }),
                  ErrorCategory::InvalidArgument,
                  "jobu.queue.invalid_name");
    require_error(service.create_queue({.name = "bad\xC3"}), ErrorCategory::InvalidArgument, "jobu.queue.invalid_name");
    require_error(service.create_queue({.name = "name-deleted#00000000-0000-7000-8000-000000000099"}),
                  ErrorCategory::InvalidArgument,
                  "jobu.queue.invalid_name");
    require_error(service.create_queue({.name = "zero-weight", .weight = 0}),
                  ErrorCategory::InvalidArgument,
                  "jobu.queue.invalid_configuration");
    require_error(service.create_queue({.name = "negative-retention", .history_retention = -1s}),
                  ErrorCategory::InvalidArgument,
                  "jobu.queue.invalid_configuration");
    require_error(service.create_queue({.name = "bad-key", .idempotency_key = ""}),
                  ErrorCategory::InvalidArgument,
                  "jobu.idempotency.invalid_key");
    require_error(service.create_queue({.name = "unknown-default", .defaults = {{"unknown", {.data = true}}}}),
                  ErrorCategory::InvalidArgument,
                  "jobu.attribute.unknown");

    MapAttributeRegistry map_registry;
    ManagementService    map_service{fixture.database, map_registry, fixture.generator, fixture.time};
    auto                 oversized_defaults = AttributeSet{
        {"test.map", {.data = AttributeValue::Map{{"value", {.data = std::string(256U * 1024U, 'x')}}}}}
    };
    auto oversized_error = require_error(
        map_service.create_queue({.name = "oversized-default", .defaults = std::move(oversized_defaults)}),
        ErrorCategory::ResourceExhausted,
        "jobu.protocol.value_too_large");
    CHECK(oversized_error.message == "Queue attribute document exceeds its size limit");

    auto invalid_utf8_defaults = AttributeSet{
        {"test.map", {.data = AttributeValue::Map{{"value", {.data = std::string{"\xc3", 1U}}}}}}
    };
    require_error(
        map_service.create_queue({.name = "invalid-utf8-default", .defaults = std::move(invalid_utf8_defaults)}),
        ErrorCategory::InvalidArgument,
        "jobu.attribute.invalid_value");

    auto created = service.create_queue({.name = "valid"});
    REQUIRE(created);
    CHECK(created->id == generated_id);

    require_error(service.update_queue({.queue = generated_id}),
                  ErrorCategory::InvalidArgument,
                  "jobu.queue.invalid_configuration");
    require_error(service.update_queue({.queue = generated_id, .concurrency_limit = 0}),
                  ErrorCategory::InvalidArgument,
                  "jobu.queue.invalid_configuration");
    require_error(service.list_queues({.page = {.limit = 0}}),
                  ErrorCategory::InvalidArgument,
                  "jobu.queue.invalid_configuration");
    require_error(service.list_queues({.page = {.limit = 201}}),
                  ErrorCategory::InvalidArgument,
                  "jobu.queue.invalid_configuration");
    require_error(service.get_queue(uuid("00000000-0000-7000-8000-000000000099")),
                  ErrorCategory::NotFound,
                  "jobu.queue.not_found");

    auto listed = service.list_queues({.page = {.limit = 1}});
    REQUIRE(listed);
    REQUIRE(listed->items.size() == 1);
    CHECK(listed->items.front().name == "valid");
}

TEST_CASE("Queue management reports invalid daemon defaults from every operation", "[jobu][queue][validation]")
{
    ServiceFixture    fixture{{uuid("00000000-0000-7000-8000-000000000020")}};
    ManagementService service{fixture.database,
                              fixture.registry,
                              fixture.generator,
                              fixture.time,
                              {{"unknown", {.data = true}}}};

    require_error(service.create_queue({.name = "queue"}), ErrorCategory::InvalidArgument, "jobu.attribute.unknown");
    require_error(service.get_queue(std::string{"queue"}), ErrorCategory::InvalidArgument, "jobu.attribute.unknown");
    require_error(service.list_queues({}), ErrorCategory::InvalidArgument, "jobu.attribute.unknown");
    require_error(service.update_queue({.queue = std::string{"queue"}, .weight = 2}),
                  ErrorCategory::InvalidArgument,
                  "jobu.attribute.unknown");
    require_error(service.suspend_queue(std::string{"queue"}),
                  ErrorCategory::InvalidArgument,
                  "jobu.attribute.unknown");
    require_error(service.resume_queue(std::string{"queue"}), ErrorCategory::InvalidArgument, "jobu.attribute.unknown");
}

TEST_CASE("Queue lookup exposes original deleted names and detects historical ambiguity", "[jobu][queue][sqlite]")
{
    ServiceFixture    fixture{{uuid("00000000-0000-7000-8000-000000000030")}};
    ManagementService service{fixture.database, fixture.registry, fixture.generator, fixture.time};

    execute(fixture.database, R"sql(
INSERT INTO jobu_queues(
    id, name, deleted_name, state, weight, concurrency_limit, recovery_policy, defaults_json,
    retention_seconds, runnable_wait_warning_ms, created_at_us, updated_at_us, deleted_at_us
) VALUES
    (X'00000000000070008000000000000031', 'old-deleted#00000000-0000-7000-8000-000000000031', 'old',
     'deleted', 1, 1, 'fail_interrupted', '{"version":1,"values":{}}', NULL, 10000, 1, 2, 2),
    (X'00000000000070008000000000000032', 'old-deleted#00000000-0000-7000-8000-000000000032', 'old',
     'deleted', 1, 1, 'fail_interrupted', '{"version":1,"values":{}}', NULL, 10000, 1, 3, 3)
)sql");

    require_error(service.get_queue(std::string{"old"}), ErrorCategory::NotFound, "jobu.queue.not_found");
    require_error(service.get_queue(std::string{"old"}, true),
                  ErrorCategory::Conflict,
                  "jobu.queue.ambiguous_deleted_name");

    auto deleted = service.get_queue(uuid("00000000-0000-7000-8000-000000000031"), true);
    REQUIRE(deleted);
    CHECK(deleted->name == "old");
    CHECK(deleted->state == QueueState::Deleted);
    REQUIRE(deleted->deleted_at);

    auto hidden = service.list_queues({.page = {.limit = 10}});
    REQUIRE(hidden);
    CHECK(hidden->items.empty());
    auto visible = service.list_queues({.include_deleted = true, .state = QueueState::Deleted, .page = {.limit = 10}});
    REQUIRE(visible);
    REQUIRE(visible->items.size() == 2);
    CHECK(visible->items[0].name == "old");
    CHECK(visible->items[1].name == "old");

    auto active = service.create_queue({.name = "old"});
    REQUIRE(active);
    auto preferred = service.get_queue(std::string{"old"}, true);
    REQUIRE(preferred);
    CHECK(preferred->id == active->id);
    CHECK(preferred->state == QueueState::Active);
}

TEST_CASE("Queue management rejects malformed persisted attribute documents", "[jobu][queue][sqlite]")
{
    auto const        id = uuid("00000000-0000-7000-8000-000000000040");
    ServiceFixture    fixture{{id}};
    ManagementService service{fixture.database, fixture.registry, fixture.generator, fixture.time};
    REQUIRE(service.create_queue({.name = "queue"}));
    execute(fixture.database, "UPDATE jobu_queues SET defaults_json = '{}' WHERE name = 'queue'");

    require_error(service.get_queue(id), ErrorCategory::Internal, "jobu.attribute.invalid_document");
    require_error(service.list_queues({}), ErrorCategory::Internal, "jobu.attribute.invalid_document");

    execute(fixture.database, "UPDATE jobu_queues SET weight = 4294967296 WHERE name = 'queue'");
    require_error(service.get_queue(id), ErrorCategory::Internal, "jobu.storage.invalid_integer");

    execute(fixture.database, "UPDATE jobu_queues SET weight = 1, name = char(1) WHERE id IS NOT NULL");
    require_error(service.get_queue(id), ErrorCategory::Internal, "jobu.storage.invalid_queue");
}
