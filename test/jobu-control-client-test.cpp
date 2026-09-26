#include "application.hpp"
#include "attribute_registry.hpp"
#include "client.hpp"
#include "control_client.hpp"
#include "framing.hpp"
#include "history_json.hpp"
#include "management_json.hpp"
#include "protocol_priv.hpp"
#include "support/catch_utils.hpp" // IWYU pragma: keep for Catch::StringMaker specializations
#include "support/memory_io_device.hpp"
#include "system_info.hpp"
#include "timer.hpp"
#include "utc_timestamp.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <thread>
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
    auto framed = frame_message(encoded.value());
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

auto info(std::vector<std::string> methods = {"job.create", "run.list"}) -> JsonValue
{
    return system_info_to_json(SystemInfo{
        .daemon_version = "test",
        .api_version    = {.major = 1, .minor = 3},
        .capabilities   = std::move(methods),
    });
}

auto empty_run_page() -> JsonValue
{
    auto encoded = run_page_to_json(RunPage{});
    REQUIRE(encoded);
    return std::move(encoded).value();
}

auto creation_request() -> CreateJobRequest
{
    JsonValue payload;
    payload.data = JsonValue::Object{};
    return CreateJobRequest{
        .queue           = std::string{"default"},
        .schedule        = ImmediateSchedule{},
        .payload         = std::move(payload),
        .idempotency_key = std::string{"creation-key"},
    };
}

auto created_job(StandardAttributeRegistry const& registry) -> JsonValue
{
    auto id         = Uuid::parse("00112233-4455-6677-8899-aabbccddeeff");
    auto queue_id   = Uuid::parse("10112233-4455-6677-8899-aabbccddeeff");
    auto at         = parse_utc_timestamp("2026-07-21T21:00:00Z");
    auto attributes = materialize_attributes(registry, {}, {}, {});
    REQUIRE(id);
    REQUIRE(queue_id);
    REQUIRE(at);
    REQUIRE(attributes);

    JsonValue payload;
    payload.data = JsonValue::Object{};
    auto job     = JobDefinition{
        .id         = id.value(),
        .queue_id   = queue_id.value(),
        .schedule   = OnceSchedule{.planned_at = at.value()},
        .attributes = std::move(attributes).value(),
        .payload    = std::move(payload),
        .created_at = at.value(),
        .updated_at = at.value(),
    };
    auto encoded = job_to_json(job, registry);
    REQUIRE(encoded);
    return std::move(encoded).value();
}

struct Fixture {
    Application                    app{0, nullptr};
    StandardAttributeRegistry      attributes;
    MemoryIODevice                 device;
    std::unique_ptr<Client>        rpc;
    std::unique_ptr<ControlClient> typed;

    explicit Fixture(ClientOptions options = {})
    {
        device.open();
        rpc   = std::make_unique<Client>(device, options);
        typed = std::make_unique<ControlClient>(*rpc, attributes);
    }

    void drain_tasks() { REQUIRE(app.process_events(EventFlag::Tasks) != ProcessEventsResult::Failed); }

    void fire_one_millisecond_timeouts()
    {
        // Keep timer delivery queued until every one-millisecond call submitted so far has passed its deadline.
        std::this_thread::sleep_until(Clock::now() + std::chrono::milliseconds{2});
        REQUIRE(app.process_events(EventFlag::Timers) != ProcessEventsResult::Failed);
    }

    void initialize(std::vector<std::string> methods = {"job.create", "run.list"})
    {
        REQUIRE(typed->initialize());
        device.inject_input(success(1U, info(std::move(methods))));
        drain_tasks();
        static_cast<void>(device.take_written_data());
    }
};

} // anonymous namespace

TEST_CASE("Typed history calls reject oversized attempt numbers before writing", "[jobu][client]")
{
    Fixture fixture;
    fixture.initialize({"attempt.get", "attempt.output"});
    auto const run = Uuid::parse("10112233-4455-6677-8899-aabbccddeeff");
    REQUIRE(run);

    for (auto number : {maximum_attempt_number + 1, std::numeric_limits<AttemptNumber>::max()}) {
        auto key = AttemptKey{.run_id = *run, .attempt_number = number};
        auto get = fixture.typed->get_attempt(key);
        REQUIRE_FALSE(get);
        CHECK(get.error().code == "jobu.protocol.invalid_request");

        auto output = fixture.typed->read_attempt_output(AttemptOutputRequest{.attempt = key});
        REQUIRE_FALSE(output);
        CHECK(output.error().code == "jobu.protocol.invalid_request");
        CHECK(fixture.device.take_written_data().empty());
    }
}

