#include "scheduler.hpp"

#include "application.hpp"
#include "attempt_repository_priv.hpp"
#include "attribute_registry.hpp"
#include "byte_buffer.hpp"
#include "database.hpp"
#include "domain_storage_priv.hpp"
#include "event_loop_types.hpp"
#include "framing.hpp"
#include "http/http_attempt_executor.hpp"
#include "http/system_http_client.hpp"
#include "job_validation_priv.hpp"
#include "json.hpp"
#include "management.hpp"
#include "management_json.hpp"
#include "management_rpc.hpp"
#include "protocol_priv.hpp"
#include "query.hpp"
#include "run_repository_priv.hpp"
#include "secret_provider_priv.hpp"
#include "secret_service.hpp"
#include "server.hpp"
#include "sqlite/sqlite_driver.hpp"
#include "sqlite/sqlite_schema.hpp"
#include "support/fake_cron_engine.hpp"
#include "support/fake_time_source.hpp"
#include "support/http_test_server.hpp"
#include "support/memory_io_device.hpp"
#include "support/rejecting_secret_provider.hpp"
#include "support/secret_execution_checks.hpp"
#include "support/sequence_uuid_generator.hpp"
#include "support/storage_fault_helpers.hpp"
#include "support/temporary_directory.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
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
using namespace jb::jobu::detail;
using jb::jobu::http::HttpAttemptExecutor;
using jb::net::http::SystemHttpClient;
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

auto test_ids() -> std::vector<Uuid>
{
    auto values = std::vector<Uuid>{};
    values.reserve(200);
    for (auto suffix = std::uint16_t{1}; suffix <= 200; ++suffix) {
        values.push_back(id(static_cast<std::uint8_t>(suffix)));
    }
    return values;
}

auto at_seconds(std::int64_t seconds) -> UtcTimePoint
{
    return UtcTimePoint{std::chrono::seconds{seconds}};
}

auto make_database(std::filesystem::path database_file) -> Database
{
    return Database{std::make_unique<jb::db::sqlite::Driver>(jb::db::sqlite::Options{
        .database_file = std::move(database_file),
        .busy_timeout  = 1000ms,
        .durability    = jb::db::sqlite::Durability::Normal,
    })};
}

auto bytes(std::string_view value) -> ByteBuffer
{
    auto const view = as_bytes(value);
    return {view.begin(), view.end()};
}

auto byte_text(ByteBuffer const& value) -> std::string_view
{
    return as_string_view(ByteView{value});
}

auto json_string(std::string value) -> JsonValue
{
    return JsonValue{.data = std::move(value)};
}

auto json_object(JsonValue::Object value) -> JsonValue
{
    return JsonValue{.data = std::move(value)};
}

auto http_payload(std::string url) -> JsonValue
{
    return json_object({
        {"url", json_string(std::move(url))}
    });
}

auto cli_payload(std::string command) -> JsonValue
{
    return json_object({
        {"command", json_string(std::move(command))}
    });
}

auto result_string(JsonValue const& result, std::string_view name) -> std::string_view
{
    auto const& object = result.as_object();
    auto const  found  = object.find(name);
    REQUIRE(found != object.end());
    return found->second.as_string();
}

auto result_uint(JsonValue const& result, std::string_view name) -> std::uint64_t
{
    auto const& object = result.as_object();
    auto const  found  = object.find(name);
    REQUIRE(found != object.end());
    return found->second.as_uint();
}

struct CreatedJob {
    JobDefinition definition;
    JobRun        run;
};

struct RealSchedulerFixture {
    explicit RealSchedulerFixture(SchedulerOptions options = {}, bool resolve_secrets = false)
        : app{0, nullptr}
        , database_file{directory.path() / "jobu.sqlite"}
        , database{make_database(database_file)}
        , database_secrets{database}
        , generator{test_ids()}
        , runs{database, registry}
        , attempts{database}
    {
        REQUIRE(database.open());
        REQUIRE(jb::jobu::sqlite::ensure_schema(database));
        time.set_utc(at_seconds(100));

        auto created_client = SystemHttpClient::create(*app.event_loop());
        REQUIRE(created_client);
        client     = std::move(created_client).value();
        executor   = std::make_unique<HttpAttemptExecutor>(*client, time);
        management = std::make_unique<ManagementService>(database, registry, cron, generator, time);
        scheduler =
            std::make_unique<Scheduler>(database,
                                        registry,
                                        cron,
                                        generator,
                                        time,
                                        *executor,
                                        resolve_secrets ? static_cast<SecretProvider&>(database_secrets) : secrets,
                                        options);
    }

    ~RealSchedulerFixture()
    {
        // Stop admission before draining so a completion cannot dispatch more work during teardown.
        if (scheduler) {
            scheduler->stop();
        }

        // Release both server barriers so assertion unwinding cannot strand a paused transfer, then keep the owner loop
        // alive while accepted work drains.
        server.release_responses();
        server.release_response_segment();
        if (client) {
            auto const deadline = std::chrono::steady_clock::now() + 5s;
            while (client->active_request_count() != 0U && std::chrono::steady_clock::now() < deadline) {
                if (app.process_events(EventFlag::All, 25) == ProcessEventsResult::Failed) {
                    break;
                }
            }
            for (auto index = 0; index < 4; ++index) {
                if (app.process_events(EventFlag::All, 0) == ProcessEventsResult::Failed) {
                    break;
                }
            }
        }
        scheduler.reset();
        management.reset();
        executor.reset();
        client.reset();
    }

    auto create_queue(std::string name, std::uint32_t weight, std::uint32_t concurrency) const -> Queue
    {
        auto created = management->create_queue({
            .name              = std::move(name),
            .weight            = weight,
            .concurrency_limit = concurrency,
        });
        REQUIRE(created);
        return std::move(created).value();
    }

