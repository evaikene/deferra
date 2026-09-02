#include "scheduler.hpp"

#include "application.hpp"
#include "attempt_repository_priv.hpp"
#include "attribute_registry.hpp"
#include "byte_buffer.hpp"
#include "database.hpp"
#include "event_loop_types.hpp"
#include "http/http_attempt_executor.hpp"
#include "http/system_http_client.hpp"
#include "json.hpp"
#include "management.hpp"
#include "run_repository_priv.hpp"
#include "sqlite/sqlite_driver.hpp"
#include "sqlite/sqlite_schema.hpp"
#include "support/fake_cron_engine.hpp"
#include "support/fake_time_source.hpp"
#include "support/http_test_server.hpp"
#include "support/sequence_uuid_generator.hpp"
#include "support/temporary_directory.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
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
    explicit RealSchedulerFixture(SchedulerOptions options = {})
        : app{0, nullptr}
        , database_file{directory.path() / "jobu.sqlite"}
        , database{make_database(database_file)}
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
        scheduler  = std::make_unique<Scheduler>(database, registry, cron, generator, time, *executor, options);
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

private:
    auto create_job(Queue const& queue, JobType type, JsonValue payload, AttributeSet attributes, std::int32_t priority)
        -> CreatedJob
    {
        auto definition = management->create_job({
            .queue      = queue.id,
            .type       = type,
            .schedule   = OnceSchedule{.planned_at = at_seconds(90)},
            .priority   = priority,
            .attributes = std::move(attributes),
            .payload    = std::move(payload),
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
    Application                          app;
    HttpTestServer                       server;
    TemporaryDirectory                   directory;
    std::filesystem::path                database_file;
    Database                             database;
    StandardAttributeRegistry            registry;
    FakeCronEngine                       cron;
    SequenceUuidGenerator                generator;
    FakeTimeSource                       time;
    RunRepository                        runs;
    AttemptRepository                    attempts;
    std::unique_ptr<SystemHttpClient>    client;
    std::unique_ptr<HttpAttemptExecutor> executor;
    std::unique_ptr<ManagementService>   management;
    std::unique_ptr<Scheduler>           scheduler;
};

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
    auto const cli = fixture.create_cli_job(narrow, "remains-pending");

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