TEST_CASE("Synchronous handshake and replies are delivered after accepting calls return", "[jobu][client]")
{
    Fixture fixture;
    auto    events = std::vector<std::string>{};
    fixture.typed->ready.connect([&](SystemInfo const&) { events.emplace_back("ready"); });
    fixture.typed->reply_received.connect([&](ControlCallId, ControlReply const& reply) {
        CHECK(std::holds_alternative<RunPage>(reply));
        events.emplace_back("reply");
    });

    auto inject_handshake =
        fixture.device.bytes_written.connect([&](std::size_t) { fixture.device.inject_input(success(1U, info())); });
    REQUIRE(fixture.typed->initialize());
    CHECK(events.empty());
    inject_handshake.disconnect();
    fixture.drain_tasks();
    CHECK(events == std::vector<std::string>{"ready"});

    auto inject_reply = fixture.device.bytes_written.connect(
        [&](std::size_t) { fixture.device.inject_input(success(2U, empty_run_page())); });
    auto call = fixture.typed->list_runs(RunQuery{});
    REQUIRE(call);
    CHECK(call.value() == 1U);
    CHECK(events == std::vector<std::string>{"ready"});
    inject_reply.disconnect();
    fixture.drain_tasks();
    CHECK(events == std::vector<std::string>{"ready", "reply"});
}

TEST_CASE("A ready handler may destroy its wrapper or owning parent", "[jobu][client]")
{
    {
        Fixture fixture;
        auto    ready_count = 0;
        fixture.typed->ready.connect([&](SystemInfo const& result) {
            fixture.typed.reset();
            CHECK(result.api_version.major == 1U);
            ++ready_count;
        });

        REQUIRE(fixture.typed->initialize());
        fixture.device.inject_input(success(1U, info()));
        fixture.drain_tasks();
        CHECK(ready_count == 1);

        auto raw_call = fixture.rpc->call("after-ready-destruction");
        REQUIRE(raw_call);
        auto raw_replies = 0;
        fixture.rpc->result_received.connect([&](RequestId const& id, JsonValue const&) {
            if (id == raw_call.value()) {
                ++raw_replies;
            }
        });
        fixture.device.inject_input(success(2U, JsonValue{}));
        CHECK(raw_replies == 1);
    }

    Fixture fixture;
    fixture.typed.reset();
    auto  parent      = std::make_unique<Object>();
    auto* typed       = new ControlClient(*fixture.rpc, fixture.attributes, parent.get());
    auto  ready_count = 0;
    typed->ready.connect([&](SystemInfo const&) {
        parent.reset();
        ++ready_count;
    });

    REQUIRE(typed->initialize());
    fixture.device.inject_input(success(1U, info()));
    fixture.drain_tasks();
    CHECK(ready_count == 1);
    CHECK(parent == nullptr);
}

TEST_CASE("Destruction from a reply suppresses later queued outcomes", "[jobu][client]")
{
    for (auto count : {1U, 2U}) {
        Fixture fixture;
        fixture.initialize();
        auto replies = 0;
        fixture.typed->reply_received.connect([&](ControlCallId, ControlReply const& reply) {
            fixture.typed.reset();
            CHECK(std::holds_alternative<RunPage>(reply));
            ++replies;
        });

        for (auto index = 0U; index < count; ++index) {
            REQUIRE(fixture.typed->list_runs(RunQuery{}));
            fixture.device.inject_input(success(2U + index, empty_run_page()));
        }

        fixture.drain_tasks();
        CHECK(replies == 1);
        CHECK(fixture.typed == nullptr);
    }
}

TEST_CASE("Destruction from deferred handshake and decoded-result failures is safe", "[jobu][client]")
{
    {
        Fixture fixture;
        auto    failures = 0;
        fixture.typed->failed.connect([&](Error const& error) {
            fixture.typed.reset();
            CHECK(error.code == "jobu.client.unsupported_api");
            ++failures;
        });

        REQUIRE(fixture.typed->initialize());
        auto incompatible = SystemInfo{
            .daemon_version = "other",
            .api_version    = {.major = 2, .minor = 0},
        };
        fixture.device.inject_input(success(1U, system_info_to_json(incompatible)));
        fixture.drain_tasks();
        CHECK(failures == 1);
    }

    Fixture fixture;
    fixture.initialize();
    auto failures = 0;
    fixture.typed->call_failed.connect([&](ControlCallId id, ControlFailure const& failure) {
        fixture.typed.reset();
        CHECK(id == 1U);
        CHECK(std::get<Error>(failure.error).code == "jobu.client.invalid_response");
        ++failures;
    });

    REQUIRE(fixture.typed->list_runs(RunQuery{}));
    fixture.device.inject_input(success(2U, info()));
    fixture.drain_tasks();
    CHECK(failures == 1);
}