    auto create_http_job(Queue const& queue, std::string path, AttributeSet attributes = {}, std::int32_t priority = 0)
        -> CreatedJob
    {
        return create_job(queue,
                          JobType::Http,
                          http_payload(server.url(std::move(path))),
                          std::move(attributes),
                          priority);
    }

    auto create_cli_job(Queue const& queue, std::string command, std::int32_t priority = 0) -> CreatedJob
    {
        return create_job(queue, JobType::Cli, cli_payload(std::move(command)), {}, priority);
    }

    auto find_run(Uuid const& run_id) -> JobRun
    {
        auto found = runs.find_by_id(run_id);
        REQUIRE(found);
        REQUIRE(found->has_value());
        return std::move(found->value());
    }

    auto find_attempt(AttemptKey const& key) -> JobAttempt
    {
        auto found = attempts.find(key.run_id, key.attempt_number);
        REQUIRE(found);
        REQUIRE(found->has_value());
        return std::move(found->value());
    }

    auto find_output(AttemptKey const& key) -> std::optional<jb::jobu::detail::AttemptOutput>
    {
        auto found = attempts.find_output(key.run_id, key.attempt_number);
        REQUIRE(found);
        return std::move(found).value();
    }

    auto has_run_state(Uuid const& run_id, RunState state) -> bool
    {
        auto found = runs.find_by_id(run_id);
        return found && found->has_value() && found->value().state == state;
    }

    template <typename Predicate>
    auto process_until(Predicate&& predicate, std::chrono::milliseconds timeout = 5s) -> bool
    {
        auto const deadline = std::chrono::steady_clock::now() + timeout;
        while (!predicate() && std::chrono::steady_clock::now() < deadline) {
            if (app.process_events(EventFlag::All, 25) == ProcessEventsResult::Failed) {
                return false;
            }
        }
        return predicate();
    }

    void stop_after_idle()
    {
        // Consume queued completion/rescan callbacks before proving a clean running scheduler and stopping its timer.
        REQUIRE(process_until([this]() -> bool { return client->active_request_count() == 0U; }));
        for (auto index = 0; index < 4; ++index) {
            REQUIRE(app.process_events(EventFlag::All, 0) != ProcessEventsResult::Failed);
        }
        CHECK(scheduler->state() == SchedulerState::Running);
        CHECK_FALSE(scheduler->failure());
        scheduler->stop();
    }

    auto create_job(Queue const&               queue,
                    JobType                    type,
                    JsonValue                  payload,
                    AttributeSet               attributes,
                    std::int32_t               priority        = 0,
                    std::optional<std::string> idempotency_key = std::nullopt) -> CreatedJob
    {
        auto definition = management->create_job({
            .queue           = queue.id,
            .type            = type,
            .schedule        = OnceSchedule{.planned_at = at_seconds(90)},
            .priority        = priority,
            .attributes      = std::move(attributes),
            .payload         = std::move(payload),
            .idempotency_key = std::move(idempotency_key),
        });
        REQUIRE(definition);
        auto run = runs.find_schedule_owned(definition->id);
        REQUIRE(run);
        REQUIRE(run->has_value());
        return {
            .definition = std::move(definition).value(),
            .run        = std::move(run->value()),
        };
    }

public:
    Application                              app;
    HttpTestServer                           server;
    TemporaryDirectory                       directory;
    std::filesystem::path                    database_file;
    Database                                 database;
    jb::jobu::detail::DatabaseSecretProvider database_secrets;
    RejectingSecretProvider                  secrets;
    StandardAttributeRegistry                registry;
    FakeCronEngine                           cron;
    SequenceUuidGenerator                    generator;
    FakeTimeSource                           time;
    RunRepository                            runs;
    AttemptRepository                        attempts;
    std::unique_ptr<SystemHttpClient>        client;
    std::unique_ptr<HttpAttemptExecutor>     executor;
    std::unique_ptr<ManagementService>       management;
    std::unique_ptr<Scheduler>               scheduler;
};

// Exercise the real framed management adapter against the same service/database that the scheduler will consume.
class ManagementWireEndpoint {
public:
    explicit ManagementWireEndpoint(RealSchedulerFixture& fixture)
    {
        REQUIRE(register_management_methods(_server, *fixture.management, fixture.registry));
        auto device = std::make_unique<MemoryIODevice>();
        _device     = device.get();
        device->open();
        REQUIRE(_server.add_connection(std::move(device)));
    }

    auto call(std::string_view method, JsonValue const& params) -> jb::rpc::detail::ResponseEnvelope
    {
        auto document = serialize_json(jb::rpc::detail::encode_request(_next_id++, method, params));
        REQUIRE(document);
        auto frame = jb::rpc::frame_message(*document);
        REQUIRE(frame);
        _device->inject_input(*frame);

        jb::rpc::StreamFramer framer;
        auto                  bodies = framer.append(_device->take_written_data());
        REQUIRE(bodies);
        REQUIRE(bodies->size() == 1U);
        auto parsed = parse_json(bodies->front());
        REQUIRE(parsed);
        auto response = jb::rpc::detail::decode_response_document(*parsed);
        REQUIRE(response);
        REQUIRE(response->entries.size() == 1U);
        return std::move(response->entries.front());
    }

private:
    jb::rpc::Server _server;
    MemoryIODevice* _device{nullptr};
    std::uint64_t   _next_id{1};
};

auto submitted_http_job(Queue const& queue, JsonValue payload) -> JsonValue
{
    return json_object({
        {"queue_id",   json_string(queue.id.to_string())                                                               },
        {"type",       json_string("http")                                                                             },
        {"schedule",   json_object({{"kind", json_string("once")}, {"at", json_string("1970-01-01T00:01:30.000000Z")}})},
        {"attributes", json_object({{"output.capture", json_string("always")}})                                        },
        {"payload",    std::move(payload)                                                                              },
    });
}

auto rpc_job(jb::rpc::detail::ResponseEnvelope const& response, AttributeRegistry const& registry) -> JobDefinition
{
    REQUIRE(std::holds_alternative<JsonValue>(response.payload));
    auto decoded = job_from_json(std::get<JsonValue>(response.payload), registry);
    REQUIRE(decoded);
    return std::move(decoded).value();
}

