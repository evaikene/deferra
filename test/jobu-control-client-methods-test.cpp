#include "application.hpp"
#include "attribute_registry.hpp"
#include "client.hpp"
#include "control_client.hpp"
#include "control_json.hpp"
#include "framing.hpp"
#include "history_json.hpp"
#include "json.hpp"
#include "management_json.hpp"
#include "protocol_priv.hpp"
#include "secret_json.hpp"
#include "statistics_json.hpp"
#include "support/catch_utils.hpp" // IWYU pragma: keep for Catch::StringMaker specializations
#include "support/memory_io_device.hpp"
#include "system_info.hpp"
#include "utc_timestamp.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace jb::core;
using namespace jb::jobu;
using namespace jb::rpc;
using jb::test::MemoryIODevice;

namespace {

auto frame(JsonValue const& value) -> std::string
{
    auto encoded = serialize_json(value);
    REQUIRE(encoded);
    auto framed = frame_message(*encoded);
    REQUIRE(framed);
    return std::move(framed).value();
}

auto success(std::uint64_t id, JsonValue const& value) -> std::string
{
    return frame(detail::encode_success_response(RequestId{id}, value));
}

auto remote_error(std::uint64_t id) -> std::string
{
    return frame(detail::encode_error_response(RequestId{id},
                                               RpcError{
                                                   .code    = static_cast<std::int64_t>(ErrorCode::ApplicationError),
                                                   .message = "Represented error",
                                               }));
}

auto encoded(jb::core::Result<JsonValue, Error> result) -> JsonValue
{
    REQUIRE(result);
    return std::move(result).value();
}

auto request_body(std::string const& frame_bytes) -> JsonValue
{
    auto const boundary = frame_bytes.find("\r\n\r\n");
    REQUIRE(boundary != std::string::npos);
    auto parsed = parse_json(std::string_view{frame_bytes}.substr(boundary + 4U));
    REQUIRE(parsed);
    return std::move(parsed).value();
}

auto all_methods() -> std::vector<std::string>
{
    return {"system.info",    "system.stats", "queue.create", "queue.get",     "queue.list",        "queue.update",
            "queue.suspend",  "queue.resume", "queue.delete", "queue.stats",   "job.create",        "job.get",
            "job.list",       "job.update",   "job.suspend",  "job.resume",    "job.move",          "job.delete",
            "job.run_now",    "run.get",      "run.list",     "run.cancel",    "attempt.get",       "attempt.list",
            "attempt.output", "secret.set",   "secret.list",  "secret.delete", "schedule.validate", "schedule.next"};
}

struct Fixture {
    Application                    app{0, nullptr};
    StandardAttributeRegistry      attributes;
    MemoryIODevice                 device;
    std::unique_ptr<Client>        rpc;
    std::unique_ptr<ControlClient> typed;

    Fixture()
    {
        device.open();
        rpc   = std::make_unique<Client>(device);
        typed = std::make_unique<ControlClient>(*rpc, attributes);
    }

    void drain_tasks() { REQUIRE(app.process_events(EventFlag::Tasks) != ProcessEventsResult::Failed); }

    void initialize()
    {
        REQUIRE(typed->initialize());
        device.inject_input(success(1U,
                                    system_info_to_json(SystemInfo{
                                        .daemon_version = "test",
                                        .api_version    = {.major = 1, .minor = 3},
                                        .capabilities   = all_methods()
        })));
        drain_tasks();
        static_cast<void>(device.take_written_data());
    }
};

using Submit = std::function<jb::core::Result<ControlCallId, Error>(ControlClient&)>;

struct MethodCase {
    std::string_view                method;
    std::optional<std::string_view> parameter;
    bool                            mutation;
    Submit                          submit;

    MethodCase(std::string_view method, std::optional<std::string_view> parameter, bool mutation, Submit submit)
        : method{method}
        , parameter{parameter}
        , mutation{mutation}
        , submit{std::move(submit)}
    {}
};

struct ReplyCase {
    Submit      submit;
    JsonValue   result;
    std::size_t reply_index;