TEST_CASE("Reply handlers may close or cancel a second queued call once", "[jobu][client]")
{
    for (auto close_wrapper : {false, true}) {
        Fixture fixture;
        fixture.initialize();
        auto first  = fixture.typed->list_runs(RunQuery{});
        auto second = fixture.typed->list_runs(RunQuery{});
        REQUIRE(first);
        REQUIRE(second);

        auto replies  = std::vector<ControlCallId>{};
        auto failures = std::vector<ControlCallId>{};
        fixture.typed->reply_received.connect([&](ControlCallId id, ControlReply const&) {
            replies.push_back(id);
            if (close_wrapper) {
                fixture.typed->close();
            }
            else {
                fixture.typed->cancel_call(second.value());
            }
        });
        fixture.typed->call_failed.connect([&](ControlCallId id, ControlFailure const& failure) {
            CHECK(std::get<Error>(failure.error).code ==
                  (close_wrapper ? "jobu.client.closed" : "jobu.client.cancelled"));
            failures.push_back(id);
        });

        fixture.device.inject_input(success(2U, empty_run_page()));
        fixture.device.inject_input(success(3U, empty_run_page()));
        fixture.drain_tasks();
        fixture.drain_tasks();
        CHECK(replies == std::vector<ControlCallId>{first.value()});
        CHECK(failures == std::vector<ControlCallId>{second.value()});
    }
}

TEST_CASE("An earlier raw reply cannot hide a synchronous typed reply", "[jobu][client]")
{
    Fixture fixture;
    fixture.initialize();

    auto custom = fixture.rpc->call("custom");
    REQUIRE(custom);
    auto custom_results = 0;
    fixture.rpc->result_received.connect([&](RequestId const& id, JsonValue const&) {
        if (id == custom.value()) {
            ++custom_results;
        }
    });
    auto replies = std::vector<ControlCallId>{};
    fixture.typed->reply_received.connect([&](ControlCallId id, ControlReply const& reply) {
        CHECK(std::holds_alternative<RunPage>(reply));
        replies.push_back(id);
    });

    auto inject_both = fixture.device.bytes_written.connect([&](std::size_t) {
        fixture.device.inject_input(success(2U, JsonValue{}));
        fixture.device.inject_input(success(3U, empty_run_page()));
    });
    auto typed_call  = fixture.typed->list_runs(RunQuery{});
    REQUIRE(typed_call);
    inject_both.disconnect();

    CHECK(custom_results == 1);
    CHECK(replies.empty());
    fixture.drain_tasks();
    CHECK(replies == std::vector<ControlCallId>{typed_call.value()});
}

TEST_CASE("Reentrant custom replies cannot exhaust typed correlation", "[jobu][client]")
{
    auto options                 = ClientOptions{};
    options.max_pending_requests = 2U;
    Fixture fixture{options};
    fixture.initialize();

    auto first_custom = fixture.rpc->call("custom");
    REQUIRE(first_custom);
    CHECK(first_custom.value() == RequestId{std::uint64_t{2}});

    auto custom_results = 0U;
    fixture.rpc->result_received.connect([&](RequestId const& id, JsonValue const&) {
        if (id == RequestId{std::uint64_t{3}}) {
            return;
        }

        ++custom_results;
        if (custom_results < 3U) {
            auto next = fixture.rpc->call("custom");
            REQUIRE(next);
            auto const next_id = std::uint64_t{3} + custom_results;
            CHECK(next.value() == RequestId{next_id});
            fixture.device.inject_input(success(next_id, JsonValue{}));
        }
        else {
            fixture.device.inject_input(success(3U, empty_run_page()));
        }
    });

    auto failures = std::vector<std::string>{};
    fixture.typed->failed.connect([&](Error const& error) { failures.push_back(error.code); });
    auto replies = std::vector<ControlCallId>{};
    fixture.typed->reply_received.connect([&](ControlCallId id, ControlReply const&) { replies.push_back(id); });

    auto inject_first = true;
    auto inject_reply = fixture.device.bytes_written.connect([&](std::size_t) {
        if (inject_first) {
            inject_first = false;
            fixture.device.inject_input(success(2U, JsonValue{}));
        }
    });
    auto typed_call   = fixture.typed->list_runs(RunQuery{});
    REQUIRE(typed_call);
    inject_reply.disconnect();

    CHECK(custom_results == 3U);
    CHECK(replies.empty());
    fixture.drain_tasks();
    CHECK(failures.empty());
    CHECK(replies == std::vector<ControlCallId>{typed_call.value()});
}

