#include "management_rpc.hpp"

#include "application.hpp"
#include "attribute_registry.hpp"
#include "database.hpp"
#include "framing.hpp"
#include "management.hpp"
#include "management_json.hpp"
#include "protocol_priv.hpp"
#include "server.hpp"
#include "sqlite/sqlite_driver.hpp"
#include "sqlite/sqlite_schema.hpp"
#include "support/fake_time_source.hpp"
#include "support/memory_io_device.hpp"
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
#include <variant>
#include <vector>

using namespace jb::core;
using namespace jb::db;
using namespace jb::jobu;
using namespace jb::rpc;
using namespace jb::rpc::detail;
using namespace jb::test;
using namespace std::chrono_literals;

namespace {

template <typename T>
auto make_json(T value) -> JsonValue
{
    JsonValue result;
    result.data = std::move(value);
    return result;
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
        time.set_utc(UtcTimePoint{1s});
    }

    TemporaryDirectory        directory;
    std::filesystem::path     database_file{directory.path() / "jobu.sqlite"};
    Database                  database{make_database(database_file)};
    StandardAttributeRegistry registry;
    SequenceUuidGenerator     generator;
    FakeTimeSource            time;
};

auto encode_frame(JsonValue const& value) -> std::string
{
    auto serialized = serialize_json(value);
    REQUIRE(serialized);
    auto framed = frame_message(serialized.value());
    REQUIRE(framed);
    return std::move(framed).value();
}

auto request_frame(std::uint64_t id, std::string_view method, std::optional<JsonValue> const& params = std::nullopt)
    -> std::string
{
    return encode_frame(encode_request(id, method, params));
}

auto take_response(MemoryIODevice& device) -> ResponseEnvelope
{
    StreamFramer framer;
    auto         bodies = framer.append(device.take_written_data());
    REQUIRE(bodies);
    REQUIRE(bodies->size() == 1U);

    auto parsed = parse_json(bodies->front());
    REQUIRE(parsed);
    auto decoded = decode_response_document(parsed.value());
    REQUIRE(decoded);
    REQUIRE(decoded->entries.size() == 1U);
    return std::move(decoded->entries.front());
}

auto require_result(ResponseEnvelope const& response) -> JsonValue const&
{
    REQUIRE(std::holds_alternative<JsonValue>(response.payload));
    return std::get<JsonValue>(response.payload);
}

auto require_error(ResponseEnvelope const& response) -> RpcError const&
{
    REQUIRE(std::holds_alternative<RpcError>(response.payload));
    return std::get<RpcError>(response.payload);
}

void require_standard_error(ResponseEnvelope const& response, ErrorCode code)
{
    auto const& error = require_error(response);
    CHECK(error.code == static_cast<std::int64_t>(code));
    CHECK_FALSE(error.data);
}

void require_application_error(ResponseEnvelope const& response, std::string_view category, std::string_view code)
{
    auto const& error = require_error(response);
    CHECK(error.code == static_cast<std::int64_t>(ErrorCode::ApplicationError));
    REQUIRE(error.data);
    REQUIRE(error.data->is_object());
    auto const& data = error.data->as_object();
    REQUIRE(data.size() == 2U);
    CHECK(data.at("category").as_string() == category);
    CHECK(data.at("code").as_string() == code);
    CHECK_FALSE(data.contains("detail"));
}

class RpcEndpoint {
public:
    explicit RpcEndpoint(ServiceFixture& fixture)
        : _service{fixture.database, fixture.registry, fixture.generator, fixture.time}
    {
        REQUIRE(register_management_methods(_server, _service, fixture.registry));

        auto device = std::make_unique<MemoryIODevice>();
        _device     = device.get();
        device->open();
        REQUIRE(_server.add_connection(std::move(device)));
    }

    [[nodiscard]] auto call(std::string_view method, std::optional<JsonValue> params = std::nullopt) -> ResponseEnvelope
    {
        _device->inject_input(request_frame(_next_id++, method, params));
        return take_response(*_device);
    }

private:
    ManagementService _service;
    Server            _server;
    MemoryIODevice*   _device{nullptr};
    std::uint64_t     _next_id{1};
};

auto max_attempts(std::int64_t value) -> AttributeSet
{
    return {
        {"retry.max_attempts", {.data = value}}
    };
}

auto once_at(UtcTimePoint planned_at) -> JobSchedule
{
    return OnceSchedule{.planned_at = planned_at};
}