    ReplyCase(Submit submit, JsonValue result, std::size_t reply_index)
        : submit{std::move(submit)}
        , result{std::move(result)}
        , reply_index{reply_index}
    {}
};

} // anonymous namespace

TEST_CASE("Every typed method selects its capability and shared request shape", "[jobu][client]")
{
    Fixture fixture;
    fixture.initialize();

    auto id = Uuid::parse("00112233-4455-6677-8899-aabbccddeeff");
    auto at = parse_utc_timestamp("2026-07-21T21:00:00Z");
    REQUIRE(id);
    REQUIRE(at);

    JsonValue payload;
    payload.data        = JsonValue::Object{};
    auto const selector = QueueSelector{id.value()};
    auto const key      = AttemptKey{.run_id = id.value(), .attempt_number = 1};
    auto const cron     = CronSchedule{.expression = "0 0 * * *"};

    auto cases = std::vector<MethodCase>{
        {"system.info",       std::nullopt,     false, [](auto& client) { return client.get_system_info(); }                     },
        {"system.stats",      "group_by",       false, [](auto& client) { return client.system_statistics(StatisticsRequest{}); }},
        {"queue.create",      "name",           true,  [](auto& client) { return client.create_queue({.name = "queue"}); }       },
        {"queue.get",         "queue_id",       false, [&](auto& client) { return client.get_queue(selector); }                  },
        {"queue.list",        "limit",          false, [](auto& client) { return client.list_queues(QueueListRequest{}); }       },
        {"queue.update",
         "weight",                              true,
         [&](auto& client) { return client.update_queue({.queue = selector, .weight = 2}); }                                     },
        {"queue.suspend",     "queue_id",       true,  [&](auto& client) { return client.suspend_queue(selector); }              },
        {"queue.resume",      "queue_id",       true,  [&](auto& client) { return client.resume_queue(selector); }               },
        {"queue.delete",      "queue_id",       true,  [&](auto& client) { return client.delete_queue(selector); }               },
        {"queue.stats",
         "queue_id",                            false,
         [&](auto& client) { return client.queue_statistics(QueueStatisticsQuery{.selector = selector}); }                       },
        {"job.create",
         "schedule",                            true,
         [&](auto& client) {
             return client.create_job({.queue = selector, .schedule = ImmediateSchedule{}, .payload = payload});
         }                                                                                                                       },
        {"job.get",           "job_id",         false, [&](auto& client) { return client.get_job(*id); }                         },
        {"job.list",          "limit",          false, [](auto& client) { return client.list_jobs(JobListRequest{}); }           },
        {"job.update",
         "expected_revision",                   true,
         [&](auto& client) { return client.update_job({.job_id = *id, .expected_revision = 1, .priority = 2}); }                 },
        {"job.suspend",       "job_id",         true,  [&](auto& client) { return client.suspend_job(*id); }                     },
        {"job.resume",        "job_id",         true,  [&](auto& client) { return client.resume_job(*id); }                      },
        {"job.move",
         "target_queue_id",                     true,
         [&](auto& client) {
             return client.move_job({.job_id = *id, .expected_revision = 1, .target_queue = selector});
         }                                                                                                                       },
        {"job.delete",
         "expected_revision",                   true,
         [&](auto& client) { return client.delete_job({.job_id = *id, .expected_revision = 1}); }                                },
        {"job.run_now",       "job_id",         true,  [&](auto& client) { return client.run_now({.job_id = *id}); }             },
        {"run.get",           "run_id",         false, [&](auto& client) { return client.get_run(*id); }                         },
        {"run.list",          "limit",          false, [](auto& client) { return client.list_runs(RunQuery{}); }                 },
        {"run.cancel",        "run_id",         true,  [&](auto& client) { return client.cancel_run(*id); }                      },
        {"attempt.get",       "attempt_number", false, [&](auto& client) { return client.get_attempt(key); }                     },
        {"attempt.list",
         "run_id",                              false,
         [&](auto& client) { return client.list_attempts(AttemptQuery{.run_id = *id}); }                                         },
        {"attempt.output",
         "channel",                             false,
         [&](auto& client) { return client.read_attempt_output({.attempt = key, .channel = OutputChannel::Stdout}); }            },
        {"secret.set",        "value",          true,  [](auto& client) { return client.set_secret({.name = "secret"}); }        },
        {"secret.list",       "limit",          false, [](auto& client) { return client.list_secrets(SecretListRequest{}); }     },
        {"secret.delete",     "name",           true,  [](auto& client) { return client.delete_secret("secret"); }               },
        {"schedule.validate", "schedule",       false, [&](auto& client) { return client.validate_schedule(cron); }              },
        {"schedule.next",
         "after",                               false,
         [&](auto& client) { return client.next_schedule_occurrences({.schedule = cron, .after = *at}); }                        },
    };

    auto failure = std::optional<ControlFailure>{};
    fixture.typed->call_failed.connect([&](ControlCallId, ControlFailure const& value) { failure = value; });

    for (auto const& test : cases) {
        INFO(test.method);
        auto call = test.submit(*fixture.typed);
        REQUIRE(call);

        auto const  request = request_body(fixture.device.take_written_data());
        auto const& object  = request.as_object();
        CHECK(object.at("method").as_string() == test.method);
        if (test.parameter) {
            REQUIRE(object.contains("params"));
            CHECK(object.at("params").as_object().contains(*test.parameter));
        }
        else {
            CHECK_FALSE(object.contains("params"));
        }

        fixture.typed->cancel_call(*call);
        REQUIRE(failure);
        CHECK(failure->kind == ControlFailureKind::Local);
        CHECK(failure->outcome_unknown == test.mutation);
        failure.reset();
    }
}