TEST_CASE("Closing an initializing client reports the unfinished handshake once", "[jobu][client]")
{
    Fixture fixture;
    auto    failures = std::vector<std::string>{};
    fixture.typed->failed.connect([&](Error const& error) { failures.push_back(error.code); });

    REQUIRE(fixture.typed->initialize());
    fixture.typed->close();
    fixture.typed->close();
    CHECK(failures == std::vector<std::string>{"jobu.client.closed"});
    CHECK(fixture.rpc->pending_request_count() == 0U);
}

TEST_CASE("Close during handshake write defers its failure until initialize returns", "[jobu][client]")
{
    Fixture fixture;
    auto    failures = std::vector<std::string>{};
    fixture.typed->failed.connect([&](Error const& error) { failures.push_back(error.code); });
    auto close_on_write = fixture.device.bytes_written.connect([&](std::size_t) { fixture.typed->close(); });

    REQUIRE(fixture.typed->initialize());
    CHECK(failures.empty());
    CHECK(fixture.rpc->pending_request_count() == 0U);
    close_on_write.disconnect();
    fixture.drain_tasks();
    CHECK(failures == std::vector<std::string>{"jobu.client.closed"});
}

TEST_CASE("Out-of-order replies and remote errors retain local call identity", "[jobu][client]")
{
    Fixture fixture;
    fixture.initialize();
    auto events = std::vector<std::string>{};
    fixture.typed->reply_received.connect(
        [&](ControlCallId id, ControlReply const&) { events.push_back("reply:" + std::to_string(id)); });
    fixture.typed->call_failed.connect([&](ControlCallId id, ControlFailure const& failure) {
        CHECK(failure.kind == ControlFailureKind::Remote);
        CHECK_FALSE(failure.outcome_unknown);
        events.push_back("remote:" + std::to_string(id));
    });

    REQUIRE(fixture.typed->list_runs(RunQuery{}));
    REQUIRE(fixture.typed->list_runs(RunQuery{}));
    fixture.device.inject_input(success(3U, empty_run_page()));
    fixture.device.inject_input(remote_error(2U));
    fixture.drain_tasks();
    CHECK(events == std::vector<std::string>{"reply:2", "remote:1"});
}

TEST_CASE("Synchronous remote errors and cancellation of queued replies complete once", "[jobu][client]")
{
    Fixture fixture;
    fixture.initialize();
    auto failures = std::vector<ControlFailure>{};
    auto replies  = 0;
    fixture.typed->call_failed.connect(
        [&](ControlCallId, ControlFailure const& failure) { failures.push_back(failure); });
    fixture.typed->reply_received.connect([&](ControlCallId, ControlReply const&) { ++replies; });

    auto inject_error =
        fixture.device.bytes_written.connect([&](std::size_t) { fixture.device.inject_input(remote_error(2U)); });
    REQUIRE(fixture.typed->list_runs(RunQuery{}));
    CHECK(failures.empty());
    inject_error.disconnect();
    fixture.drain_tasks();
    REQUIRE(failures.size() == 1U);
    CHECK(failures.front().kind == ControlFailureKind::Remote);

    auto second = fixture.typed->list_runs(RunQuery{});
    REQUIRE(second);
    fixture.device.inject_input(success(3U, empty_run_page()));
    fixture.typed->cancel_call(second.value());
    fixture.drain_tasks();
    REQUIRE(failures.size() == 2U);
    CHECK(failures.back().kind == ControlFailureKind::Local);
    CHECK(replies == 0);
}

TEST_CASE("Create replies decode through the shared JobU codec", "[jobu][client]")
{
    Fixture fixture;
    fixture.initialize();
    auto jobs = 0;
    fixture.typed->reply_received.connect([&](ControlCallId id, ControlReply const& reply) {
        CHECK(id == 1U);
        CHECK(std::holds_alternative<JobDefinition>(reply));
        ++jobs;
    });

    auto call = fixture.typed->create_job(creation_request());
    REQUIRE(call);
    fixture.device.inject_input(success(2U, created_job(fixture.attributes)));
    fixture.drain_tasks();
    CHECK(jobs == 1);
}