auto cli_payload(std::string command) -> JsonValue
{
    return make_json(JsonValue::Object{
        {"command", make_json(std::move(command))}
    });
}

auto encode_create(CreateQueueRequest const& request, AttributeRegistry const& registry) -> JsonValue
{
    auto encoded = create_queue_request_to_json(request, registry);
    REQUIRE(encoded);
    return std::move(encoded).value();
}

auto encode_create(CreateJobRequest const& request, AttributeRegistry const& registry) -> JsonValue
{
    auto encoded = create_job_request_to_json(request, registry);
    REQUIRE(encoded);
    return std::move(encoded).value();
}

auto encode_selector(QueueSelector const& selector) -> JsonValue
{
    auto encoded = queue_selector_to_json(selector);
    REQUIRE(encoded);
    return std::move(encoded).value();
}

auto encode_job_id(Uuid const& id) -> JsonValue
{
    auto encoded = job_id_to_json(id);
    REQUIRE(encoded);
    return std::move(encoded).value();
}

auto decode_queue(ResponseEnvelope const& response, AttributeRegistry const& registry) -> Queue
{
    auto decoded = queue_from_json(require_result(response), registry);
    REQUIRE(decoded);
    return std::move(decoded).value();
}

auto decode_job(ResponseEnvelope const& response, AttributeRegistry const& registry) -> JobDefinition
{
    auto decoded = job_from_json(require_result(response), registry);
    REQUIRE(decoded);
    return std::move(decoded).value();
}

} // anonymous namespace

TEST_CASE("Management RPC registration is exact and reports duplicate failure", "[jobu][management-rpc]")
{
    Application       app{0, nullptr};
    ServiceFixture    fixture{{sequence_id(1)}};
    ManagementService service{fixture.database, fixture.registry, fixture.generator, fixture.time};
    Server            server;

    constexpr std::array<std::string_view, 15> expected_methods{
        "queue.create",
        "queue.get",
        "queue.list",
        "queue.update",
        "queue.suspend",
        "queue.resume",
        "queue.delete",
        "job.create",
        "job.get",
        "job.list",
        "job.update",
        "job.suspend",
        "job.resume",
        "job.move",
        "job.delete",
    };

    auto const methods = management_rpc_method_names();
    REQUIRE(methods.size() == expected_methods.size());
    REQUIRE(register_management_methods(server, service, fixture.registry));
    for (auto index = std::size_t{0}; index < expected_methods.size(); ++index) {
        CHECK(methods[index] == expected_methods[index]);
        CHECK(server.has_method(expected_methods[index]));
    }
    CHECK_FALSE(server.has_method("system.info"));
    CHECK_FALSE(server.has_method("job.run_now"));
    CHECK_FALSE(server.has_method("run.get"));
    CHECK_FALSE(server.has_method("attempt.list"));
    CHECK_FALSE(server.has_method("secret.list"));
    CHECK_FALSE(server.has_method("schedule.cron"));
    CHECK_FALSE(server.has_method("stats.get"));

    Server duplicate;
    REQUIRE(duplicate.register_method("job.update", [](auto const&, auto const&) {
        return MethodResult::success(make_json(JsonNull{}));
    }));
    CHECK_FALSE(register_management_methods(duplicate, service, fixture.registry));
    CHECK(duplicate.has_method("queue.create"));
    CHECK(duplicate.has_method("queue.get"));
    CHECK(duplicate.has_method("queue.list"));
    CHECK(duplicate.has_method("queue.update"));
    CHECK(duplicate.has_method("queue.suspend"));
    CHECK(duplicate.has_method("queue.resume"));
    CHECK(duplicate.has_method("queue.delete"));
    CHECK(duplicate.has_method("job.create"));
    CHECK(duplicate.has_method("job.get"));
    CHECK(duplicate.has_method("job.list"));
    CHECK(duplicate.has_method("job.update"));
    CHECK_FALSE(duplicate.has_method("job.suspend"));
}