TEST_CASE("Every distinct typed result decodes into its public reply alternative", "[jobu][client]")
{
    Fixture fixture;
    fixture.initialize();

    auto id = Uuid::parse("00112233-4455-6677-8899-aabbccddeeff");
    auto at = parse_utc_timestamp("2026-07-21T21:00:00Z");
    REQUIRE(id);
    REQUIRE(at);
    auto attributes = materialize_attributes(fixture.attributes, {}, {}, {});
    REQUIRE(attributes);

    auto queue       = Queue{.id = *id, .name = "queue", .created_at = *at, .updated_at = *at};
    auto job         = JobDefinition{.id         = *id,
                                     .queue_id   = *id,
                                     .schedule   = OnceSchedule{.planned_at = *at},
                                     .attributes = *attributes,
                                     .created_at = *at,
                                     .updated_at = *at};
    job.payload.data = JsonValue::Object{
        {"large", JsonValue{.data = std::string(100000U, 'x')}}
    };

    auto run         = JobRun{.id          = *id,
                              .job_id      = *id,
                              .queue_id    = *id,
                              .planned_at  = *at,
                              .runnable_at = *at,
                              .attributes  = *attributes};
    run.payload.data = JsonValue::Object{};

    auto attempt   = AttemptDetails{};
    attempt.run_id = *id;
    attempt.due_at = *at;

    auto output = AttemptOutputChunk{
        .attempt        = {.run_id = *id, .attempt_number = 1},
        .channel        = OutputChannel::Stdout,
        .status         = OutputStatus::Available,
        .bytes_returned = 65536U,
        .retained_bytes = 65536U,
        .encoding       = OutputEncoding::Base64,
        .data           = ByteBuffer(65536U, std::byte{0x80}
                 )
    };

    auto statistics        = StatisticsPage{};
    statistics.window.from = *at;
    statistics.window.to   = *at + std::chrono::hours{1};
    statistics.groups.emplace_back();

    auto const key   = AttemptKey{.run_id = *id, .attempt_number = 1};
    auto       cases = std::vector<ReplyCase>{
        {[](auto& client) { return client.get_system_info(); },
         system_info_to_json(SystemInfo{.daemon_version = "test",
                                        .api_version    = {.major = 1, .minor = 3},
                                        .capabilities   = all_methods()}),
         ControlReply{SystemInfo{}}.index()},
        {[](auto& client) { return client.system_statistics(StatisticsRequest{}); },
         encoded(statistics_page_to_json(statistics)),
         ControlReply{StatisticsPage{}}.index()},
        {[&](auto& client) { return client.get_queue(*id); },
         encoded(queue_to_json(queue, fixture.attributes)),
         ControlReply{Queue{}}.index()},
        {[](auto& client) { return client.list_queues(QueueListRequest{}); },
         encoded(queue_page_to_json(QueuePage{}, fixture.attributes)),
         ControlReply{QueuePage{}}.index()},
        {[&](auto& client) { return client.get_job(*id); },
         encoded(job_to_json(job, fixture.attributes)),
         ControlReply{JobDefinition{}}.index()},
        {[](auto& client) { return client.list_jobs(JobListRequest{}); },
         encoded(job_page_to_json(JobPage{.items = {job}}, fixture.attributes)),
         ControlReply{JobPage{}}.index()},
        {[&](auto& client) { return client.get_run(*id); },
         encoded(run_details_to_json(run, fixture.attributes)),
         ControlReply{RunDetails{}}.index()},
        {[](auto& client) { return client.list_runs(RunQuery{}); },
         encoded(run_page_to_json(RunPage{})),
         ControlReply{RunPage{}}.index()},
        {[&](auto& client) { return client.cancel_run(*id); },
         encoded(cancel_run_result_to_json(CancelRunResult{.run = run}, fixture.attributes)),
         ControlReply{CancelRunResult{}}.index()},
        {[&](auto& client) { return client.get_attempt(key); },
         encoded(attempt_details_to_json(attempt)),
         ControlReply{AttemptDetails{}}.index()},
        {[&](auto& client) { return client.list_attempts(AttemptQuery{.run_id = *id}); },
         encoded(attempt_page_to_json(AttemptPage{})),
         ControlReply{AttemptPage{}}.index()},
        {[&](auto& client) { return client.read_attempt_output({.attempt = key}); },
         encoded(attempt_output_chunk_to_json(output)),
         ControlReply{AttemptOutputChunk{}}.index()},
        {[](auto& client) { return client.set_secret({.name = "secret"}); },
         encoded(secret_metadata_to_json(SecretMetadata{.name = "secret", .created_at = *at, .updated_at = *at})),
         ControlReply{SecretMetadata{}}.index()},
        {[](auto& client) { return client.list_secrets(SecretListRequest{}); },
         encoded(secret_page_to_json(SecretPage{})),
         ControlReply{SecretPage{}}.index()},
        {[](auto& client) { return client.validate_schedule({.expression = "0 0 * * *"}); },
         schedule_validate_result_to_json(),
         ControlReply{ScheduleValidationReply{}}.index()},
        {[&](auto& client) {
             return client.next_schedule_occurrences({.schedule = {.expression = "0 0 * * *"}, .after = *at});
         }, encoded(schedule_next_result_to_json({*at})),
         ControlReply{ScheduleNextReply{}}.index()},
        {[&](auto& client) { return client.delete_queue(QueueSelector{*id}); },
         JsonValue{},
         ControlReply{EmptyReply{}}.index()},
    };

    auto received = std::vector<std::pair<ControlCallId, std::size_t>>{};
    fixture.typed->reply_received.connect([&](ControlCallId id, ControlReply const& reply) {
        received.emplace_back(id, reply.index());
        if (auto const* chunk = std::get_if<AttemptOutputChunk>(&reply)) {
            CHECK(chunk->data.size() == 65536U);
            CHECK(chunk->data.front() == std::byte{0x80});
        }
        if (auto const* page = std::get_if<JobPage>(&reply)) {
            REQUIRE(page->items.size() == 1U);
            CHECK(page->items.front().payload.as_object().at("large").as_string().size() == 100000U);
        }
    });

    for (auto& test : cases) {
        auto call = test.submit(*fixture.typed);
        REQUIRE(call);
        static_cast<void>(fixture.device.take_written_data());
        fixture.device.inject_input(success(*call + 1U, test.result));
        fixture.drain_tasks();
        REQUIRE_FALSE(received.empty());
        CHECK(received.back() == std::pair{*call, test.reply_index});
    }

    CHECK(received.size() == cases.size());
}

