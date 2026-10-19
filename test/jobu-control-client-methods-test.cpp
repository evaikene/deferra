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

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
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

    void initialize(std::uint32_t minor = 6U)
    {
        REQUIRE(typed->initialize());
        device.inject_input(success(1U,
                                    system_info_to_json(SystemInfo{
                                        .daemon_version = "test",
                                        .api_version    = {.major = 1, .minor = minor},
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

auto revision_queue_methods() -> std::vector<MethodCase>
{
    return {
        {"queue.create",  "name",       true,  [](auto& client) { return client.create_queue({.name = "queue"}); }    },
        {"queue.get",     "queue_name", false, [](auto& client) { return client.get_queue(std::string{"queue"}); }    },
        {"queue.get",
         "include_deleted",             false,
         [](auto& client) {
             return client.get_queue(GetQueueRequest{.queue = std::string{"queue"}, .include_deleted = true});
         }                                                                                                            },
        {"queue.list",    "limit",      false, [](auto& client) { return client.list_queues(QueueListRequest{}); }    },
        {"queue.update",
         "expected_revision",           true,
         [](auto& client) {
             return client.update_queue({.queue = std::string{"queue"}, .expected_revision = 1, .weight = 2});
         }                                                                                                            },
        {"queue.suspend", "queue_name", true,  [](auto& client) { return client.suspend_queue(std::string{"queue"}); }},
        {"queue.resume",  "queue_name", true,  [](auto& client) { return client.resume_queue(std::string{"queue"}); } },
    };
}

} // anonymous namespace

TEST_CASE("Revision-bearing queue calls reject older peers without accepting or writing a call",
          "[jobu][client][queue]")
{
    for (auto minor : {0U, 1U, 2U, 3U, 4U, 5U}) {
        CAPTURE(minor);
        Fixture fixture;
        fixture.initialize(minor);

        auto replies  = 0;
        auto failures = 0;
        fixture.typed->reply_received.connect(fixture.typed.get(),
                                              [&](ControlCallId, ControlReply const&) { ++replies; });
        fixture.typed->call_failed.connect(fixture.typed.get(),
                                           [&](ControlCallId, ControlFailure const&) { ++failures; });

        for (auto const& method : revision_queue_methods()) {
            CAPTURE(method.method);
            auto rejected = method.submit(*fixture.typed);
            REQUIRE_FALSE(rejected);
            CHECK(rejected.error().category == ErrorCategory::Unsupported);
            CHECK(rejected.error().code == "jobu.client.unsupported_feature");
            CHECK(rejected.error().message == "Queue revision support requires JobU API 1.6 or newer");
            CHECK(fixture.device.take_written_data().empty());
        }
        fixture.drain_tasks();
        CHECK(replies == 0);
        CHECK(failures == 0);

        // Neither local nor wire IDs are consumed. Queue calls with unrelated result contracts still work.
        auto deleted = fixture.typed->delete_queue(std::string{"queue"});
        REQUIRE(deleted);
        CHECK(*deleted == 1U);
        auto wire = request_body(fixture.device.take_written_data());
        CHECK(wire.as_object().at("id").as_uint() == 2U);
        CHECK(wire.as_object().at("method").as_string() == "queue.delete");
        fixture.device.inject_input(success(2U, JsonValue{}));
        fixture.drain_tasks();
        CHECK(replies == 1);

        auto stats = fixture.typed->queue_statistics(CursorRequest{.cursor = "retained-query"});
        REQUIRE(stats);
        CHECK(*stats == 2U);
        wire = request_body(fixture.device.take_written_data());
        CHECK(wire.as_object().at("id").as_uint() == 3U);
        CHECK(wire.as_object().at("method").as_string() == "queue.stats");
        fixture.device.inject_input(remote_error(3U));
        fixture.drain_tasks();
        CHECK(failures == 1);
        CHECK(fixture.device.take_written_data().empty());
    }
}

TEST_CASE("API 1.6 and newer accept every revision-bearing queue method", "[jobu][client][queue]")
{
    for (auto minor : {6U, 7U}) {
        CAPTURE(minor);
        Fixture fixture;
        fixture.initialize(minor);
        auto queue    = Queue{.id = Uuid::parse("00112233-4455-6677-8899-aabbccddeeff").value(), .name = "queue"};
        auto received = 0;
        fixture.typed->reply_received.connect(fixture.typed.get(),
                                              [&](ControlCallId, ControlReply const&) { ++received; });

        for (auto const& method : revision_queue_methods()) {
            CAPTURE(method.method);
            auto call = method.submit(*fixture.typed);
            REQUIRE(call);
            auto const wire = request_body(fixture.device.take_written_data());
            CHECK(wire.as_object().at("method").as_string() == method.method);
            REQUIRE(method.parameter);
            CHECK(wire.as_object().at("params").as_object().contains(std::string{*method.parameter}));

            auto result = method.method == "queue.list"
                            ? encoded(queue_page_to_json(QueuePage{.items = {queue}}, fixture.attributes))
                            : encoded(queue_to_json(queue, fixture.attributes));
            fixture.device.inject_input(success(wire.as_object().at("id").as_uint(), result));
            fixture.drain_tasks();
        }
        CHECK(received == 7);
        CHECK(fixture.device.take_written_data().empty());
    }
}

TEST_CASE("Queue peer checks preserve lifecycle and request validation precedence", "[jobu][client][queue]")
{
    for (auto minor : {5U, 6U}) {
        CAPTURE(minor);
        Fixture fixture;
        auto    check_not_ready = [&] {
            for (auto const& method : revision_queue_methods()) {
                auto rejected = method.submit(*fixture.typed);
                REQUIRE_FALSE(rejected);
                CHECK(rejected.error().code == "jobu.client.not_ready");
            }
            auto invalid = fixture.typed->update_queue({.queue = std::string{"queue"}, .weight = 2});
            REQUIRE_FALSE(invalid);
            CHECK(invalid.error().code == "jobu.client.not_ready");
            CHECK(fixture.device.take_written_data().empty());
        };
        check_not_ready();

        auto ready_count = 0;
        fixture.typed->ready.connect(fixture.typed.get(), [&](SystemInfo const&) {
            ++ready_count;
            auto call = fixture.typed->get_queue(std::string{"queue"});
            if (minor < 6U) {
                REQUIRE_FALSE(call);
                CHECK(call.error().code == "jobu.client.unsupported_feature");
                CHECK(fixture.device.take_written_data().empty());
            }
            else {
                REQUIRE(call);
                CHECK_FALSE(fixture.device.take_written_data().empty());
            }
        });
        REQUIRE(fixture.typed->initialize());
        static_cast<void>(fixture.device.take_written_data());
        check_not_ready();
        fixture.device.inject_input(success(1U,
                                            system_info_to_json(SystemInfo{
                                                .daemon_version = "test",
                                                .api_version    = {.major = 1, .minor = minor},
                                                .capabilities   = all_methods()
        })));
        check_not_ready();
        fixture.drain_tasks();
        CHECK(ready_count == 1);

        auto invalid = fixture.typed->update_queue({.queue = std::string{"queue"}, .weight = 2});
        REQUIRE_FALSE(invalid);
        CHECK(invalid.error().code == "jobu.protocol.invalid_request");
        CHECK(fixture.device.take_written_data().empty());
        fixture.typed->close();
        check_not_ready();
    }
}

TEST_CASE("Queue peer checks retain failed-handshake and capability errors", "[jobu][client][queue]")
{
    for (auto major : {1U, 2U}) {
        CAPTURE(major);
        Fixture fixture;
        REQUIRE(fixture.typed->initialize());
        static_cast<void>(fixture.device.take_written_data());
        fixture.device.inject_input(success(1U,
                                            system_info_to_json(SystemInfo{
                                                .daemon_version = "test",
                                                .api_version    = {.major = major, .minor = 6},
                                                .capabilities   = {"system.info"}
        })));
        fixture.drain_tasks();

        for (auto const& method : revision_queue_methods()) {
            auto rejected = method.submit(*fixture.typed);
            REQUIRE_FALSE(rejected);
            CHECK(rejected.error().code == (major == 1U ? "jobu.client.unsupported_method" : "jobu.client.not_ready"));
            CHECK(fixture.device.take_written_data().empty());
        }
    }
}

TEST_CASE("Run state sets require API 1.5 without consuming a call or writing to older peers", "[jobu][client]")
{
    for (auto minor : {0U, 1U, 2U, 3U, 4U}) {
        INFO(minor);
        Fixture fixture;
        fixture.initialize(minor);

        auto replies  = 0;
        auto failures = 0;
        fixture.typed->reply_received.connect(fixture.typed.get(),
                                              [&](ControlCallId, ControlReply const&) { ++replies; });
        fixture.typed->call_failed.connect(fixture.typed.get(),
                                           [&](ControlCallId, ControlFailure const&) { ++failures; });

        for (auto const& states : {
                 std::vector<RunState>{RunState::Failed},
                 std::vector<RunState>{RunState::Interrupted, RunState::Failed}
        }) {
            auto rejected = fixture.typed->list_runs(RunQuery{.filters = {.states = states}});
            REQUIRE_FALSE(rejected);
            CHECK(rejected.error().category == ErrorCategory::Unsupported);
            CHECK(rejected.error().code == "jobu.client.unsupported_feature");
            CHECK(rejected.error().message == "Run-list state sets require JobU API 1.5 or newer");
            CHECK(fixture.device.take_written_data().empty());
        }

        fixture.drain_tasks();
        CHECK(replies == 0);
        CHECK(failures == 0);

        // Rejection must not allocate a local call ID or disable otherwise advertised calls.
        auto accepted = fixture.typed->list_runs(RunQuery{});
        REQUIRE(accepted);
        CHECK(*accepted == 1U);
    }
}

TEST_CASE("API 1.5 and newer preserve run state sets and other query fields on the wire", "[jobu][client]")
{
    auto const job_id = Uuid::parse("00112233-4455-6677-8899-aabbccddeeff");
    REQUIRE(job_id);

    for (auto minor : {5U, 6U}) {
        INFO(minor);
        Fixture fixture;
        fixture.initialize(minor);

        for (auto const& states : {
                 std::vector<RunState>{RunState::Failed},
                 std::vector<RunState>{RunState::Interrupted, RunState::Failed}
        }) {
            auto const query = RunQuery{
                .filters = {.job_id = *job_id, .states = states},
                .limit   = 2,
            };
            auto call = fixture.typed->list_runs(query);
            REQUIRE(call);

            auto const wire = request_body(fixture.device.take_written_data());
            CHECK(wire.as_object().at("method").as_string() == "run.list");
            CHECK(wire.as_object().at("params") == encoded(run_list_request_to_json(query)));
            fixture.device.inject_input(
                success(wire.as_object().at("id").as_uint(), encoded(run_page_to_json(RunPage{}))));
            fixture.drain_tasks();
        }
    }
}

TEST_CASE("Compatible run-list peers retain scalar unfiltered and cursor-only request shapes", "[jobu][client]")
{
    for (auto minor : {3U, 4U, 5U, 6U}) {
        INFO(minor);
        Fixture fixture;
        fixture.initialize(minor);

        auto const requests = std::vector<RunListRequest>{
            RunQuery{.filters = {.state = RunState::Failed}},
            RunQuery{},
            CursorRequest{.cursor = "retained-query-cursor"},
        };
        auto replies = 0;
        fixture.typed->reply_received.connect(fixture.typed.get(), [&](ControlCallId, ControlReply const& reply) {
            CHECK(std::holds_alternative<RunPage>(reply));
            ++replies;
        });

        for (auto const& request : requests) {
            REQUIRE(fixture.typed->list_runs(request));
            auto const wire = request_body(fixture.device.take_written_data());
            CHECK(wire.as_object().at("method").as_string() == "run.list");
            CHECK(wire.as_object().at("params") == encoded(run_list_request_to_json(request)));
            CHECK_FALSE(wire.as_object().at("params").as_object().contains("states"));
            fixture.device.inject_input(
                success(wire.as_object().at("id").as_uint(), encoded(run_page_to_json(RunPage{}))));
            fixture.drain_tasks();
        }
        CHECK(replies == 3);
    }
}

TEST_CASE("Run state sets use shared request validation before checking the peer feature version", "[jobu][client]")
{
    for (auto minor : {4U, 5U}) {
        INFO(minor);
        Fixture fixture;
        fixture.initialize(minor);

        auto const invalid = std::vector<RunQuery>{
            {.filters = {.states = std::vector<RunState>{}}},
            {.filters = {.states = std::vector<RunState>{RunState::Failed, RunState::Failed}}},
            {.filters = {.states = std::vector<RunState>{static_cast<RunState>(255)}}},
            {.filters = {.state = RunState::Failed, .states = std::vector<RunState>{RunState::Interrupted}}},
            {.filters = {.states = std::vector<RunState>{RunState::Failed}}, .limit = 0},
        };
        for (auto const& request : invalid) {
            auto rejected = fixture.typed->list_runs(request);
            REQUIRE_FALSE(rejected);
            CHECK(rejected.error().code == "jobu.protocol.invalid_request");
            CHECK(fixture.device.take_written_data().empty());
        }
    }
}

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
    auto const cron     = CronScheduleInput{.expression = "0 0 * * *"};

    auto cases = std::vector<MethodCase>{
        {"system.info",       std::nullopt,     false, [](auto& client) { return client.get_system_info(); }                     },
        {"system.stats",      "group_by",       false, [](auto& client) { return client.system_statistics(StatisticsRequest{}); }},
        {"queue.create",      "name",           true,  [](auto& client) { return client.create_queue({.name = "queue"}); }       },
        {"queue.get",         "queue_id",       false, [&](auto& client) { return client.get_queue(selector); }                  },
        {"queue.list",        "limit",          false, [](auto& client) { return client.list_queues(QueueListRequest{}); }       },
        {"queue.update",
         "weight",                              true,
         [&](auto& client) { return client.update_queue({.queue = selector, .expected_revision = 1, .weight = 2}); }             },
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
                                     .revision   = 2,
                                     .state      = JobState::Succeeded,
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
                                        .api_version    = {.major = 1, .minor = 6},
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
            CHECK(page->items.front().state == JobState::Succeeded);
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

TEST_CASE("All typed queue replies preserve revision and reject missing or invalid tokens", "[jobu][client][queue]")
{
    Fixture fixture;
    fixture.initialize();
    auto id = Uuid::parse("00112233-4455-6677-8899-aabbccddeeff");
    REQUIRE(id);
    auto queue = Queue{.id = *id, .revision = 9007199254740993ULL, .name = "queue"};
    auto raw   = encoded(queue_to_json(queue, fixture.attributes));

    struct QueueCase {
        Submit submit;
        bool   list;
        bool   mutation;
    };

    auto cases = std::vector<QueueCase>{
        {.submit   = [&](auto& client) { return client.create_queue({.name = "queue"}); },
         .list     = false,
         .mutation = true                                                                                                      },
        {.submit = [&](auto& client) { return client.get_queue(*id); },                    .list = false,     .mutation = false},
        {.submit   = [](auto& client) { return client.list_queues(QueueListRequest{}); },
         .list     = true,
         .mutation = false                                                                                                     },
        {.submit =
             [&](auto& client) {
                 return client.update_queue({.queue = *id, .expected_revision = queue.revision, .weight = 2});
             },                                                                   .list     = false,
         .mutation = true                                                                                                      },
        {.submit = [&](auto& client) { return client.suspend_queue(*id); },                .list = false,     .mutation = true },
        {.submit = [&](auto& client) { return client.resume_queue(*id); },                 .list = false,     .mutation = true },
    };

    auto received = std::size_t{0};
    auto failures = std::vector<ControlFailure>{};
    fixture.typed->reply_received.connect(fixture.typed.get(), [&](ControlCallId, ControlReply const& reply) {
        if (auto const* result = std::get_if<Queue>(&reply)) {
            CHECK(result->revision == queue.revision);
        }
        else {
            auto const* page = std::get_if<QueuePage>(&reply);
            REQUIRE(page != nullptr);
            REQUIRE(page->items.size() == 1U);
            CHECK(page->items.front().revision == queue.revision);
        }
        ++received;
    });
    fixture.typed->call_failed.connect(fixture.typed.get(), [&](ControlCallId, ControlFailure const& failure) {
        failures.push_back(failure);
    });

    for (auto const& test : cases) {
        auto wrap = [&](JsonValue value) {
            if (test.list) {
                return JsonValue{
                    .data = JsonValue::Object{
                                              {"items", JsonValue{.data = JsonValue::Array{std::move(value)}}},
                                              {"next_after_id", JsonValue{}},
                                              }
                };
            }
            return value;
        };
        auto call = test.submit(*fixture.typed);
        REQUIRE(call);
        static_cast<void>(fixture.device.take_written_data());
        fixture.device.inject_input(success(*call + 1U, wrap(raw)));
        fixture.drain_tasks();

        for (auto const* representation :
             {"missing", "null", "0", "-1", "9223372036854775808", "true", "\"1\"", "1.0"}) {
            CAPTURE(representation, test.list, test.mutation);
            auto  malformed = raw;
            auto& object    = std::get<JsonValue::Object>(malformed.data);
            if (std::string_view{representation} == "missing") {
                object.erase("revision");
            }
            else {
                auto revision = parse_json(representation);
                REQUIRE(revision);
                object.at("revision") = *revision;
            }
            auto rejected = test.submit(*fixture.typed);
            REQUIRE(rejected);
            static_cast<void>(fixture.device.take_written_data());
            auto const before = failures.size();
            fixture.device.inject_input(success(*rejected + 1U, wrap(std::move(malformed))));
            fixture.drain_tasks();
            REQUIRE(failures.size() == before + 1U);
            CHECK(failures.back().kind == ControlFailureKind::Local);
            CHECK(std::get<Error>(failures.back().error).code == "jobu.client.invalid_response");
            CHECK(failures.back().outcome_unknown == test.mutation);
        }
    }
    CHECK(received == cases.size());
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

TEST_CASE("Both typed statistics methods preserve measured coverage and reject malformed replies",
          "[jobu][client][statistics]")
{
    Fixture fixture;
    fixture.initialize();
    auto page = StatisticsPage{
        .window      = {.from = UtcTimePoint{}, .to = UtcTimePoint{} + std::chrono::hours{1}},
        .groups      = {StatisticsGroup{
            .runs                   = {.total = 4, .scheduled = 1, .succeeded = 3},
            .runnable_wait_ms       = StatisticsDuration{.samples = 1, .average = 1.5, .maximum = 1.5},
            .runnable_wait_coverage = {.complete = 1, .partial = 1, .unmeasured = 1, .unfinished = 1},
        }},
        .measurement = {.runnable_wait = "monotonic_observed"},
    };
    auto raw      = encoded(statistics_page_to_json(page));
    auto received = std::size_t{0};
    auto failed   = std::size_t{0};
    fixture.typed->reply_received.connect(fixture.typed.get(), [&](ControlCallId, ControlReply const& reply) {
        auto const* result = std::get_if<StatisticsPage>(&reply);
        REQUIRE(result != nullptr);
        CHECK(result->measurement.runnable_wait == "monotonic_observed");
        CHECK(encoded(statistics_page_to_json(*result)) == raw);
        ++received;
    });
    fixture.typed->call_failed.connect(fixture.typed.get(), [&](ControlCallId, ControlFailure const& failure) {
        CHECK(failure.kind == ControlFailureKind::Local);
        CHECK(std::get<Error>(failure.error).code == "jobu.client.invalid_response");
        CHECK_FALSE(failure.outcome_unknown);
        ++failed;
    });

    for (auto queue_scope : {false, true}) {
        auto submit = [&] {
            return queue_scope ? fixture.typed->queue_statistics(QueueStatisticsQuery{.selector = std::string{"queue"}})
                               : fixture.typed->system_statistics(StatisticsRequest{});
        };
        auto call = submit();
        REQUIRE(call);
        static_cast<void>(fixture.device.take_written_data());
        fixture.device.inject_input(success(*call + 1U, raw));
        fixture.drain_tasks();

        auto  malformed = raw;
        auto& group     = std::get<JsonValue::Object>(
            std::get<JsonValue::Array>(std::get<JsonValue::Object>(malformed.data).at("groups").data).front().data);
        group.erase("runnable_wait_coverage");
        auto rejected = submit();
        REQUIRE(rejected);
        static_cast<void>(fixture.device.take_written_data());
        fixture.device.inject_input(success(*rejected + 1U, malformed));
        fixture.drain_tasks();
    }
    CHECK(received == 2);
    CHECK(failed == 2);
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

TEST_CASE("Typed cron requests preserve omitted and explicit UTC timezones", "[jobu][client][timezone]")
{
    Fixture fixture;
    fixture.initialize();
    auto const after = parse_utc_timestamp("2030-01-01T00:00:00Z").value();
    auto const queue = Uuid::parse("00112233-4455-6677-8899-aabbccddeeff").value();
    for (auto timezone : {std::optional<std::string>{}, std::optional<std::string>{"UTC"}}) {
        auto const schedule = CronScheduleInput{.expression = "@daily", .timezone = timezone};
        auto       submit   = std::vector<Submit>{
            [&](ControlClient& client) { return client.validate_schedule(schedule); },
            [&](ControlClient& client) {
                return client.next_schedule_occurrences({.schedule = schedule, .after = after});
            },
            [&](ControlClient& client) {
                return client.create_job(
                    {.queue = queue, .schedule = schedule, .payload = JsonValue{.data = JsonValue::Object{}}});
            },
            [&](ControlClient& client) {
                return client.update_job({.job_id = queue, .expected_revision = 1, .schedule = schedule});
            },
        };
        for (auto const& call : submit) {
            auto submitted = call(*fixture.typed);
            REQUIRE(submitted);
            auto        request = request_body(fixture.device.take_written_data());
            auto const& fields  = request.as_object().at("params").as_object().at("schedule").as_object();
            CHECK(fields.contains("timezone") == timezone.has_value());
            if (timezone) {
                CHECK(fields.at("timezone").as_string() == *timezone);
            }
            fixture.typed->cancel_call(*submitted);
        }
    }
}

TEST_CASE("Typed queue updates validate revisions locally and preserve conflict tokens",
          "[jobu][client][queue][revision]")
{
    Fixture fixture;
    auto    request     = UpdateQueueRequest{.queue = std::string{"protected"}, .weight = 2};
    auto    unavailable = fixture.typed->update_queue(request);
    REQUIRE_FALSE(unavailable);
    CHECK(unavailable.error().code == "jobu.client.not_ready");
    fixture.initialize();
    for (auto revision : {QueueRevision{0}, QueueRevision{9223372036854775808ULL}}) {
        request.expected_revision = revision;
        auto rejected             = fixture.typed->update_queue(request);
        REQUIRE_FALSE(rejected);
        CHECK(rejected.error().code == "jobu.protocol.invalid_request");
        CHECK(fixture.device.take_written_data().empty());
    }

    auto failures = std::vector<ControlFailure>{};
    fixture.typed->call_failed.connect(fixture.typed.get(), [&](ControlCallId, ControlFailure const& failure) {
        failures.push_back(failure);
    });
    request.expected_revision = 9007199254740993ULL;
    auto call                 = fixture.typed->update_queue(request);
    REQUIRE(call);
    auto wire = request_body(fixture.device.take_written_data());
    CHECK(wire.as_object().at("params").as_object().at("expected_revision").as_uint() == request.expected_revision);
    auto error_data = parse_json(R"({"category":"conflict","code":"jobu.queue.revision_conflict"})");
    REQUIRE(error_data);
    fixture.device.inject_input(frame(
        detail::encode_error_response(RequestId{wire.as_object().at("id").as_uint()},
                                      RpcError{.code = -32000, .message = "Stale revision", .data = *error_data})));
    fixture.drain_tasks();
    REQUIRE(failures.size() == 1);
    CHECK(failures.front().kind == ControlFailureKind::Remote);
    auto const& error = std::get<RpcError>(failures.front().error);
    REQUIRE(error.data);
    CHECK(error.data->as_object().at("code").as_string() == "jobu.queue.revision_conflict");
    CHECK_FALSE(failures.front().outcome_unknown);
    CHECK(request.expected_revision == 9007199254740993ULL);
    CHECK(fixture.device.take_written_data().empty());
    fixture.typed->close();
    request.expected_revision = 0;
    auto closed               = fixture.typed->update_queue(request);
    REQUIRE_FALSE(closed);
    CHECK(closed.error().code == "jobu.client.not_ready");
}

TEST_CASE("Typed queue get preserves visibility convenience calls and call options", "[jobu][client][queue][get]")
{
    Fixture fixture;
    fixture.initialize();
    auto const id       = Uuid::parse("00112233-4455-6677-8899-aabbccddeeff").value();
    auto const selector = QueueSelector{id};
    auto const submits  = std::vector<Submit>{
        [&](auto& client) { return client.get_queue(id); },
        [&](auto& client) { return client.get_queue(selector, ControlCallOptions{}); },
        [](auto& client) { return client.get_queue(std::string{"archived"}); },
        [&](auto& client) { return client.get_queue(GetQueueRequest{.queue = id}); },
        [&](auto& client) { return client.get_queue(GetQueueRequest{.queue = id, .include_deleted = true}); },
    };
    auto received = std::optional<Queue>{};
    auto failure  = std::optional<ControlFailure>{};
    fixture.typed->reply_received.connect(fixture.typed.get(), [&](ControlCallId, ControlReply const& reply) {
        received = std::get<Queue>(reply);
    });
    fixture.typed->call_failed.connect(fixture.typed.get(),
                                       [&](ControlCallId, ControlFailure const& value) { failure = value; });

    for (std::size_t index = 0; index < submits.size(); ++index) {
        CAPTURE(index);
        auto call = submits[index](*fixture.typed);
        REQUIRE(call);
        auto wire = request_body(fixture.device.take_written_data());
        CHECK(wire.as_object().at("method").as_string() == "queue.get");
        auto const& params = wire.as_object().at("params").as_object();
        CHECK(params.size() == 2);
        CHECK(params.at("include_deleted").as_bool() == (index == 4));
        if (index == 2) {
            CHECK(params.at("queue_name").as_string() == "archived");
        }
        else {
            CHECK(params.at("queue_id").as_string() == id.to_string());
        }

        if (index == 4) {
            auto tombstone = Queue{.id         = id,
                                   .revision   = 4,
                                   .name       = "archived",
                                   .state      = QueueState::Deleted,
                                   .deleted_at = UtcTimePoint{std::chrono::seconds{1}}};
            fixture.device.inject_input(
                success(wire.as_object().at("id").as_uint(), encoded(queue_to_json(tombstone, fixture.attributes))));
            fixture.drain_tasks();
            REQUIRE(received);
            CHECK(received->id == id);
            CHECK(received->revision == 4);
            CHECK(received->state == QueueState::Deleted);
        }
        else {
            fixture.typed->cancel_call(*call);
            REQUIRE(failure);
            CHECK_FALSE(failure->outcome_unknown);
            failure.reset();
        }
    }

    // Both overloads must forward options rather than silently substituting their default timeout.
    auto const invalid_options = ControlCallOptions{.timeout = std::chrono::milliseconds{0}};
    auto invalid = fixture.typed->get_queue(GetQueueRequest{.queue = id, .include_deleted = true}, invalid_options);
    REQUIRE_FALSE(invalid);
    CHECK(invalid.error().code == "jobu.client.invalid_timeout");
    invalid = fixture.typed->get_queue(selector, invalid_options);
    REQUIRE_FALSE(invalid);
    CHECK(invalid.error().code == "jobu.client.invalid_timeout");
    CHECK(fixture.device.take_written_data().empty());
}

TEST_CASE("Typed queue get validates before peer admission and after readiness", "[jobu][client][queue][get]")
{
    Fixture    fixture;
    auto const invalid_request = GetQueueRequest{
        .queue           = std::string{"\xc3\x28", 2U},
        .include_deleted = true
    };
    auto rejected = fixture.typed->get_queue(invalid_request);
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().code == "jobu.client.not_ready");
    fixture.initialize(5);
    rejected = fixture.typed->get_queue(invalid_request);
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().code == "jobu.protocol.invalid_request");
    CHECK(fixture.device.take_written_data().empty());
    fixture.typed->close();
    rejected = fixture.typed->get_queue(invalid_request);
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().code == "jobu.client.not_ready");
}