TEST_CASE("Malformed typed result fails only its call", "[jobu][client]")
{
    Fixture fixture;
    fixture.initialize();
    auto failures = std::vector<std::string>{};
    fixture.typed->call_failed.connect([&](ControlCallId id, ControlFailure const& failure) {
        CHECK(id == 1U);
        CHECK(failure.kind == ControlFailureKind::Local);
        failures.push_back(std::get<Error>(failure.error).code);
    });

    REQUIRE(fixture.typed->list_runs(RunQuery{}));
    fixture.device.inject_input(success(2U, info()));
    fixture.drain_tasks();
    CHECK(failures == std::vector<std::string>{"jobu.client.invalid_response"});
    CHECK(fixture.rpc->pending_request_count() == 0U);
    CHECK(fixture.typed->list_runs(RunQuery{}));
}

TEST_CASE("The typed pending ceiling rejects a 129th call before writing", "[jobu][client]")
{
    Fixture fixture;
    fixture.initialize();
    for (auto count = 0U; count < 128U; ++count) {
        REQUIRE(fixture.typed->list_runs(RunQuery{}));
    }
    auto const before = fixture.device.written_data();
    auto       over   = fixture.typed->list_runs(RunQuery{});
    REQUIRE_FALSE(over);
    CHECK(over.error().code == "jobu.client.pending_limit");
    CHECK(fixture.device.written_data() == before);
}

TEST_CASE("Capability and smaller raw pending limits reject without writing", "[jobu][client]")
{
    auto options                 = ClientOptions{};
    options.max_pending_requests = 1U;
    Fixture fixture{options};
    fixture.initialize({"run.list"});

    auto unsupported = fixture.typed->create_job(creation_request());
    REQUIRE_FALSE(unsupported);
    CHECK(unsupported.error().code == "jobu.client.unsupported_method");
    CHECK(fixture.device.written_data().empty());

    REQUIRE(fixture.typed->list_runs(RunQuery{}));
    auto const before  = fixture.device.written_data();
    auto       limited = fixture.typed->list_runs(RunQuery{});
    REQUIRE_FALSE(limited);
    CHECK(limited.error().code == "jobu.client.pending_limit");
    CHECK(fixture.device.written_data() == before);
}

TEST_CASE("A synchronous raw completion still occupies a typed pending slot", "[jobu][client]")
{
    auto options                 = ClientOptions{};
    options.max_pending_requests = 1U;
    Fixture fixture{options};
    fixture.initialize();

    auto inject_reply = fixture.device.bytes_written.connect(
        [&](std::size_t) { fixture.device.inject_input(success(2U, empty_run_page())); });
    auto first = fixture.typed->list_runs(RunQuery{});
    REQUIRE(first);
    inject_reply.disconnect();
    CHECK(fixture.rpc->pending_request_count() == 0U);

    auto const before = fixture.device.written_data();
    auto       second = fixture.typed->list_runs(RunQuery{});
    REQUIRE_FALSE(second);
    CHECK(second.error().code == "jobu.client.pending_limit");
    CHECK(fixture.device.written_data() == before);

    fixture.drain_tasks();
    REQUIRE(fixture.typed->list_runs(RunQuery{}));
}

TEST_CASE("A partial mutation write fails once with an unknown outcome and terminal ordering", "[jobu][client]")
{
    Fixture fixture;
    fixture.initialize();
    auto events = std::vector<std::string>{};
    fixture.typed->failed.connect([&](Error const& error) {
        CHECK(error.code == "rpc.short_write");
        events.emplace_back("failed");
        fixture.typed->close();
    });
    fixture.typed->call_failed.connect([&](ControlCallId id, ControlFailure const& failure) {
        CHECK(failure.kind == ControlFailureKind::Local);
        events.push_back(std::to_string(id) + (failure.outcome_unknown ? ":unknown" : ":known"));
    });

    REQUIRE(fixture.typed->list_runs(RunQuery{}));
    fixture.device.set_write_limit(3U);
    auto creation = fixture.typed->create_job(creation_request());
    REQUIRE(creation);
    CHECK(creation.value() == 2U);
    CHECK(events.empty());
    fixture.drain_tasks();
    CHECK(events == std::vector<std::string>{"failed", "1:known", "2:unknown"});
}