TEST_CASE("Queue management RPC completes and persists the durable lifecycle", "[jobu][management-rpc][sqlite]")
{
    Application    app{0, nullptr};
    auto const     first_id  = sequence_id(1);
    auto const     second_id = sequence_id(2);
    ServiceFixture fixture{
        {first_id, second_id}
    };

    auto const create_request = CreateQueueRequest{
        .name                  = "alpha",
        .weight                = 2,
        .concurrency_limit     = 3,
        .recovery_policy       = RecoveryPolicy::RetryInterrupted,
        .defaults              = max_attempts(4),
        .history_retention     = 0s,
        .runnable_wait_warning = 25ms,
        .idempotency_key       = "create-alpha",
    };
    auto const create_params = encode_create(create_request, fixture.registry);

    {
        RpcEndpoint endpoint{fixture};

        auto created = decode_queue(endpoint.call("queue.create", create_params), fixture.registry);
        CHECK(created.id == first_id);
        CHECK(created.name == "alpha");
        CHECK(created.weight == 2U);
        CHECK(created.concurrency_limit == 3U);
        CHECK(created.recovery_policy == RecoveryPolicy::RetryInterrupted);
        CHECK(created.history_retention == 0s);
        CHECK(created.runnable_wait_warning == 25ms);
        CHECK(std::get<std::int64_t>(created.defaults.at("retry.max_attempts").data) == 4);

        auto replayed = decode_queue(endpoint.call("queue.create", create_params), fixture.registry);
        CHECK(replayed.id == first_id);
        CHECK(replayed.name == created.name);
        CHECK(replayed.weight == created.weight);
        CHECK(replayed.created_at == created.created_at);
        CHECK(replayed.updated_at == created.updated_at);

        auto by_name =
            decode_queue(endpoint.call("queue.get", encode_selector(std::string{"alpha"})), fixture.registry);
        CHECK(by_name.id == first_id);

        auto list_params = queue_list_request_to_json({.page = {.limit = 1}});
        REQUIRE(list_params);
        auto listed = endpoint.call("queue.list", std::move(list_params).value());
        auto page   = queue_page_from_json(require_result(listed), fixture.registry);
        REQUIRE(page);
        REQUIRE(page->items.size() == 1U);
        CHECK(page->items.front().id == first_id);
        CHECK_FALSE(page->next_after_id);

        auto update_params = update_queue_request_to_json(
            {
                .queue    = first_id,
                .weight   = 5,
                .defaults = max_attempts(6),
            },
            fixture.registry);
        REQUIRE(update_params);
        auto updated = decode_queue(endpoint.call("queue.update", std::move(update_params).value()), fixture.registry);
        CHECK(updated.weight == 5U);
        CHECK(std::get<std::int64_t>(updated.defaults.at("retry.max_attempts").data) == 6);

        auto selector  = encode_selector(first_id);
        auto suspended = decode_queue(endpoint.call("queue.suspend", selector), fixture.registry);
        CHECK(suspended.state == QueueState::Suspended);
        auto resumed = decode_queue(endpoint.call("queue.resume", selector), fixture.registry);
        CHECK(resumed.state == QueueState::Active);
        suspended = decode_queue(endpoint.call("queue.suspend", selector), fixture.registry);
        CHECK(suspended.state == QueueState::Suspended);

        CHECK(require_result(endpoint.call("queue.delete", selector)).is_null());

        auto replacement_request = CreateQueueRequest{.name = "alpha"};
        auto replacement_create  = encode_create(replacement_request, fixture.registry);
        auto replacement         = decode_queue(endpoint.call("queue.create", replacement_create), fixture.registry);
        CHECK(replacement.id == second_id);
        CHECK(replacement.name == "alpha");
        CHECK(replacement.state == QueueState::Active);
    }

    {
        RpcEndpoint endpoint{fixture};
        auto        list_params = queue_list_request_to_json({
            .include_deleted = true,
            .page            = {.limit = 10},
        });
        REQUIRE(list_params);
        auto listed = endpoint.call("queue.list", std::move(list_params).value());
        auto page   = queue_page_from_json(require_result(listed), fixture.registry);
        REQUIRE(page);
        REQUIRE(page->items.size() == 2U);
        CHECK(page->items[0].id == first_id);
        CHECK(page->items[0].name == "alpha");
        CHECK(page->items[0].state == QueueState::Deleted);
        CHECK(page->items[1].id == second_id);
        CHECK(page->items[1].name == "alpha");
        CHECK(page->items[1].state == QueueState::Active);
    }
}