void check_payload_rejection(jb::rpc::detail::ResponseEnvelope const& response)
{
    REQUIRE(std::holds_alternative<jb::rpc::RpcError>(response.payload));
    auto const& error = std::get<jb::rpc::RpcError>(response.payload);
    CHECK(error.code == static_cast<std::int64_t>(jb::rpc::ErrorCode::ApplicationError));
    REQUIRE(error.data);
    CHECK(error.data->as_object().at("code").as_string() == "jobu.job.invalid_payload");
}

void seed_legacy_payload(RealSchedulerFixture& fixture, Uuid const& job_id, JsonValue const& payload)
{
    // Old definitions and snapshots enter through fixture storage, never through the new admission boundary.
    auto serialized = validate_and_serialize_job_payload(JobType::Http, payload);
    REQUIRE(serialized);
    for (auto const* sql : {"UPDATE jobu_jobs SET payload_json = :payload WHERE id = :job_id",
                            "UPDATE jobu_runs SET payload_json = :payload WHERE job_id = :job_id"}) {
        Query query{fixture.database};
        REQUIRE(query.prepare(sql));
        REQUIRE(query.bind_value(":payload", std::string{serialized->serialized()}));
        REQUIRE(query.bind_value(":job_id", uuid_to_storage(job_id)));
        REQUIRE(query.exec());
    }
}

auto all_terminal(RealSchedulerFixture& fixture, std::vector<CreatedJob> const& jobs) -> bool
{
    for (auto const& job : jobs) {
        if (!fixture.has_run_state(job.run.id, RunState::Succeeded)) {
            return false;
        }
    }
    return true;
}

auto prefix_count(std::vector<HttpTestRequest> const& requests, std::size_t limit, std::string_view prefix)
    -> std::size_t
{
    auto result = std::size_t{0};
    for (auto index = std::size_t{0}; index < limit; ++index) {
        result += requests[index].target.starts_with(prefix) ? 1U : 0U;
    }
    return result;
}

} // anonymous namespace

TEST_CASE("admitted HTTP methods reach the wire from canonical definitions and snapshots",
          "[jobu][scheduler][http][integration][admission]")
{
    struct MethodCase {
        std::optional<std::string> submitted;
        std::string                canonical;
    };

    for (auto const& method : std::array<MethodCase, 9>{
             {
              {.submitted = "get", .canonical = "GET"},
              {.submitted = "Get", .canonical = "GET"},
              {.submitted = "GET", .canonical = "GET"},
              {.submitted = "pOsT", .canonical = "POST"},
              {.submitted = "pUt", .canonical = "PUT"},
              {.submitted = "pAtCh", .canonical = "PATCH"},
              {.submitted = "dElEtE", .canonical = "DELETE"},
              {.submitted = "hEaD", .canonical = "HEAD"},
              {.submitted = std::nullopt, .canonical = "GET"},
              }
    }) {
        DYNAMIC_SECTION(method.submitted.value_or("omitted"))
        {
            auto                   fixture = RealSchedulerFixture{};
            ManagementWireEndpoint endpoint{fixture};
            auto const             queue   = fixture.create_queue("methods", 1, 1);
            auto                   payload = http_payload(fixture.server.url("/method"));
            if (method.submitted) {
                std::get<JsonValue::Object>(payload.data).emplace("method", json_string(*method.submitted));
            }
            auto const created =
                rpc_job(endpoint.call("job.create", submitted_http_job(queue, payload)), fixture.registry);
            auto expected = payload;
            std::get<JsonValue::Object>(expected.data).insert_or_assign("method", json_string(method.canonical));
            CHECK(created.payload == expected);
            auto stored = fixture.management->get_job(created.id);
            REQUIRE(stored);
            CHECK(stored->payload == expected);
            auto run = fixture.runs.find_schedule_owned(created.id);
            REQUIRE(run);
            REQUIRE(*run);
            CHECK((**run).payload == expected);

            // The fixture advertises the nonempty representation's length but sends no body for exact HEAD.
            // A mere custom HEAD string without libcurl's no-body behavior would fail this completion check.
            fixture.server.enqueue_response({.body = bytes("representation")});
            fixture.server.release_responses();
            REQUIRE(fixture.scheduler->start());
            REQUIRE(fixture.process_until([&] { return fixture.has_run_state((**run).id, RunState::Succeeded); }));
            auto const requests = fixture.server.requests();
            REQUIRE(requests.size() == 1U);
            CHECK(requests.front().method == method.canonical);
            CHECK(requests.front().body.empty());
            CHECK(fixture.find_run((**run).id).payload == expected);
            auto output = fixture.find_output({.run_id = (**run).id, .attempt_number = 1});
            REQUIRE(output);
            REQUIRE(output->stdout_bytes);
            CHECK(byte_text(*output->stdout_bytes) == (method.canonical == "HEAD" ? "" : "representation"));
            REQUIRE(output->stderr_bytes);
            CHECK(byte_text(*output->stderr_bytes).find("Content-Length: 14") != std::string_view::npos);
            fixture.stop_after_idle();
        }
    }
}