TEST_CASE("Terminal failure delivery stops when a handler destroys the wrapper", "[jobu][client]")
{
    for (auto destroy_on_failed : {true, false}) {
        Fixture fixture;
        fixture.initialize();
        auto failed_count  = 0;
        auto call_failures = std::vector<ControlCallId>{};

        fixture.typed->failed.connect([&](Error const& error) {
            ++failed_count;
            if (destroy_on_failed) {
                fixture.typed.reset();
            }
            CHECK(error.code == "rpc.connection_closed");
        });
        fixture.typed->call_failed.connect([&](ControlCallId id, ControlFailure const& failure) {
            call_failures.push_back(id);
            fixture.typed.reset();
            CHECK(std::get<Error>(failure.error).code == "rpc.connection_closed");
        });

        REQUIRE(fixture.typed->list_runs(RunQuery{}));
        REQUIRE(fixture.typed->list_runs(RunQuery{}));
        fixture.rpc->close();
        fixture.drain_tasks();

        CHECK(failed_count == 1);
        CHECK(call_failures == (destroy_on_failed ? std::vector<ControlCallId>{} : std::vector<ControlCallId>{1U}));
        CHECK(fixture.typed == nullptr);
    }
}

TEST_CASE("A timeout batch keeps its latched failures after reentrant close", "[jobu][client]")
{
    Fixture fixture;
    fixture.initialize();
    auto first  = fixture.typed->list_runs(RunQuery{}, {.timeout = std::chrono::milliseconds{1}});
    auto second = fixture.typed->list_runs(RunQuery{}, {.timeout = std::chrono::milliseconds{1}});
    auto third  = fixture.typed->list_runs(RunQuery{}, {.timeout = std::chrono::seconds{1}});
    REQUIRE(first);
    REQUIRE(second);
    REQUIRE(third);

    auto outcomes = std::vector<std::pair<ControlCallId, std::string>>{};
    fixture.typed->call_failed.connect([&](ControlCallId id, ControlFailure const& failure) {
        outcomes.emplace_back(id, std::get<Error>(failure.error).code);
        if (id == first.value()) {
            fixture.typed->close();
        }
    });

    fixture.fire_one_millisecond_timeouts();
    CHECK(outcomes == std::vector<std::pair<ControlCallId, std::string>>{
                          {first.value(),  "jobu.client.timeout"},
                          {third.value(),  "jobu.client.closed" },
                          {second.value(), "jobu.client.timeout"},
    });
}

TEST_CASE("Destroying the wrapper from a timeout suppresses the rest of the batch", "[jobu][client]")
{
    Fixture fixture;
    fixture.initialize();
    auto first  = fixture.typed->list_runs(RunQuery{}, {.timeout = std::chrono::milliseconds{1}});
    auto second = fixture.typed->list_runs(RunQuery{}, {.timeout = std::chrono::milliseconds{1}});
    REQUIRE(first);
    REQUIRE(second);

    auto failures = std::vector<ControlCallId>{};
    fixture.typed->call_failed.connect([&](ControlCallId id, ControlFailure const& failure) {
        failures.push_back(id);
        fixture.typed.reset();
        CHECK(std::get<Error>(failure.error).code == "jobu.client.timeout");
    });

    fixture.fire_one_millisecond_timeouts();
    CHECK(failures == std::vector<ControlCallId>{first.value()});

    auto raw_call = fixture.rpc->call("after-timeout-destruction");
    REQUIRE(raw_call);
    auto raw_replies = 0;
    fixture.rpc->result_received.connect([&](RequestId const& id, JsonValue const&) {
        if (id == raw_call.value()) {
            ++raw_replies;
        }
    });
    fixture.device.inject_input(success(4U, JsonValue{}));
    CHECK(raw_replies == 1);
}