TEST_CASE("Job management RPC completes revisions idempotency and a durable lifecycle",
          "[jobu][management-rpc][sqlite]")
{
    Application    app{0, nullptr};
    auto const     source_queue_id = sequence_id(1);
    auto const     target_queue_id = sequence_id(2);
    auto const     job_id          = sequence_id(3);
    ServiceFixture fixture{
        {source_queue_id, target_queue_id, job_id, sequence_id(4)}
    };

    auto const create_request = CreateJobRequest{
        .queue           = source_queue_id,
        .name            = "rpc-job",
        .type            = JobType::Cli,
        .schedule        = once_at(UtcTimePoint{20s}),
        .priority        = 1,
        .attributes      = max_attempts(4),
        .payload         = cli_payload("/usr/bin/true"),
        .idempotency_key = "create-rpc-job",
    };
    auto const create_params = encode_create(create_request, fixture.registry);

    {
        RpcEndpoint endpoint{fixture};

        auto source = decode_queue(
            endpoint.call("queue.create", encode_create(CreateQueueRequest{.name = "source"}, fixture.registry)),
            fixture.registry);
        CHECK(source.id == source_queue_id);
        auto target = decode_queue(
            endpoint.call("queue.create", encode_create(CreateQueueRequest{.name = "target"}, fixture.registry)),
            fixture.registry);
        CHECK(target.id == target_queue_id);

        auto created = decode_job(endpoint.call("job.create", create_params), fixture.registry);
        CHECK(created.id == job_id);
        CHECK(created.queue_id == source_queue_id);
        CHECK(created.revision == 1);
        CHECK(created.name == "rpc-job");
        CHECK(created.state == JobState::Active);
        CHECK(created.priority == 1);
        CHECK(std::get<std::int64_t>(created.attributes.at("retry.max_attempts").data) == 4);
        CHECK(created.payload == create_request.payload);

        auto fetched = decode_job(endpoint.call("job.get", encode_job_id(job_id)), fixture.registry);
        CHECK(fetched.id == created.id);
        CHECK(fetched.revision == created.revision);

        auto list_params = job_list_request_to_json({
            .queue = source_queue_id,
            .page  = {.limit = 1},
        });
        REQUIRE(list_params);
        auto listed = endpoint.call("job.list", std::move(list_params).value());
        auto page   = job_page_from_json(require_result(listed), fixture.registry);
        REQUIRE(page);
        REQUIRE(page->items.size() == 1U);
        CHECK(page->items.front().id == job_id);
        CHECK_FALSE(page->next_after_id);

        auto update_params = update_job_request_to_json(
            {
                .job_id            = job_id,
                .expected_revision = 1,
                .priority          = 7,
                .attribute_changes = max_attempts(6),
            },
            fixture.registry);
        REQUIRE(update_params);
        auto updated = decode_job(endpoint.call("job.update", update_params.value()), fixture.registry);
        CHECK(updated.revision == 2);
        CHECK(updated.priority == 7);
        CHECK(std::get<std::int64_t>(updated.attributes.at("retry.max_attempts").data) == 6);

        require_application_error(endpoint.call("job.update", update_params.value()),
                                  "conflict",
                                  "jobu.job.revision_conflict");

        auto suspended = decode_job(endpoint.call("job.suspend", encode_job_id(job_id)), fixture.registry);
        CHECK(suspended.state == JobState::Suspended);
        CHECK(suspended.revision == 4);
        auto resumed = decode_job(endpoint.call("job.resume", encode_job_id(job_id)), fixture.registry);
        CHECK(resumed.state == JobState::Active);
        CHECK(resumed.revision == 5);
        suspended = decode_job(endpoint.call("job.suspend", encode_job_id(job_id)), fixture.registry);
        CHECK(suspended.state == JobState::Suspended);
        CHECK(suspended.revision == 7);

        auto move_params = move_job_request_to_json({
            .job_id            = job_id,
            .expected_revision = 7,
            .target_queue      = target_queue_id,
        });
        REQUIRE(move_params);
        auto moved = decode_job(endpoint.call("job.move", std::move(move_params).value()), fixture.registry);
        CHECK(moved.queue_id == target_queue_id);
        CHECK(moved.revision == 8);
        CHECK(moved.state == JobState::Suspended);
    }

    REQUIRE(fixture.database.close());
    REQUIRE(fixture.database.open());
    auto schema = jb::jobu::sqlite::ensure_schema(fixture.database);
    REQUIRE(schema);
    CHECK_FALSE(schema->created);

    {
        RpcEndpoint endpoint{fixture};

        auto replayed = decode_job(endpoint.call("job.create", create_params), fixture.registry);
        CHECK(replayed.id == job_id);
        CHECK(replayed.queue_id == source_queue_id);
        CHECK(replayed.revision == 1);
        CHECK(replayed.priority == 1);

        auto persisted = decode_job(endpoint.call("job.get", encode_job_id(job_id)), fixture.registry);
        CHECK(persisted.queue_id == target_queue_id);
        CHECK(persisted.revision == 8);
        CHECK(persisted.state == JobState::Suspended);

        auto resumed = decode_job(endpoint.call("job.resume", encode_job_id(job_id)), fixture.registry);
        CHECK(resumed.state == JobState::Active);
        CHECK(resumed.revision == 9);
        auto suspended = decode_job(endpoint.call("job.suspend", encode_job_id(job_id)), fixture.registry);
        CHECK(suspended.state == JobState::Suspended);
        CHECK(suspended.revision == 11);

        auto delete_params = delete_job_request_to_json({
            .job_id            = job_id,
            .expected_revision = 11,
        });
        REQUIRE(delete_params);
        CHECK(require_result(endpoint.call("job.delete", std::move(delete_params).value())).is_null());

        auto list_params = job_list_request_to_json({
            .queue           = target_queue_id,
            .include_deleted = true,
            .page            = {.limit = 10},
        });
        REQUIRE(list_params);
        auto listed = endpoint.call("job.list", std::move(list_params).value());
        auto page   = job_page_from_json(require_result(listed), fixture.registry);
        REQUIRE(page);
        REQUIRE(page->items.size() == 1U);
        CHECK(page->items.front().id == job_id);
        CHECK(page->items.front().queue_id == target_queue_id);
        CHECK(page->items.front().state == JobState::Deleted);
        CHECK(page->items.front().revision == 12);
    }
}