TEST_CASE("admitted HTTP bodies and rejected replacements preserve the existing policies",
          "[jobu][scheduler][http][integration][admission]")
{
    for (auto const* method : {"gEt", "pOsT", "pUt", "pAtCh", "dElEtE"}) {
        DYNAMIC_SECTION(method)
        {
            auto                   fixture = RealSchedulerFixture{};
            ManagementWireEndpoint endpoint{fixture};
            auto const             queue  = fixture.create_queue("bodies", 1, 1);
            auto                   params = submitted_http_job(queue, http_payload(fixture.server.url("/body")));
            auto const             empty  = storage_snapshot(fixture.database);

            auto  invalid = params;
            auto& invalid_payload =
                std::get<JsonValue::Object>(std::get<JsonValue::Object>(invalid.data).at("payload").data);
            invalid_payload.emplace("method", json_string("hEaD"));
            invalid_payload.emplace("body",
                                    json_object({
                                        {"encoding", json_string("utf8")},
                                        {"data",     json_string("")    }
            }));
            check_payload_rejection(endpoint.call("job.create", invalid));
            CHECK(storage_snapshot(fixture.database) == empty);

            auto const created = rpc_job(endpoint.call("job.create", params), fixture.registry);
            auto const before  = storage_snapshot(fixture.database);
            auto       update  = json_object({
                {"job_id",            json_string(created.id.to_string())                    },
                {"expected_revision", JsonValue{.data = std::uint64_t{1}}                    },
                {"payload",           std::get<JsonValue::Object>(invalid.data).at("payload")},
            });
            check_payload_rejection(endpoint.call("job.update", update));
            CHECK(storage_snapshot(fixture.database) == before);

            // Replace before dispatch; the eligible snapshot must carry exactly the admitted replacement.
            auto& replacement =
                std::get<JsonValue::Object>(std::get<JsonValue::Object>(update.data).at("payload").data);
            replacement.at("method") = json_string(method);
            replacement.at("body")   = json_object({
                {"encoding", json_string("base64")},
                {"data",     json_string("AEH/")  }
            });
            auto const updated       = rpc_job(endpoint.call("job.update", update), fixture.registry);
            CHECK(updated.revision == 2);
            auto run = fixture.runs.find_schedule_owned(created.id);
            REQUIRE(run);
            REQUIRE(*run);
            CHECK((**run).payload == updated.payload);

            fixture.server.release_responses();
            REQUIRE(fixture.scheduler->start());
            REQUIRE(fixture.process_until([&] { return fixture.has_run_state((**run).id, RunState::Succeeded); }));
            auto const requests = fixture.server.requests();
            REQUIRE(requests.size() == 1U);
            CHECK(requests.front().method == updated.payload.as_object().at("method").as_string());
            CHECK(requests.front().body == ByteBuffer{std::byte{0}, std::byte{0x41}, std::byte{0xff}});
            fixture.stop_after_idle();
        }
    }
}

TEST_CASE("legacy HTTP spelling and lowercase head body behavior survive metadata edits on the wire",
          "[jobu][scheduler][http][integration][legacy]")
{
    for (auto const* method : {"get", "head", "CUSTOM"}) {
        DYNAMIC_SECTION(method)
        {
            auto       fixture = RealSchedulerFixture{};
            auto const queue   = fixture.create_queue("legacy", 1, 1);
            auto const job     = fixture.create_http_job(queue,
                                                         "/legacy",
                                                         {
                                                             {"output.capture", {.data = std::string{"always"}}}
            });
            auto       legacy  = http_payload(fixture.server.url("/legacy"));
            auto&      fields  = std::get<JsonValue::Object>(legacy.data);
            fields.emplace("method", json_string(method));
            fields.emplace("body",
                           json_object({
                               {"encoding", json_string("utf8")    },
                               {"data",     json_string("old-body")}
            }));
            seed_legacy_payload(fixture, job.definition.id, legacy);

            auto updated = fixture.management->update_job(
                {.job_id = job.definition.id, .expected_revision = 1, .type = JobType::Http, .priority = 5});
            REQUIRE(updated);
            CHECK(updated->payload == legacy);
            CHECK(fixture.find_run(job.run.id).payload == legacy);

            fixture.server.enqueue_response({.body = bytes("legacy-response")});
            fixture.server.release_responses();
            REQUIRE(fixture.scheduler->start());
            REQUIRE(fixture.process_until([&] { return fixture.has_run_state(job.run.id, RunState::Succeeded); }));
            auto const requests = fixture.server.requests();
            REQUIRE(requests.size() == 1U);
            CHECK(requests.front().method == method);
            CHECK(byte_text(requests.front().body) == "old-body");
            auto output = fixture.find_output({.run_id = job.run.id, .attempt_number = 1});
            REQUIRE(output);
            REQUIRE(output->stdout_bytes);
            CHECK(byte_text(*output->stdout_bytes) == "legacy-response");
            CHECK(fixture.find_run(job.run.id).payload == legacy);
            fixture.stop_after_idle();
        }
    }
}

TEST_CASE("real HTTP scheduling commits attempts before network observation and respects capacity",
          "[jobu][scheduler][http][integration][sqlite]")
{
    auto fixture = RealSchedulerFixture{
        {
         .cli_concurrency      = 1,
         .http_concurrency     = 3,
         .candidate_batch_size = 20,
         }
    };
    REQUIRE(fixture.scheduler->start());

    auto const narrow    = fixture.create_queue("narrow", 1, 1);
    auto const wide      = fixture.create_queue("wide", 1, 2);
    auto       http_jobs = std::vector<CreatedJob>{};
    http_jobs.push_back(fixture.create_http_job(narrow, "/narrow-1"));
    http_jobs.push_back(fixture.create_http_job(narrow, "/narrow-2"));
    http_jobs.push_back(fixture.create_http_job(wide, "/wide-1"));
    http_jobs.push_back(fixture.create_http_job(wide, "/wide-2"));
    http_jobs.push_back(fixture.create_http_job(wide, "/wide-3"));
    auto const cli = fixture.create_cli_job(narrow, "/remains-pending");

    // This direct-service fixture does not connect the management signal to the scheduler; process only the explicit
    // scheduler timer so curl drive tasks remain pending while the durable running boundary is inspected.
    fixture.scheduler->request_rescan();
    REQUIRE(fixture.app.process_events(EventFlag::Timers, 0) != ProcessEventsResult::Failed);
    CHECK(fixture.server.requests().empty());
    CHECK(fixture.client->active_request_count() == 3U);

    auto narrow_running = std::size_t{0};
    auto wide_running   = std::size_t{0};
    for (auto const& job : http_jobs) {
        auto const run     = fixture.find_run(job.run.id);
        auto       attempt = fixture.attempts.find(job.run.id, 1);
        REQUIRE(attempt);
        if (run.state == RunState::Running) {
            REQUIRE(attempt->has_value());
            CHECK(attempt->value().state == AttemptState::Running);
            narrow_running += run.queue_id == narrow.id ? 1U : 0U;
            wide_running   += run.queue_id == wide.id ? 1U : 0U;
        }
        else {
            CHECK(run.state == RunState::Scheduled);
            CHECK_FALSE(attempt->has_value());
        }
    }
    CHECK(narrow_running == 1U);
    CHECK(wide_running == 2U);
    CHECK(fixture.find_run(cli.run.id).state == RunState::Scheduled);
    auto cli_attempt = fixture.attempts.find(cli.run.id, 1);
    REQUIRE(cli_attempt);
    CHECK_FALSE(cli_attempt->has_value());

    REQUIRE(fixture.process_until([&fixture]() -> bool { return fixture.server.requests().size() == 3U; }));
    CHECK(fixture.client->active_request_count() == 3U);
    REQUIRE(fixture.app.process_events(EventFlag::All, 0) != ProcessEventsResult::Failed);
    CHECK(fixture.server.requests().size() == 3U);

    fixture.server.release_responses();
    REQUIRE(fixture.process_until([&fixture, &http_jobs]() -> bool { return all_terminal(fixture, http_jobs); }));
    CHECK(fixture.server.requests().size() == http_jobs.size());
    CHECK(fixture.find_run(cli.run.id).state == RunState::Scheduled);
    fixture.stop_after_idle();
}