TEST_CASE("Explicit close failure delivery respects destruction and reentrancy", "[jobu][client]")
{
    {
        Fixture fixture;
        REQUIRE(fixture.typed->initialize());
        auto failures = 0;
        fixture.typed->failed.connect([&](Error const& error) {
            fixture.typed.reset();
            CHECK(error.code == "jobu.client.closed");
            ++failures;
        });

        fixture.typed->close();
        CHECK(failures == 1);
    }

    {
        Fixture fixture;
        fixture.initialize();
        auto failures = std::vector<ControlCallId>{};
        fixture.typed->call_failed.connect([&](ControlCallId id, ControlFailure const& failure) {
            failures.push_back(id);
            fixture.typed.reset();
            CHECK(std::get<Error>(failure.error).code == "jobu.client.closed");
        });

        REQUIRE(fixture.typed->list_runs(RunQuery{}));
        REQUIRE(fixture.typed->list_runs(RunQuery{}));
        fixture.typed->close();
        CHECK(failures == std::vector<ControlCallId>{1U});
    }

    Fixture fixture;
    fixture.initialize();
    auto failures = std::vector<ControlCallId>{};
    fixture.typed->call_failed.connect([&](ControlCallId id, ControlFailure const& failure) {
        CHECK(std::get<Error>(failure.error).code == "jobu.client.closed");
        failures.push_back(id);
        fixture.typed->close();
    });

    REQUIRE(fixture.typed->list_runs(RunQuery{}));
    REQUIRE(fixture.typed->list_runs(RunQuery{}));
    fixture.typed->close();
    fixture.typed->close();
    CHECK(failures == std::vector<ControlCallId>{1U, 2U});
}

TEST_CASE("Cancellation and close retire correlations exactly once", "[jobu][client]")
{
    Fixture fixture;
    fixture.initialize();
    auto failures = std::vector<ControlCallId>{};
    fixture.typed->call_failed.connect([&](ControlCallId id, ControlFailure const&) {
        failures.push_back(id);
        fixture.typed->close();
    });
    auto first  = fixture.typed->list_runs(RunQuery{});
    auto second = fixture.typed->list_runs(RunQuery{});
    REQUIRE(first);
    REQUIRE(second);

    fixture.typed->cancel_call(first.value());
    fixture.typed->cancel_call(first.value());
    fixture.device.inject_input(success(3U, empty_run_page()));
    fixture.drain_tasks();
    CHECK(failures == std::vector<ControlCallId>{1U, 2U});
    CHECK(fixture.device.is_open());
}

TEST_CASE("Late cancelled and closed replies leave the borrowed raw client usable", "[jobu][client]")
{
    Fixture fixture;
    fixture.initialize();
    auto protocol_errors = 0;
    fixture.rpc->protocol_error.connect([&](Error const&) { ++protocol_errors; });
    auto replies = std::vector<ControlCallId>{};
    fixture.typed->reply_received.connect([&](ControlCallId id, ControlReply const&) { replies.push_back(id); });

    auto first  = fixture.typed->list_runs(RunQuery{});
    auto second = fixture.typed->list_runs(RunQuery{});
    REQUIRE(first);
    REQUIRE(second);
    fixture.typed->cancel_call(first.value());
    fixture.device.inject_input(success(2U, empty_run_page()));
    fixture.device.inject_input(success(3U, empty_run_page()));
    fixture.drain_tasks();
    CHECK(replies == std::vector<ControlCallId>{second.value()});
    CHECK(protocol_errors == 0);

    REQUIRE(fixture.typed->list_runs(RunQuery{}));
    fixture.typed->close();
    fixture.device.inject_input(success(4U, empty_run_page()));
    CHECK(protocol_errors == 0);

    auto raw_call = fixture.rpc->call("after-close");
    REQUIRE(raw_call);
    auto raw_replies = 0;
    fixture.rpc->result_received.connect([&](RequestId const& id, JsonValue const&) {
        if (id == raw_call.value()) {
            ++raw_replies;
        }
    });
    fixture.device.inject_input(success(5U, JsonValue{}));
    CHECK(raw_replies == 1);
}

TEST_CASE("Close during a raw write defers the current mutation failure", "[jobu][client]")
{
    Fixture fixture;
    fixture.initialize();
    auto failures = std::vector<ControlFailure>{};
    fixture.typed->call_failed.connect(
        [&](ControlCallId, ControlFailure const& failure) { failures.push_back(failure); });
    auto close_on_write = fixture.device.bytes_written.connect([&](std::size_t) { fixture.typed->close(); });

    auto call = fixture.typed->create_job(creation_request());
    REQUIRE(call);
    CHECK(failures.empty());
    CHECK(fixture.rpc->pending_request_count() == 0U);
    close_on_write.disconnect();
    fixture.drain_tasks();
    REQUIRE(failures.size() == 1U);
    CHECK(failures.front().outcome_unknown);
}