TEST_CASE("Management RPC separates invalid params from safe application errors", "[jobu][management-rpc][errors]")
{
    Application    app{0, nullptr};
    auto const     queue_id = sequence_id(1);
    ServiceFixture fixture{{queue_id}};
    RpcEndpoint    endpoint{fixture};

    for (auto const method : management_rpc_method_names()) {
        CAPTURE(method);
        require_standard_error(endpoint.call(method), ErrorCode::InvalidParams);
    }

    require_standard_error(endpoint.call("queue.create",
                                         make_json(JsonValue::Object{
                                             {"name",       make_json(std::string{"alpha"})},
                                             {"unexpected", make_json(true)                },
    })),
                           ErrorCode::InvalidParams);

    require_application_error(
        endpoint.call("queue.create", encode_create(CreateQueueRequest{.name = ""}, fixture.registry)),
        "invalid_argument",
        "jobu.queue.invalid_name");

    auto const missing_id = sequence_id(9);
    require_application_error(endpoint.call("queue.get", encode_selector(missing_id)),
                              "not_found",
                              "jobu.queue.not_found");

    auto created = endpoint.call("queue.create", encode_create(CreateQueueRequest{.name = "active"}, fixture.registry));
    REQUIRE(require_result(created).is_object());
    require_application_error(endpoint.call("queue.delete", encode_selector(queue_id)),
                              "conflict",
                              "jobu.queue.not_suspended");

    require_standard_error(endpoint.call("queue.update",
                                         make_json(JsonValue::Object{
                                             {"queue_id", make_json(queue_id.to_string())},
    })),
                           ErrorCode::InvalidParams);

    require_standard_error(endpoint.call("job.create",
                                         make_json(JsonValue::Object{
                                             {"queue_id",   make_json(queue_id.to_string())},
                                             {"unexpected", make_json(true)                },
    })),
                           ErrorCode::InvalidParams);

    require_application_error(
        endpoint.call("job.create",
                      encode_create(
                          CreateJobRequest{
                              .queue    = queue_id,
                              .schedule = CronSchedule{.expression = "0 * * * *", .timezone = "UTC"},
                              .payload  = cli_payload("/usr/bin/true"),
    },
                          fixture.registry)),
        "unsupported",
        "jobu.schedule.cron_unavailable");

    require_application_error(endpoint.call("job.get", encode_job_id(missing_id)), "not_found", "jobu.job.not_found");

    require_standard_error(endpoint.call("job.update",
                                         make_json(JsonValue::Object{
                                             {"job_id", make_json(missing_id.to_string())},
    })),
                           ErrorCode::InvalidParams);
}