TEST_CASE("Remote errors stay observed and malformed mutation replies stay uncertain", "[jobu][client]")
{
    Fixture fixture;
    fixture.initialize();

    auto failures = std::vector<std::pair<ControlCallId, ControlFailure>>{};
    fixture.typed->call_failed.connect(
        [&](ControlCallId id, ControlFailure const& failure) { failures.emplace_back(id, failure); });

    auto remote = fixture.typed->delete_secret("secret");
    REQUIRE(remote);
    static_cast<void>(fixture.device.take_written_data());
    fixture.device.inject_input(remote_error(*remote + 1U));
    fixture.drain_tasks();
    REQUIRE(failures.size() == 1U);
    CHECK(failures.back().second.kind == ControlFailureKind::Remote);
    CHECK_FALSE(failures.back().second.outcome_unknown);

    auto malformed = fixture.typed->delete_secret("secret");
    REQUIRE(malformed);
    static_cast<void>(fixture.device.take_written_data());
    fixture.device.inject_input(success(*malformed + 1U, JsonValue{.data = true}));
    fixture.drain_tasks();
    REQUIRE(failures.size() == 2U);
    CHECK(failures.back().second.kind == ControlFailureKind::Local);
    CHECK(failures.back().second.outcome_unknown);
    CHECK(std::get<Error>(failures.back().second.error).code == "jobu.client.invalid_response");

    auto queued_error = fixture.typed->delete_secret("secret");
    REQUIRE(queued_error);
    static_cast<void>(fixture.device.take_written_data());
    fixture.device.inject_input(remote_error(*queued_error + 1U));
    fixture.typed->cancel_call(*queued_error);
    REQUIRE(failures.size() == 3U);
    CHECK_FALSE(failures.back().second.outcome_unknown);
    fixture.drain_tasks();
    CHECK(failures.size() == 3U);
}