TEST_CASE("real HTTP scheduling preserves smooth weighted queue fairness",
          "[jobu][scheduler][http][integration][sqlite]")
{
    auto fixture = RealSchedulerFixture{
        {
         .cli_concurrency      = 1,
         .http_concurrency     = 1,
         .candidate_batch_size = 2,
         }
    };
    auto const light = fixture.create_queue("light", 1, 1);
    auto const heavy = fixture.create_queue("heavy", 2, 1);
    auto       jobs  = std::vector<CreatedJob>{};
    for (auto index = std::size_t{0}; index < 6; ++index) {
        jobs.push_back(fixture.create_http_job(light, "/light-" + std::to_string(index)));
    }
    for (auto index = std::size_t{0}; index < 6; ++index) {
        jobs.push_back(fixture.create_http_job(heavy, "/heavy-" + std::to_string(index)));
    }
    fixture.server.release_responses();

    REQUIRE(fixture.scheduler->start());
    REQUIRE(fixture.process_until([&fixture, &jobs]() -> bool { return all_terminal(fixture, jobs); }));
    auto const requests = fixture.server.requests();
    REQUIRE(requests.size() == 12U);
    for (auto const prefix : {std::size_t{3}, std::size_t{6}, std::size_t{9}}) {
        CAPTURE(prefix);
        CHECK(prefix_count(requests, prefix, "/light-") == prefix / 3U);
        CHECK(prefix_count(requests, prefix, "/heavy-") == (prefix * 2U) / 3U);
    }
    fixture.stop_after_idle();
}

TEST_CASE("real HTTP completion persists success and terminal capture with the run transition",
          "[jobu][scheduler][http][integration][sqlite]")
{
    SECTION("successful captured response")
    {
        auto fixture = RealSchedulerFixture{};
        fixture.server.enqueue_response({
            .headers = {{.name = "X-Stage", .value = "success"}},
            .body    = bytes("abcdef"),
        });
        fixture.server.release_responses();
        auto const queue = fixture.create_queue("success", 1, 1);
        auto const job   = fixture.create_http_job(queue,
                                                   "/success",
                                                   {
                                                       {"output.capture",         {.data = std::string{"always"}}},
                                                       {"output.http_body_limit", {.data = std::int64_t{4}}      },
        });

        REQUIRE(fixture.scheduler->start());
        REQUIRE(fixture.process_until(
            [&fixture, &job]() -> bool { return fixture.has_run_state(job.run.id, RunState::Succeeded); }));
        auto const run     = fixture.find_run(job.run.id);
        auto const attempt = fixture.find_attempt({.run_id = job.run.id, .attempt_number = 1});
        REQUIRE(run.result);
        REQUIRE(attempt.result);
        CHECK(attempt.state == AttemptState::Completed);
        CHECK(attempt.outcome == AttemptOutcome::Succeeded);
        CHECK(*run.result == *attempt.result);
        CHECK(result_string(*attempt.result, "outcome") == "expected_status");
        CHECK(result_uint(*attempt.result, "status") == 200U);
        auto const& result_body = attempt.result->as_object().at("body").as_object();
        CHECK(result_body.at("captured_bytes").as_uint() == 4U);
        CHECK(result_body.at("total_bytes").as_uint() == 6U);
        CHECK(result_body.at("truncated").as_bool());

        auto output = fixture.find_output({.run_id = job.run.id, .attempt_number = 1});
        REQUIRE(output);
        REQUIRE(output->stdout_bytes);
        CHECK(byte_text(*output->stdout_bytes) == "abef");
        CHECK(output->stdout_truncated);
        REQUIRE(output->stderr_bytes);
        CHECK(byte_text(*output->stderr_bytes).find("X-Stage: success") != std::string_view::npos);
        CHECK_FALSE(output->stderr_truncated);
        CHECK_FALSE(output->capture_lost);
        fixture.stop_after_idle();
    }

    SECTION("terminal unexpected status")
    {
        auto fixture = RealSchedulerFixture{};
        fixture.server.enqueue_response({
            .status_code = 404,
            .reason      = "Not Found",
            .body        = bytes("missing"),
        });
        fixture.server.release_responses();
        auto const queue = fixture.create_queue("terminal", 1, 1);
        auto const job   = fixture.create_http_job(queue, "/terminal");

        REQUIRE(fixture.scheduler->start());
        REQUIRE(fixture.process_until(
            [&fixture, &job]() -> bool { return fixture.has_run_state(job.run.id, RunState::Failed); }));
        auto const run     = fixture.find_run(job.run.id);
        auto const attempt = fixture.find_attempt({.run_id = job.run.id, .attempt_number = 1});
        REQUIRE(run.result);
        REQUIRE(attempt.result);
        CHECK(attempt.outcome == AttemptOutcome::Failed);
        CHECK(*run.result == *attempt.result);
        CHECK(result_string(*attempt.result, "outcome") == "unexpected_status");
        CHECK(result_uint(*attempt.result, "status") == 404U);
        auto output = fixture.find_output({.run_id = job.run.id, .attempt_number = 1});
        REQUIRE(output);
        REQUIRE(output->stdout_bytes);
        CHECK(byte_text(*output->stdout_bytes) == "missing");
        fixture.stop_after_idle();
    }
}