TEST_CASE("A failure handler may destroy the wrapper during another call's raw write", "[jobu][client]")
{
    Fixture fixture;
    fixture.initialize();
    auto earlier = fixture.typed->list_runs(RunQuery{});
    REQUIRE(earlier);

    auto failures = std::vector<ControlCallId>{};
    fixture.typed->call_failed.connect([&](ControlCallId id, ControlFailure const& failure) {
        failures.push_back(id);
        fixture.typed.reset();
        CHECK(std::get<Error>(failure.error).code == "jobu.client.closed");
    });
    auto close_on_write = fixture.device.bytes_written.connect([&](std::size_t) { fixture.typed->close(); });

    auto current = fixture.typed->create_job(creation_request());
    REQUIRE(current);
    close_on_write.disconnect();
    fixture.drain_tasks();

    CHECK(failures == std::vector<ControlCallId>{earlier.value()});
    CHECK(fixture.typed == nullptr);
    CHECK(fixture.rpc->pending_request_count() == 0U);

    auto raw_call = fixture.rpc->call("after-write-destruction");
    REQUIRE(raw_call);
    auto raw_replies = 0;
    fixture.rpc->result_received.connect([&](RequestId const& id, JsonValue const&) {
        if (id == raw_call.value()) {
            ++raw_replies;
        }
    });
    fixture.device.inject_input(success(4U, JsonValue{}));
    CHECK(raw_replies == 1);
}

TEST_CASE("A cancellation failure may destroy the wrapper without affecting the raw client", "[jobu][client]")
{
    Fixture fixture;
    fixture.initialize();
    auto call = fixture.typed->list_runs(RunQuery{});
    REQUIRE(call);

    auto failures = 0;
    fixture.typed->call_failed.connect([&](ControlCallId id, ControlFailure const& failure) {
        fixture.typed.reset();
        CHECK(id == call.value());
        CHECK(std::get<Error>(failure.error).code == "jobu.client.cancelled");
        ++failures;
    });
    fixture.typed->cancel_call(call.value());
    CHECK(failures == 1);
    REQUIRE(fixture.rpc->call("after-cancel-destruction"));
}

TEST_CASE("Idle raw closure and wrapper destruction suppress deferred callbacks", "[jobu][client]")
{
    {
        Fixture fixture;
        fixture.initialize();
        auto failures = std::vector<std::string>{};
        fixture.typed->failed.connect([&](Error const& error) { failures.push_back(error.code); });
        fixture.rpc->close();
        CHECK(failures.empty());
        fixture.drain_tasks();
        CHECK(failures == std::vector<std::string>{"rpc.connection_closed"});
    }

    Fixture second;
    second.initialize();
    auto replies = 0;
    second.typed->reply_received.connect([&](ControlCallId, ControlReply const&) { ++replies; });
    REQUIRE(second.typed->list_runs(RunQuery{}));
    second.device.inject_input(success(2U, empty_run_page()));
    second.typed.reset();
    second.drain_tasks();
    CHECK(replies == 0);
}

TEST_CASE("Handshake rejects incompatible major and a timeout retires a late response", "[jobu][client]")
{
    {
        Fixture incompatible;
        auto    errors = std::vector<std::string>{};
        incompatible.typed->failed.connect([&](Error const& error) { errors.push_back(error.code); });
        REQUIRE(incompatible.typed->initialize());
        auto version = SystemInfo{
            .daemon_version = "other",
            .api_version    = {.major = 2, .minor = 0}
        };
        incompatible.device.inject_input(success(1U, system_info_to_json(version)));
        incompatible.drain_tasks();
        CHECK(errors == std::vector<std::string>{"jobu.client.unsupported_api"});
    }

    Fixture fixture;
    fixture.initialize();
    auto timeouts = 0;
    fixture.typed->call_failed.connect([&](ControlCallId, ControlFailure const& failure) {
        if (std::get<Error>(failure.error).code == "jobu.client.timeout") {
            ++timeouts;
            REQUIRE(fixture.app.quit());
        }
    });
    REQUIRE(fixture.typed->list_runs(RunQuery{}, {.timeout = std::chrono::milliseconds{1}}));
    Timer watchdog;
    watchdog.timeout.connect([&] { REQUIRE(fixture.app.quit()); });
    watchdog.start(std::chrono::milliseconds{100});
    CHECK(fixture.app.exec() == 0);
    CHECK(timeouts == 1);
    fixture.device.inject_input(success(2U, empty_run_page()));
    fixture.drain_tasks();
    CHECK(timeouts == 1);
    REQUIRE(fixture.rpc->call("after-timeout"));
}