TEST_CASE("Unknown response members are ignored and nullable output metadata stays absent", "[jobu][client]")
{
    Fixture fixture;
    fixture.initialize();

    auto id = Uuid::parse("00112233-4455-6677-8899-aabbccddeeff");
    auto at = parse_utc_timestamp("2026-07-21T21:00:00Z");
    REQUIRE(id);
    REQUIRE(at);

    auto result =
        encoded(secret_metadata_to_json(SecretMetadata{.name = "secret", .created_at = *at, .updated_at = *at}));
    std::get<JsonValue::Object>(result.data).emplace("future", JsonValue{.data = std::string{"ignored"}});

    auto replies = std::vector<ControlReply>{};
    fixture.typed->reply_received.connect([&](ControlCallId, ControlReply const& reply) { replies.push_back(reply); });

    auto metadata = fixture.typed->set_secret({.name = "secret"});
    REQUIRE(metadata);
    static_cast<void>(fixture.device.take_written_data());
    fixture.device.inject_input(success(*metadata + 1U, result));
    fixture.drain_tasks();
    REQUIRE(replies.size() == 1U);
    CHECK(std::get<SecretMetadata>(replies.back()).name == "secret");

    auto output = AttemptOutputChunk{
        .attempt = {.run_id = *id, .attempt_number = 1},
        .channel = OutputChannel::Stdout,
        .status  = OutputStatus::NotCaptured
    };
    auto output_call = fixture.typed->read_attempt_output({.attempt = output.attempt});
    REQUIRE(output_call);
    static_cast<void>(fixture.device.take_written_data());
    fixture.device.inject_input(success(*output_call + 1U, encoded(attempt_output_chunk_to_json(output))));
    fixture.drain_tasks();
    REQUIRE(replies.size() == 2U);
    auto const& decoded = std::get<AttemptOutputChunk>(replies.back());
    CHECK_FALSE(decoded.total_bytes);
    CHECK_FALSE(decoded.omitted_bytes);
    CHECK_FALSE(decoded.next_offset);
}