TEST_CASE("real HTTP transport failure retries and commits the recovered attempt",
          "[jobu][scheduler][http][integration][sqlite]")
{
    auto fixture = RealSchedulerFixture{};
    fixture.server.enqueue_response({
        .body                       = bytes("incomplete"),
        .close_after_response_bytes = 0U,
    });
    fixture.server.enqueue_response({.body = bytes("recovered")});
    fixture.server.release_responses();
    auto const queue = fixture.create_queue("transport-retry", 1, 1);
    auto const job   = fixture.create_http_job(queue,
                                               "/transport-retry",
                                               {
                                                   {"retry.initial_delay", {.data = Duration::zero()}},
                                                   {"retry.max_attempts",  {.data = std::int64_t{2}} },
                                                   {"retry.max_delay",     {.data = Duration{10s}}   },
    });

    REQUIRE(fixture.scheduler->start());
    REQUIRE(fixture.process_until(
        [&fixture, &job]() -> bool { return fixture.has_run_state(job.run.id, RunState::Succeeded); }));
    auto attempts = fixture.attempts.list_for_run(job.run.id, 10);
    REQUIRE(attempts);
    REQUIRE(attempts->size() == 2U);
    REQUIRE(attempts->at(0).result);
    CHECK(attempts->at(0).attempt_number == 1U);
    CHECK(attempts->at(0).outcome == AttemptOutcome::Failed);
    CHECK(result_string(*attempts->at(0).result, "error_category") == "receive");
    CHECK(attempts->at(1).attempt_number == 2U);
    CHECK(attempts->at(1).outcome == AttemptOutcome::Succeeded);
    CHECK(fixture.find_output({.run_id = job.run.id, .attempt_number = 1}).has_value());
    CHECK_FALSE(fixture.find_output({.run_id = job.run.id, .attempt_number = 2}).has_value());
    CHECK(fixture.server.requests().size() == 2U);
    fixture.stop_after_idle();
}

TEST_CASE("real HTTP Retry-After delays scheduler retry eligibility", "[jobu][scheduler][http][integration][sqlite]")
{
    auto fixture = RealSchedulerFixture{};
    fixture.server.enqueue_response({
        .status_code = 503,
        .reason      = "Unavailable",
        .headers     = {{.name = "Retry-After", .value = "10"}},
        .body        = bytes("retry-later"),
    });
    fixture.server.enqueue_response({.body = bytes("recovered")});
    fixture.server.release_responses();
    auto const queue = fixture.create_queue("retry-after", 1, 1);
    auto const job   = fixture.create_http_job(queue,
                                               "/retry-after",
                                               {
                                                   {"retry.initial_delay", {.data = Duration{1s}}   },
                                                   {"retry.max_attempts",  {.data = std::int64_t{2}}},
                                                   {"retry.max_delay",     {.data = Duration{20s}}  },
    });

    REQUIRE(fixture.scheduler->start());
    REQUIRE(fixture.process_until(
        [&fixture, &job]() -> bool { return fixture.has_run_state(job.run.id, RunState::RetryWait); }));
    auto waiting = fixture.find_run(job.run.id);
    CHECK(waiting.runnable_at == at_seconds(110));
    CHECK(fixture.server.requests().size() == 1U);
    auto first = fixture.find_attempt({.run_id = job.run.id, .attempt_number = 1});
    REQUIRE(first.result);
    auto const& retry_after = first.result->as_object().at("retry_after").as_object();
    CHECK(retry_after.at("accepted").as_bool());
    CHECK_FALSE(retry_after.at("clamped").as_bool());

    fixture.time.set_utc(at_seconds(109));
    fixture.scheduler->request_rescan();
    REQUIRE(fixture.app.process_events(EventFlag::Timers, 0) != ProcessEventsResult::Failed);
    CHECK(fixture.find_run(job.run.id).state == RunState::RetryWait);
    CHECK(fixture.server.requests().size() == 1U);

    fixture.time.set_utc(at_seconds(110));
    fixture.scheduler->request_rescan();
    REQUIRE(fixture.process_until(
        [&fixture, &job]() -> bool { return fixture.has_run_state(job.run.id, RunState::Succeeded); }));
    auto attempts = fixture.attempts.list_for_run(job.run.id, 10);
    REQUIRE(attempts);
    REQUIRE(attempts->size() == 2U);
    CHECK(attempts->at(1).outcome == AttemptOutcome::Succeeded);
    CHECK(fixture.server.requests().size() == 2U);
    fixture.stop_after_idle();
}

TEST_CASE("real HTTP cancellation retains capacity until durable completion",
          "[jobu][scheduler][http][integration][sqlite]")
{
    auto fixture = RealSchedulerFixture{
        {
         .cli_concurrency  = 1,
         .http_concurrency = 1,
         }
    };
    fixture.server.enqueue_response({
        .body                       = bytes("partial-body"),
        .pause_after_response_bytes = 4U,
    });
    fixture.server.enqueue_response({.body = bytes("follower")});
    fixture.server.release_responses();
    auto const queue    = fixture.create_queue("cancel", 1, 1);
    auto const target   = fixture.create_http_job(queue, "/cancel-target", {}, 10);
    auto const follower = fixture.create_http_job(queue, "/cancel-follower");

    REQUIRE(fixture.scheduler->start());
    REQUIRE(fixture.process_until([&fixture]() -> bool { return fixture.server.wait_for_response_segments(1U, 0ms); }));
    REQUIRE(fixture.app.process_events(EventFlag::All, 0) != ProcessEventsResult::Failed);
    CHECK(fixture.server.requests().size() == 1U);

    auto cancelled = fixture.scheduler->cancel_run(target.run.id);
    REQUIRE(cancelled);
    CHECK(cancelled->disposition == CancelDisposition::Requested);
    CHECK(fixture.find_run(target.run.id).state == RunState::Running);
    CHECK(fixture.find_run(follower.run.id).state == RunState::Scheduled);
    CHECK(fixture.server.requests().size() == 1U);

    REQUIRE(fixture.process_until(
        [&fixture, &target]() -> bool { return fixture.has_run_state(target.run.id, RunState::Cancelled); }));
    auto target_attempt = fixture.find_attempt({.run_id = target.run.id, .attempt_number = 1});
    CHECK(target_attempt.outcome == AttemptOutcome::Cancelled);
    REQUIRE(target_attempt.result);
    CHECK(result_string(*target_attempt.result, "reason") == "cancelled");
    auto output = fixture.find_output({.run_id = target.run.id, .attempt_number = 1});
    REQUIRE(output);
    REQUIRE(output->stdout_bytes);
    CHECK(byte_text(*output->stdout_bytes) == "part");

    fixture.server.release_response_segment();
    REQUIRE(fixture.process_until(
        [&fixture, &follower]() -> bool { return fixture.has_run_state(follower.run.id, RunState::Succeeded); }));
    CHECK(fixture.server.requests().size() == 2U);
    fixture.stop_after_idle();
}

namespace {

// An ordinary header name and explicit false flag prove sensitivity comes from resolution, not credential heuristics.
auto secret_http_payload(std::string url) -> JsonValue
{
    return json_object({
        {"url",     json_string(std::move(url))                                                                      },
        {"method",  json_string("POST")                                                                              },
        {"headers",
         {.data = JsonValue::Array{json_object({{"name", json_string("X-Execution-Token")},
                                                {"value", secret_reference("execution.token")},
                                                {"sensitive", {.data = false}}}),
                                   json_object(
                                       {{"name", json_string("X-Public")}, {"value", json_string("public-value")}})}}},
        {"body",    secret_reference("execution.body")                                                               },
    });
}

auto recorded_header(HttpTestRequest const& request, std::string_view name) -> std::optional<std::string>
{
    for (auto const& header : request.headers) {
        if (header.name == name) {
            return header.value;
        }
    }
    return std::nullopt;
}

} // namespace

TEST_CASE("real HTTP secret headers and binary bodies rotate per retry with faithful response capture",
          "[jobu][scheduler][http][secrets][integration][sqlite]")
{
    for (auto const* mode : {"blocking", "reschedule"}) {
        for (auto const* capture : {"always", "on_error", "none"}) {
            CAPTURE(mode, capture);
            SecretExecutionLogGuard logs;
            RealSchedulerFixture    fixture{{}, true};
            SecretService           secrets{fixture.database, fixture.time};
            auto const              first_body  = bytes(std::string{"stage89-private-first-body"} + '\0' + '\xff');
            auto const              second_body = bytes(std::string{"stage89-private-second-body"} + '\xff' + '\0');
            REQUIRE(secrets.set({.name = "execution.token", .value = bytes("stage89-private-first-header")}));
            REQUIRE(secrets.set({.name = "execution.body", .value = first_body}));
            fixture.server.enqueue_response({.status_code = 503,
                                             .reason      = "Unavailable",
                                             .headers = {{.name = "X-Echo", .value = "stage89-private-first-header"}},
                                             .body    = first_body});
            fixture.server.enqueue_response(
                {.headers = {{.name = "X-Echo", .value = "stage89-private-second-header"}}, .body = second_body});
            fixture.server.release_responses();
            auto const payload = secret_http_payload(fixture.server.url("/secret-rotation"));
            auto       job     = fixture.create_job(fixture.create_queue("secret-rotation", 1, 1),
                                                    JobType::Http,
                                                    payload,
                                                    {
                                                        {"retry.initial_delay", {.data = Duration{10s}}       },
                                                        {"retry.max_delay",     {.data = Duration{10s}}       },
                                                        {"retry.max_attempts",  {.data = std::int64_t{2}}     },
                                                        {"retry.jitter",        {.data = 0.0}                 },
                                                        {"retry.mode",          {.data = std::string{mode}}   },
                                                        {"output.capture",      {.data = std::string{capture}}},
                                                        {"http.tls_verify",     {.data = false}               }
            },
                                                    0,
                                                    "secret-http-create");
            REQUIRE(fixture.scheduler->start());
            REQUIRE(fixture.process_until([&] { return fixture.has_run_state(job.run.id, RunState::RetryWait); }));
            REQUIRE(fixture.server.requests().size() == 1U);

            // Rotation happens only after the first completion commits, before the second attempt becomes eligible.
            REQUIRE(secrets.set({.name = "execution.token", .value = bytes("stage89-private-second-header")}));
            REQUIRE(secrets.set({.name = "execution.body", .value = second_body}));
            fixture.time.set_utc(at_seconds(109));
            fixture.scheduler->request_rescan();
            REQUIRE(fixture.app.process_events(EventFlag::Timers, 0) != ProcessEventsResult::Failed);
            CHECK(fixture.server.requests().size() == 1U);
            fixture.time.set_utc(at_seconds(110));
            fixture.scheduler->request_rescan();
            REQUIRE(fixture.process_until([&] { return fixture.has_run_state(job.run.id, RunState::Succeeded); }));
            auto requests = fixture.server.requests();
            REQUIRE(requests.size() == 2U);
            CHECK(recorded_header(requests[0], "X-Execution-Token") == "stage89-private-first-header");
            CHECK(recorded_header(requests[1], "X-Execution-Token") == "stage89-private-second-header");
            CHECK(requests[0].body == first_body);
            CHECK(requests[1].body == second_body);

            for (AttemptNumber number : {1U, 2U}) {
                auto       output = fixture.find_output({.run_id = job.run.id, .attempt_number = number});
                auto const retained =
                    std::string_view{capture} == "always" || (std::string_view{capture} == "on_error" && number == 1U);
                if (retained) {
                    REQUIRE(output);
                    CHECK(output->stdout_bytes == (number == 1U ? first_body : second_body));
                    REQUIRE(output->stderr_bytes);
                    CHECK(byte_text(*output->stderr_bytes)
                              .find(number == 1U ? "stage89-private-first-header" : "stage89-private-second-header") !=
                          std::string_view::npos);
                }
                else {
                    CHECK_FALSE(output);
                }
            }
            CHECK(fixture.find_run(job.run.id).payload == payload);
            auto definition = fixture.management->get_job(job.definition.id);
            REQUIRE(definition);
            CHECK(definition->payload == payload);
            check_secret_create_record(fixture.database, payload);
            check_secret_execution_metadata(fixture.database);
            fixture.stop_after_idle();
            logs.check("HTTP TLS verification disabled for job");
        }
    }
}

TEST_CASE("real HTTP redirects retain same-origin secret headers and permanently remove them across origins",
          "[jobu][scheduler][http][secrets][integration][sqlite]")
{
    SecretExecutionLogGuard logs;
    RealSchedulerFixture    fixture{{}, true};
    HttpTestServer          other_origin;
    SecretService           secrets{fixture.database, fixture.time};
    REQUIRE(secrets.set({.name = "execution.token", .value = bytes("stage89-private-redirect-header")}));
    REQUIRE(secrets.set({.name = "execution.body", .value = bytes("stage89-private-redirect-body")}));

    // A -> A -> B -> A also proves a removed header cannot reappear on a later return to its original origin.
    fixture.server.enqueue_response({.status_code = 302,
                                     .reason      = "Found",
                                     .headers = {{.name = "Location", .value = fixture.server.url("/same-origin")}}});
    fixture.server.enqueue_response({.status_code = 302,
                                     .reason      = "Found",
                                     .headers = {{.name = "Location", .value = other_origin.url("/other-origin")}}});
    other_origin.enqueue_response({.status_code = 302,
                                   .reason      = "Found",
                                   .headers     = {{.name = "Location", .value = fixture.server.url("/return")}}});
    fixture.server.enqueue_response({});
    fixture.server.release_responses();
    other_origin.release_responses();
    auto job = fixture.create_job(fixture.create_queue("secret-redirect", 1, 1),
                                  JobType::Http,
                                  secret_http_payload(fixture.server.url("/initial")),
                                  {
                                      {"http.follow_redirects", {.data = true}}
    });
    REQUIRE(fixture.scheduler->start());
    REQUIRE(fixture.process_until([&] { return fixture.has_run_state(job.run.id, RunState::Succeeded); }));
    auto original_requests = fixture.server.requests();
    auto other_requests    = other_origin.requests();
    REQUIRE(original_requests.size() == 3U);
    REQUIRE(other_requests.size() == 1U);
    CHECK(recorded_header(original_requests[0], "X-Execution-Token") == "stage89-private-redirect-header");
    CHECK(recorded_header(original_requests[1], "X-Execution-Token") == "stage89-private-redirect-header");
    CHECK_FALSE(recorded_header(other_requests[0], "X-Execution-Token"));
    CHECK_FALSE(recorded_header(original_requests[2], "X-Execution-Token"));
    CHECK(recorded_header(other_requests[0], "X-Public") == "public-value");
    check_secret_execution_metadata(fixture.database);
    fixture.stop_after_idle();
    logs.check();
}

TEST_CASE("real HTTP secret preparation and transport failures leave safe generated diagnostics",
          "[jobu][scheduler][http][secrets][integration][sqlite]")
{
    for (auto const invalid_value : {false, true}) {
        CAPTURE(invalid_value);
        SecretExecutionLogGuard logs;
        RealSchedulerFixture    fixture{{}, true};
        SecretService           secrets{fixture.database, fixture.time};
        REQUIRE(secrets.set({.name  = "execution.token",
                             .value = bytes(invalid_value ? "stage89-private-rejected\r\nInjected: value"
                                                          : "stage89-private-transport-header")}));
        REQUIRE(secrets.set({.name = "execution.body", .value = bytes("stage89-private-transport-body")}));
        fixture.server.reset_next_request_after_headers();
        auto job = fixture.create_job(fixture.create_queue("secret-failure", 1, 1),
                                      JobType::Http,
                                      secret_http_payload(fixture.server.url("/failure")),
                                      {
                                          {"retry.max_attempts", {.data = std::int64_t{1}}}
        });
        REQUIRE(fixture.scheduler->start());
        REQUIRE(fixture.process_until([&] { return fixture.has_run_state(job.run.id, RunState::Failed); }));
        auto attempt = fixture.find_attempt({.run_id = job.run.id, .attempt_number = 1});
        REQUIRE(attempt.result);
        auto result = serialize_json(*attempt.result);
        REQUIRE(result);
        if (invalid_value) {
            CHECK(fixture.server.accepted_connection_count() == 0U);
            CHECK(result->find("jobu.secret.invalid_value") != std::string::npos);
        }
        else {
            CHECK(fixture.server.accepted_connection_count() >= 1U);
            CHECK(result_string(*attempt.result, "outcome") == "transport_error");
        }
        check_secret_execution_metadata(fixture.database);
        fixture.stop_after_idle();
        logs.check();
    }
}
