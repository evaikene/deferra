#include "execution_telemetry.hpp"

#include "attempt_repository_priv.hpp"
#include "job_repository_priv.hpp"
#include "management.hpp"
#include "query.hpp"
#include "run_repository_priv.hpp"
#include "scheduler.hpp"
#include "scheduler_core_priv.hpp"
#include "secret_provider_priv.hpp"
#include "transaction.hpp"

#include "support/catch_utils.hpp" // IWYU pragma: keep for Catch::StringMaker specializations
#include "support/fake_attempt_executor.hpp"
#include "support/fake_cron_engine.hpp"
#include "support/fake_event_loop_backend.hpp"
#include "support/fake_time_source.hpp"
#include "support/fault_database_driver.hpp"
#include "support/recovery_fixture.hpp"
#include "support/sequence_uuid_generator.hpp"
#include "support/storage_fault_helpers.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace jb::core;
using namespace jb::jobu;
using namespace jb::jobu::detail;
using namespace jb::test;
using namespace std::chrono_literals;

namespace {

auto identities() -> std::vector<Uuid>
{
    std::vector<Uuid> result;
    for (std::uint32_t i = 10000; i < 13000; ++i) {
        result.push_back(recovery_id(i));
    }
    return result;
}

auto classify(std::string_view sql) -> std::string_view
{
    if (sql.starts_with("UPDATE jobu_run_timing SET delay_warned")) {
        return "warning.claim";
    }
    if (sql.starts_with("SELECT r.id, r.job_id")) {
        return "eligibility.read";
    }
    if (sql.starts_with("SELECT r.state, t.runnable_wait_us")) {
        return "timing.read";
    }
    if (sql.starts_with("UPDATE jobu_run_timing")) {
        return "timing.write";
    }
    if (sql.starts_with("INSERT INTO jobu_run_timing")) {
        return "successor.timing";
    }
    if (sql.starts_with("INSERT INTO jobu_runs")) {
        return "successor.run";
    }
    if (sql.starts_with("UPDATE jobu_runs SET state = 'running'")) {
        return "dispatch.run";
    }
    if (sql.starts_with("UPDATE jobu_attempts SET completed_at_us")) {
        return "completion.attempt";
    }
    if (sql.starts_with("UPDATE jobu_runs")) {
        return "completion.run";
    }
    return "other";
}

auto completion(AttemptKey key, bool retry = false) -> AttemptCompletion
{
    auto result = JsonValue{};
    result.data = JsonValue::Object{};
    return {
        .key                 = key,
        .outcome             = retry ? AttemptOutcome::Failed : AttemptOutcome::Succeeded,
        .failure_disposition = retry ? std::optional{FailureDisposition::Retryable} : std::nullopt,
        .result              = std::move(result),
    };
}

/// Observes the real durable boundary and retains copies for deliberately invalid callbacks.
class ObservingExecutor final : public AttemptExecutor {
public:
    ObservingExecutor(jb::db::Database& database, DatabaseFaultState& calls)
        : _database{database}
        , _calls{calls}
    {}

    FakeAttemptExecutor                   fake;
    std::vector<AttemptCompletionHandler> handlers;
    std::function<void()>                 on_start;
    bool                                  synchronous{false};

    auto is_available(JobType type) const noexcept -> bool override { return fake.is_available(type); }

    auto start(AttemptStartRequest request, AttemptCompletionHandler handler) -> Result<void, Error> override
    {
        REQUIRE_FALSE(_calls.calls.empty());
        CHECK(_calls.calls.back() == DatabaseCall{.boundary  = "connection",
                                                  .operation = DatabaseOperation::Commit,
                                                  .phase     = DatabaseFaultPhase::AfterSuccess});
        WaitRepository repository{_database};
        auto           timing = repository.read(request.key.run_id);
        REQUIRE(timing);
        CHECK(timing->state == RunState::Running);
        CHECK_FALSE(timing->open_epoch);

        handlers.push_back(handler);
        if (on_start) {
            on_start();
        }
        if (synchronous) {
            handler(completion(request.key));
        }
        return fake.start(std::move(request), std::move(handler));
    }

    auto cancel(AttemptKey const& key) -> Result<void, Error> override { return fake.cancel(key); }

private:
    jb::db::Database&   _database;
    DatabaseFaultState& _calls;
};

struct Fixture {
    explicit Fixture(bool active = true, bool supply = true, std::uint32_t concurrency = 1, bool http_available = true)
    {
        time.set_utc(UtcTimePoint{100s});
        time.set_monotonic(TimePoint{1000s});
        executor.fake.set_available(JobType::Cli, true);
        executor.fake.set_available(JobType::Http, http_available);
        telemetry = std::make_unique<ExecutionTelemetry>(storage.database, storage.registry, time, generator);
        // Register through public construction, while deterministic core calls keep wake delivery under test control.
        registrar = std::make_unique<Scheduler>(storage.database,
                                                storage.registry,
                                                cron,
                                                generator,
                                                time,
                                                executor,
                                                secrets,
                                                SchedulerOptions{
                                                    .cli_concurrency  = concurrency,
                                                    .http_concurrency = concurrency,
                                                    .telemetry        = telemetry.get(),
                                                });
        if (active) {
            REQUIRE(telemetry->start());
        }
        auto* owner = supply ? telemetry.get() : nullptr;
        service     = std::make_unique<ManagementService>(storage.database,
                                                          storage.registry,
                                                          cron,
                                                          generator,
                                                          time,
                                                          ManagementServiceOptions{.telemetry = owner});
        core        = std::make_unique<SchedulerCore>(
            storage.database,
            storage.registry,
            cron,
            generator,
            time,
            executor,
            secrets,
            SchedulerCoreOptions{.cli_concurrency = concurrency, .http_concurrency = concurrency, .telemetry = owner},
            SchedulerCoreCallbacks{.failure_reported = [this](Error const& error) { failures.push_back(error); }});
        faults->classify = [](std::string_view sql) { return std::string{classify(sql)}; };
        telemetry->failed.connect(&receiver, [this](Error const& error) {
            timing_failures.push_back(error);
            calls_at_failure = faults->calls.size();
        });
        telemetry->delayed.connect(&receiver, [this](DelayedRun const& warning) { warnings.push_back(warning); });
    }

    auto queue(std::uint32_t concurrency = 1, std::chrono::milliseconds threshold = 10000ms) const -> Queue
    {
        auto result = service->create_queue(
            {.name = "queue", .concurrency_limit = concurrency, .runnable_wait_warning = threshold});
        REQUIRE(result);
        return *result;
    }

    auto job(Queue const&        queue,
             AttributeSet        attributes = {},
             JobCreationSchedule schedule   = ImmediateSchedule{},
             JobType             type       = JobType::Cli) const -> JobDefinition
    {
        auto model  = storage.make_job(recovery_id(9000), queue.id, type);
        auto result = service->create_job({
            .queue      = queue.id,
            .name       = "job",
            .type       = type,
            .schedule   = std::move(schedule),
            .attributes = std::move(attributes),
            .payload    = model.payload,
        });
        REQUIRE(result);
        return *result;
    }

    auto scheduled(Uuid job) -> JobRun
    {
        RunRepository repository{storage.database, storage.registry};
        auto          result = repository.find_schedule_owned(job);
        REQUIRE(result);
        REQUIRE(result->has_value());
        return **result;
    }

    auto run(Uuid id) -> JobRun
    {
        RunRepository repository{storage.database, storage.registry};
        auto          result = repository.find_by_id(id);
        REQUIRE(result);
        REQUIRE(result->has_value());
        return **result;
    }

    auto timing(Uuid id) -> WaitTiming
    {
        WaitRepository repository{storage.database};
        auto           result = repository.read(id);
        REQUIRE(result);
        return *result;
    }

    void finish(bool retry = false)
    {
        auto keys = executor.fake.pending_keys();
        REQUIRE(keys.size() == 1);
        REQUIRE(executor.fake.complete(keys.front(), completion(keys.front(), retry)));
    }

    void fault(std::string        boundary,
               DatabaseOperation  operation = DatabaseOperation::Execute,
               DatabaseFaultPhase phase     = DatabaseFaultPhase::Before,
               std::string        code      = "db.io")
    {
        faults->faults.push_back({
            .at    = {.boundary = std::move(boundary), .operation = operation, .phase = phase},
            .error = fault_error(std::move(code)),
        });
    }

    void sql(std::string_view text)
    {
        jb::db::Query query{storage.database};
        REQUIRE(query.exec(text));
    }

    /// Release every borrower before inspecting a poisoned/uncertain connection anew.
    void reopen()
    {
        core = nullptr;
        registrar.reset();
        service.reset();
        telemetry.reset();
        storage.reopen();
    }

    jb::core::priv::FakeEventLoop          loop{jb::core::priv::make_fake_event_loop()};
    jb::core::priv::ScopedCurrentEventLoop current{loop.loop.get()};
    std::shared_ptr<DatabaseFaultState>    faults{std::make_shared<DatabaseFaultState>()};
    RecoveryFixture                        storage{[this](std::unique_ptr<jb::db::Driver> driver) {
        return std::make_unique<FaultDatabaseDriver>(std::move(driver), faults);
    }};
    FakeCronEngine                         cron;
    FakeTimeSource                         time;
    SequenceUuidGenerator                  generator{identities()};
    ObservingExecutor                      executor{storage.database, *faults};
    DatabaseSecretProvider                 secrets{storage.database};
    Object                                 receiver;
    std::vector<Error>                     failures;
    std::vector<Error>                     timing_failures;
    std::vector<DelayedRun>                warnings;
    std::size_t                            calls_at_failure{0};
    std::unique_ptr<ExecutionTelemetry>    telemetry;
    std::unique_ptr<Scheduler>             registrar;
    std::unique_ptr<ManagementService>     service;
    std::unique_ptr<SchedulerCore>         core;
};

auto retry_attributes(std::string mode, std::string strategy = "fixed", Duration delay = 10s) -> AttributeSet
{
    return {
        {"retry.max_attempts",  {.data = std::int64_t{3}}    },
        {"retry.mode",          {.data = std::move(mode)}    },
        {"retry.strategy",      {.data = std::move(strategy)}},
        {"retry.initial_delay", {.data = delay}              },
        {"retry.max_delay",     {.data = Duration{60s}}      },
        {"retry.multiplier",    {.data = 2.0}                },
        {"retry.jitter",        {.data = 0.0}                },
    };
}

} // namespace

TEST_CASE("Dispatch closes wait before external start and completion excludes execution",
          "[jobu][telemetry][scheduler]")
{
    auto    type = GENERATE(JobType::Cli, JobType::Http);
    Fixture f;
    auto    queue = f.queue();
    auto    run   = f.scheduled(f.job(queue, {}, ImmediateSchedule{}, type).id);
    f.time.advance(7s);
    REQUIRE(f.core->process_cycle());
    CHECK(f.timing(run.id).runnable_wait_us == 7'000'000);
    CHECK_FALSE(f.timing(run.id).open_epoch);
    f.time.advance(50s);
    f.finish();
    CHECK(f.run(run.id).state == RunState::Succeeded);
    CHECK(f.timing(run.id).quality == WaitQuality::Complete);
    CHECK(f.timing(run.id).runnable_wait_us == 7'000'000);
    CHECK_FALSE(f.timing(run.id).open_epoch);
}

TEST_CASE("Observed work keeps waiting under full global or queue capacity", "[jobu][telemetry][scheduler][capacity]")
{
    auto    global = GENERATE(false, true);
    Fixture f{true, true, global ? 1U : 2U};
    auto    queue  = f.queue(global ? 2U : 1U);
    auto    first  = f.scheduled(f.job(queue).id);
    auto    second = f.scheduled(f.job(queue).id);
    f.time.advance(3s);
    REQUIRE(f.core->process_cycle());
    REQUIRE(f.executor.fake.pending_keys().size() == 1);
    CHECK(f.timing(first.id).runnable_wait_us == 3'000'000);
    CHECK(f.timing(second.id).open_epoch);
    f.time.advance(4s);
    REQUIRE(f.core->process_cycle());
    f.finish();
    REQUIRE(f.core->process_cycle());
    CHECK(f.timing(second.id).runnable_wait_us == 7'000'000);
    CHECK_FALSE(f.timing(second.id).open_epoch);
}

TEST_CASE("Retry totals survive fixed and exponential backoff in both occupancy modes",
          "[jobu][telemetry][scheduler][retry]")
{
    auto    mode     = GENERATE(std::string{"blocking"}, std::string{"reschedule"});
    auto    strategy = GENERATE(std::string{"fixed"}, std::string{"exponential"});
    Fixture f;
    auto    queue = f.queue();
    auto    run   = f.scheduled(f.job(queue, retry_attributes(mode, strategy)).id);
    f.time.advance(2s);
    REQUIRE(f.core->process_cycle());

    for (int attempt = 1; attempt <= 2; ++attempt) {
        f.time.advance(30s);
        auto const completed_at = f.time.utc_now();
        f.finish(true);
        auto const delay = strategy == "exponential" && attempt == 2 ? 20s : 10s;
        CHECK(f.run(run.id).runnable_at == completed_at + delay);
        CHECK_FALSE(f.timing(run.id).open_epoch);
        f.time.advance(delay - 1s);
        REQUIRE(f.core->process_cycle());
        CHECK(f.executor.fake.pending_keys().empty());

        f.time.advance(1s);
        // General due-backlog observation is Stage 9.12. A real scoped mutation
        // observes this due retry now, independently of its occupied blocking slot.
        REQUIRE(f.service->update_queue({.queue = queue.id, .weight = static_cast<std::uint32_t>(attempt + 1)}));
        CHECK(f.timing(run.id).open_epoch);
        f.time.advance(3s);
        REQUIRE(f.core->process_cycle());
        CHECK(f.timing(run.id).runnable_wait_us == 2'000'000 + (attempt * 3'000'000));
    }
    f.time.advance(30s);
    f.finish();
    CHECK(f.timing(run.id).runnable_wait_us == 8'000'000);
    CHECK(f.run(run.id).state == RunState::Succeeded);
}

TEST_CASE("Zero-backoff retry reopens at completion and warning state survives", "[jobu][telemetry][scheduler][retry]")
{
    Fixture f;
    auto    queue = f.queue();
    auto    run   = f.scheduled(f.job(queue, retry_attributes("blocking", "fixed", 0s)).id);
    f.sql("UPDATE jobu_run_timing SET delay_warned = 1");
    f.time.advance(2s);
    REQUIRE(f.core->process_cycle());
    f.time.advance(20s);
    f.finish(true);
    CHECK(f.timing(run.id).open_epoch);
    f.time.advance(4s);
    REQUIRE(f.core->process_cycle());
    CHECK(f.timing(run.id).runnable_wait_us == 6'000'000);
    CHECK(f.timing(run.id).delay_warned);
}

TEST_CASE("Manual completion releases its sibling only under active owner gates",
          "[jobu][telemetry][scheduler][manual]")
{
    auto    suspended = GENERATE(false, true);
    Fixture f;
    auto    queue     = f.queue();
    auto    job       = f.job(queue, {}, OnceSchedule{.planned_at = UtcTimePoint{102s}});
    auto    scheduled = f.scheduled(job.id);
    auto    manual    = f.service->run_now({.job_id = job.id});
    REQUIRE(manual);
    if (suspended) {
        REQUIRE(f.service->suspend_job(job.id));
    }
    f.time.advance(2s);
    REQUIRE(f.core->process_cycle());
    REQUIRE(f.executor.fake.pending_keys().size() == 1);
    CHECK(f.executor.fake.pending_keys().front().run_id == manual->id);
    f.time.advance(20s);
    f.finish();
    CHECK(f.timing(scheduled.id).runnable_wait_us == 0);
    CHECK(f.timing(scheduled.id).open_epoch.has_value() == !suspended);
    if (!suspended) {
        f.time.advance(3s);
        REQUIRE(f.core->process_cycle());
        CHECK(f.timing(scheduled.id).runnable_wait_us == 3'000'000);
    }
}

TEST_CASE("Ordinary completion creates a complete closed recurring successor",
          "[jobu][telemetry][scheduler][recurrence]")
{
    Fixture            f;
    CronSchedule const schedule{.expression = "* * * * *", .timezone = "UTC"};
    f.cron.set_occurrences(schedule, {UtcTimePoint{101s}, UtcTimePoint{200s}, UtcTimePoint{300s}});
    auto queue = f.queue();
    auto job   = f.job(queue, {}, CronScheduleInput{.expression = schedule.expression, .timezone = schedule.timezone});
    auto run   = f.scheduled(job.id);
    f.time.advance(1s);
    REQUIRE(f.core->process_cycle());
    f.time.advance(20s);
    f.finish();
    auto successor = f.scheduled(job.id);
    CHECK(successor.id != run.id);
    CHECK(f.timing(successor.id).quality == WaitQuality::Complete);
    CHECK_FALSE(f.timing(successor.id).open_epoch);
    CHECK(f.timing(successor.id).runnable_wait_us == 0);
}

TEST_CASE("Running cancellation completes atomically without execution wait",
          "[jobu][telemetry][scheduler][cancellation]")
{
    Fixture f;
    auto    queue = f.queue();
    auto    run   = f.scheduled(f.job(queue, retry_attributes("blocking")).id);
    f.time.advance(2s);
    REQUIRE(f.core->process_cycle());
    REQUIRE(f.core->cancel_run(run.id));
    f.time.advance(20s);
    f.finish(true);
    CHECK(f.run(run.id).state == RunState::Cancelled);
    CHECK(f.timing(run.id).runnable_wait_us == 2'000'000);
    CHECK_FALSE(f.timing(run.id).open_epoch);
}

TEST_CASE("Immediate start failures use fresh samples between dispatches", "[jobu][telemetry][scheduler][immediate]")
{
    Fixture f;
    auto    queue  = f.queue();
    auto    first  = f.scheduled(f.job(queue).id);
    auto    second = f.scheduled(f.job(queue).id);
    f.executor.fake.set_start_error(fault_error("test.start.failed"));
    f.executor.on_start = [&f] { f.time.advance(10s); };
    f.time.advance(2s);
    REQUIRE(f.core->process_cycle());
    CHECK(f.run(first.id).state == RunState::Failed);
    CHECK(f.run(second.id).state == RunState::Failed);
    CHECK(f.timing(first.id).runnable_wait_us == 2'000'000);
    CHECK(f.timing(second.id).runnable_wait_us == 12'000'000);
    CHECK(f.failures.empty());
    CHECK(f.timing_failures.empty());
}

TEST_CASE("Embedded closed timing stays unmeasured and late dispatch stays partial",
          "[jobu][telemetry][scheduler][quality]")
{
    auto    supply = GENERATE(false, true);
    Fixture f{true, supply};
    auto    queue = f.queue();
    auto    run   = f.scheduled(f.job(queue).id);
    if (supply) {
        // Simulate a run born before instrumentation, without a fabricated prior wait.
        f.sql("UPDATE jobu_run_timing SET measurement_status = 'unmeasured', open_epoch = NULL, open_tick_us = NULL");
    }
    f.time.advance(20s);
    REQUIRE(f.core->process_cycle());
    f.finish();
    CHECK(f.timing(run.id).quality == (supply ? WaitQuality::Partial : WaitQuality::Unmeasured));
    CHECK(f.timing(run.id).runnable_wait_us == 0);
}

TEST_CASE("Synchronous and mismatched callbacks leave committed Running timing closed",
          "[jobu][telemetry][scheduler][protocol]")
{
    auto    sync = GENERATE(false, true);
    Fixture f;
    auto    queue          = f.queue();
    auto    run            = f.scheduled(f.job(queue).id);
    f.executor.synchronous = sync;
    auto result            = f.core->process_cycle();
    if (sync) {
        REQUIRE_FALSE(result);
        CHECK(result.error().code == "jobu.executor.invalid_completion");
    }
    else {
        REQUIRE(result);
        auto key = f.executor.fake.pending_keys().front();
        ++key.attempt_number;
        f.executor.handlers.front()(completion(key));
    }
    REQUIRE(f.failures.size() == 1);
    CHECK(f.run(run.id).state == RunState::Running);
    CHECK_FALSE(f.timing(run.id).open_epoch);
    auto before = storage_snapshot(f.storage.database);
    auto calls  = f.faults->calls.size();
    f.executor.handlers.front()(completion(f.executor.fake.pending_keys().front()));
    CHECK(f.faults->calls.size() == calls);
    CHECK(storage_snapshot(f.storage.database) == before);
}

TEST_CASE("Dispatch accounting faults restore the original open interval", "[jobu][telemetry][scheduler][fault]")
{
    auto    boundary = GENERATE(std::string{"timing.read"}, std::string{"timing.write"}, std::string{"dispatch.run"});
    Fixture f;
    auto    queue = f.queue();
    f.job(queue);
    auto before = storage_snapshot(f.storage.database);
    f.time.advance(2s);
    f.fault(boundary);
    auto result = f.core->process_cycle();
    REQUIRE_FALSE(result);
    check_safe_error(result.error(), "db.io");
    require_consumed_faults(*f.faults);
    CHECK(f.executor.fake.start_requests().empty());
    REQUIRE(f.timing_failures.size() == 1);
    CHECK(f.faults->calls.size() == f.calls_at_failure);
    CHECK(storage_snapshot(f.storage.database) == before);
}

TEST_CASE("Completion accounting faults roll back manual barrier release", "[jobu][telemetry][scheduler][fault]")
{
    auto    boundary = GENERATE(std::string{"timing.read"}, std::string{"timing.write"});
    Fixture f;
    auto    queue = f.queue();
    auto    job   = f.job(queue, {}, OnceSchedule{.planned_at = UtcTimePoint{101s}});
    REQUIRE(f.service->run_now({.job_id = job.id}));
    REQUIRE(f.core->process_cycle());
    auto before = storage_snapshot(f.storage.database);
    f.time.advance(20s);
    f.fault(boundary);
    f.finish();
    require_consumed_faults(*f.faults);
    REQUIRE(f.failures.size() == 1);
    REQUIRE(f.timing_failures.size() == 1);
    check_safe_error(f.failures.front(), "db.io");
    CHECK(f.faults->calls.size() == f.calls_at_failure);
    CHECK(storage_snapshot(f.storage.database) == before);
}

TEST_CASE("Lost dispatch acknowledgement commits closed timing without external launch",
          "[jobu][telemetry][scheduler][fault]")
{
    Fixture f;
    auto    queue = f.queue();
    auto    run   = f.scheduled(f.job(queue).id);
    f.time.advance(2s);
    // Observation now commits its own pages before dispatch. Arm acknowledgement loss at
    // the Running write so this still exercises the durable-start transaction specifically.
    f.faults->classify = [&f](std::string_view sql) {
        auto label = classify(sql);
        if (label == "dispatch.run") {
            f.fault("connection", DatabaseOperation::Commit, DatabaseFaultPhase::AfterSuccess);
        }
        return std::string{label};
    };
    REQUIRE_FALSE(f.core->process_cycle());
    require_consumed_faults(*f.faults);
    CHECK(f.executor.fake.start_requests().empty());
    // Acknowledgement loss poisons the connection when rollback discovers an already
    // committed transaction. Inspect durable evidence only after exclusive reopen.
    f.reopen();
    CHECK(f.run(run.id).state == RunState::Running);
    CHECK_FALSE(f.timing(run.id).open_epoch);
    CHECK(f.timing(run.id).runnable_wait_us == 2'000'000);
}

TEST_CASE("Accounting failure receivers may destroy telemetry after cleanup", "[jobu][telemetry][scheduler][lifetime]")
{
    Fixture f;
    auto    queue = f.queue();
    f.job(queue);
    f.telemetry->failed.connect(&f.receiver, [&f](Error const&) {
        auto transaction = jb::db::Transaction::begin(f.storage.database);
        REQUIRE(transaction);
        REQUIRE(transaction->rollback());
        f.telemetry.reset();
    });
    f.fault("timing.write");
    REQUIRE_FALSE(f.core->process_cycle());
    CHECK_FALSE(f.telemetry);
    auto calls = f.faults->calls.size();
    REQUIRE_FALSE(f.core->process_cycle());
    CHECK(f.faults->calls.size() == calls);
}

TEST_CASE("Preparation failures preserve observed wait without external execution",
          "[jobu][telemetry][scheduler][preparation]")
{
    Fixture f;
    auto    queue   = f.queue();
    // Seed an immutable template whose secret has disappeared. Resolution, rather than
    // creation validation, must produce the ordinary terminal preparation outcome.
    auto    job     = f.storage.make_job(recovery_id(1), queue.id);
    auto    payload = parse_json(R"({"command":"/test","arguments":[{"secret":"missing"}]})");
    REQUIRE(payload);
    job.payload = *payload;
    f.storage.insert_job(job);
    auto seeded = f.storage.make_run(recovery_id(2), job);
    f.storage.insert_run(seeded);
    REQUIRE(f.service->update_queue({.queue = queue.id, .weight = 2}));
    f.time.advance(3s);
    REQUIRE(f.core->process_cycle());
    CHECK(f.executor.fake.start_requests().empty());
    CHECK(f.run(seeded.run.id).state == RunState::Failed);
    CHECK(f.timing(seeded.run.id).runnable_wait_us == 3'000'000);
    CHECK(f.timing(seeded.run.id).quality == WaitQuality::Partial);
    CHECK_FALSE(f.timing(seeded.run.id).open_epoch);
    CHECK(f.timing_failures.empty());
}

TEST_CASE("Due blocking retry keeps runner wait and reuses its occupied queue slot",
          "[jobu][telemetry][scheduler][capacity][retry]")
{
    Fixture f;
    auto    queue = f.queue();
    auto    retry = f.scheduled(f.job(queue, retry_attributes("blocking")).id);
    REQUIRE(f.core->process_cycle());
    f.finish(true);
    auto other_queue = f.service->create_queue({.name = "other"});
    REQUIRE(other_queue);
    f.job(*other_queue);
    REQUIRE(f.core->process_cycle());

    f.time.advance(10s);
    REQUIRE(f.service->update_queue({.queue = queue.id, .weight = 2}));
    CHECK(f.timing(retry.id).open_epoch);
    f.time.advance(4s);
    REQUIRE(f.core->process_cycle());
    CHECK(f.executor.fake.start_requests().size() == 2);
    f.finish();
    REQUIRE(f.core->process_cycle());
    CHECK(f.executor.fake.pending_keys().front().run_id == retry.id);
    CHECK(f.timing(retry.id).runnable_wait_us == 4'000'000);
    CHECK_FALSE(f.timing(retry.id).open_epoch);
}

TEST_CASE("Completion acknowledgement loss preserves atomic barrier and timing changes",
          "[jobu][telemetry][scheduler][fault]")
{
    auto    lost = GENERATE(false, true);
    Fixture f;
    auto    queue   = f.queue();
    auto    job     = f.job(queue, {}, OnceSchedule{.planned_at = UtcTimePoint{101s}});
    auto    sibling = f.scheduled(job.id);
    auto    manual  = f.service->run_now({.job_id = job.id});
    REQUIRE(manual);
    f.time.advance(2s);
    REQUIRE(f.core->process_cycle());
    auto before = storage_snapshot(f.storage.database);
    f.time.advance(20s);
    f.fault("connection",
            DatabaseOperation::Commit,
            lost ? DatabaseFaultPhase::AfterSuccess : DatabaseFaultPhase::Before);
    f.finish();
    require_consumed_faults(*f.faults);
    REQUIRE(f.failures.size() == 1);
    auto calls = f.faults->calls.size();
    f.executor.handlers.front()(completion(f.executor.fake.start_requests().front().key));
    CHECK(f.faults->calls.size() == calls);
    f.reopen();
    if (lost) {
        CHECK(f.run(manual->id).state == RunState::Succeeded);
        CHECK(f.timing(manual->id).runnable_wait_us == 2'000'000);
        CHECK_FALSE(f.timing(manual->id).open_epoch);
        CHECK(f.timing(sibling.id).open_tick_us == 22'000'000);
    }
    else {
        CHECK(storage_snapshot(f.storage.database) == before);
    }
}

TEST_CASE("Dispatch corruption and arithmetic failures preserve provenance and storage",
          "[jobu][telemetry][scheduler][fault]")
{
    auto scenario =
        GENERATE(std::string{"missing"}, std::string{"foreign"}, std::string{"overflow"}, std::string{"clock"});
    Fixture f;
    auto    queue = f.queue();
    f.job(queue);
    auto code = std::string{"jobu.telemetry.invalid_state"};
    if (scenario == "missing") {
        f.sql("DELETE FROM jobu_run_timing");
    }
    else if (scenario == "foreign") {
        f.sql("UPDATE jobu_run_timing SET open_epoch = X'00000000000070008000000000000001'");
    }
    else if (scenario == "overflow") {
        f.sql("UPDATE jobu_run_timing SET runnable_wait_us = 9223372036854775807");
        code = "jobu.telemetry.counter_overflow";
    }
    else {
        code = "jobu.telemetry.clock_regression";
    }
    auto before = storage_snapshot(f.storage.database);
    f.time.advance(1s);
    if (scenario == "clock") {
        f.time.set_monotonic(TimePoint{999s});
    }
    auto result = f.core->process_cycle();
    REQUIRE_FALSE(result);
    CHECK(result.error().code == code);
    if (scenario == "missing" || scenario == "foreign") {
        CHECK(result.error().detail == "operation=dispatch reason=durable_invariant");
    }
    REQUIRE(f.timing_failures.size() == 1);
    CHECK(f.executor.fake.start_requests().empty());
    CHECK(storage_snapshot(f.storage.database) == before);
}

TEST_CASE("Missing or open Running timing aborts completion before durable changes",
          "[jobu][telemetry][scheduler][fault]")
{
    auto    missing = GENERATE(false, true);
    Fixture f;
    auto    queue = f.queue();
    f.job(queue);
    REQUIRE(f.core->process_cycle());
    if (missing) {
        f.sql("DELETE FROM jobu_run_timing");
    }
    else {
        f.sql("UPDATE jobu_run_timing SET open_epoch = X'00000000000070008000000000000001', open_tick_us = 0");
    }
    auto before = storage_snapshot(f.storage.database);
    f.finish();
    REQUIRE(f.failures.size() == 1);
    CHECK(f.failures.front().detail == "operation=completion reason=durable_invariant");
    CHECK(storage_snapshot(f.storage.database) == before);
}

TEST_CASE("Accounting failure outranks rollback poison and suppresses later callbacks",
          "[jobu][telemetry][scheduler][fault]")
{
    auto    completing = GENERATE(false, true);
    Fixture f;
    auto    queue = f.queue();
    f.job(queue);
    if (completing) {
        REQUIRE(f.core->process_cycle());
    }
    auto before = storage_snapshot(f.storage.database);
    f.time.advance(2s);
    f.fault(completing ? "timing.read" : "timing.write",
            DatabaseOperation::Execute,
            DatabaseFaultPhase::Before,
            "db.corrupt");
    f.fault("connection", DatabaseOperation::Rollback, DatabaseFaultPhase::Before, "test.rollback.failed");
    if (completing) {
        f.finish();
        REQUIRE(f.failures.size() == 1);
        CHECK(f.failures.front().code == "db.corrupt");
    }
    else {
        auto result = f.core->process_cycle();
        REQUIRE_FALSE(result);
        CHECK(result.error().code == "db.corrupt");
    }
    require_consumed_faults(*f.faults);
    CHECK(f.storage.database.is_poisoned());
    REQUIRE(f.timing_failures.size() == 1);
    CHECK(f.timing_failures.front().code == "db.corrupt");
    f.reopen();
    CHECK(storage_snapshot(f.storage.database) == before);
}

TEST_CASE("Completion failure receiver may destroy telemetry before accounting notification",
          "[jobu][telemetry][scheduler][lifetime]")
{
    Fixture f;
    auto    queue = f.queue();
    f.job(queue);
    f.core = std::make_unique<SchedulerCore>(f.storage.database,
                                             f.storage.registry,
                                             f.cron,
                                             f.generator,
                                             f.time,
                                             f.executor,
                                             f.secrets,
                                             SchedulerCoreOptions{.telemetry = f.telemetry.get()},
                                             SchedulerCoreCallbacks{
                                                 .failure_reported =
                                                     [&f](Error const& error) {
                                                         CHECK(error.code == "db.io");
                                                         auto transaction =
                                                             jb::db::Transaction::begin(f.storage.database);
                                                         REQUIRE(transaction);
                                                         REQUIRE(transaction->rollback());
                                                         f.telemetry.reset();
                                                     },
                                             });
    REQUIRE(f.core->process_cycle());
    f.fault("timing.read");
    f.finish();
    require_consumed_faults(*f.faults);
    CHECK_FALSE(f.telemetry);
    CHECK(f.timing_failures.empty());
}

TEST_CASE("Public scheduler preserves accounting failure provenance", "[jobu][telemetry][scheduler][public]")
{
    auto    completing = GENERATE(false, true);
    Fixture f;
    auto    queue = f.queue();
    f.job(queue);
    std::vector<Error> observed;
    f.registrar->failed.connect(&f.receiver, [&observed](Error const& error) { observed.push_back(error); });
    if (completing) {
        REQUIRE(f.registrar->start());
    }
    f.sql("DELETE FROM jobu_run_timing");
    if (completing) {
        f.finish();
    }
    else {
        auto result = f.registrar->start();
        REQUIRE_FALSE(result);
        CHECK(result.error().detail == "operation=dispatch reason=durable_invariant");
    }
    REQUIRE(observed.size() == 1);
    CHECK(observed.front().code == "jobu.telemetry.invalid_state");
    CHECK(observed.front().detail == (completing ? "operation=completion reason=durable_invariant"
                                                 : "operation=dispatch reason=durable_invariant"));
    CHECK(f.registrar->state() == SchedulerState::Failed);
}

TEST_CASE("Dispatch and completion retain one sample when durable writes advance the clock",
          "[jobu][telemetry][scheduler][clock]")
{
    Fixture f;
    auto    queue      = f.queue();
    auto    run        = f.scheduled(f.job(queue, retry_attributes("blocking", "fixed", 0s)).id);
    f.faults->classify = [&f](std::string_view sql) {
        auto label = classify(sql);
        if (label == "dispatch.run" || label == "completion.attempt") {
            f.time.advance(10s);
        }
        return std::string{label};
    };
    f.time.advance(2s);
    REQUIRE(f.core->process_cycle());
    CHECK(f.run(run.id).started_at == UtcTimePoint{102s});
    CHECK(f.timing(run.id).runnable_wait_us == 2'000'000);
    f.finish(true);
    CHECK(f.run(run.id).runnable_at == UtcTimePoint{112s});
    CHECK(f.timing(run.id).open_tick_us == 12'000'000);
    CHECK(f.timing(run.id).runnable_wait_us == 2'000'000);
}

TEST_CASE("Omitted telemetry rejects open wait at the claim boundary", "[jobu][telemetry][scheduler][embedded]")
{
    Fixture f{true, false};
    auto    queue = f.queue();
    f.job(queue);
    f.sql("UPDATE jobu_run_timing SET measurement_status = 'partial', "
          "open_epoch = X'00000000000070008000000000000001', open_tick_us = 0");
    auto before = storage_snapshot(f.storage.database);
    auto result = f.core->process_cycle();
    REQUIRE_FALSE(result);
    CHECK(result.error().code == "jobu.telemetry.invalid_state");
    CHECK(f.executor.fake.start_requests().empty());
    CHECK(storage_snapshot(f.storage.database) == before);
}

TEST_CASE("Automatic observation reaches backlog tails despite exhausted capacity",
          "[jobu][telemetry][scheduler][warning][page]")
{
    auto    global = GENERATE(false, true);
    Fixture f{true, true, global ? 1U : 2U};
    auto    queue = f.queue(global ? 2U : 1U, 10ms);
    f.job(queue);
    REQUIRE(f.core->process_cycle());

    // These rows bypass covered creation, so the scheduler must discover them itself and
    // establish Partial. More than two pages exercise the exclusive keyset's final tail.
    std::vector<Uuid> waiting;
    for (std::uint32_t i = 0; i < 401; ++i) {
        auto job = f.storage.make_job(recovery_id(20000 + (2 * i)), queue.id);
        auto row = f.storage.make_run(recovery_id(20001 + (2 * i)), job);
        f.storage.insert_job(job);
        f.storage.insert_run(row);
        waiting.push_back(row.run.id);
    }
    REQUIRE(f.core->process_cycle());
    for (auto id : waiting) {
        auto timing = f.timing(id);
        CHECK(timing.quality == WaitQuality::Partial);
        CHECK(timing.open_epoch);
    }
    f.time.advance(10001us);
    REQUIRE(f.core->process_cycle());
    REQUIRE(f.warnings.size() == waiting.size());
    for (std::size_t i = 0; i < waiting.size(); ++i) {
        CHECK(f.warnings[i].run_id == waiting[i]);
        CHECK(f.warnings[i].runnable_wait == 10001us);
        CHECK(f.timing(waiting[i]).delay_warned);
        CHECK(f.timing(waiting[i]).runnable_wait_us == 0); // Observation is not a checkpoint sweep.
    }
    REQUIRE(f.core->process_cycle());
    CHECK(f.warnings.size() == waiting.size());
    CHECK(f.executor.fake.start_requests().size() == 1);
}

TEST_CASE("Warning-only wakes cross equality once and remain bounded to one timer",
          "[jobu][telemetry][scheduler][warning][wake]")
{
    Fixture f;
    auto    queue = f.queue(1, 10ms);
    f.job(queue);
    auto waiting = f.scheduled(f.job(queue).id);
    REQUIRE(f.registrar->start());
    CHECK(jb::core::priv::EventLoopTestAccess::active_timer_count(*f.loop.loop) == 1);
    f.time.advance(10ms);
    jb::core::priv::EventLoopTestAccess::fire_next_timer(*f.loop.loop);
    CHECK(f.warnings.empty());
    auto deadline = jb::core::priv::EventLoopTestAccess::next_timer_deadline(*f.loop.loop);
    REQUIRE(deadline);
    auto equality = f.core->process_cycle();
    REQUIRE(equality);
    CHECK(equality->next_warning == f.time.monotonic_now() + 1us);
    CHECK(jb::core::priv::EventLoopTestAccess::active_timer_count(*f.loop.loop) == 1);
    f.time.advance(1us);
    jb::core::priv::EventLoopTestAccess::fire_next_timer(*f.loop.loop);
    REQUIRE(f.warnings.size() == 1);
    CHECK(f.warnings.front().run_id == waiting.id);
    CHECK(f.warnings.front().runnable_wait == 10001us);
    CHECK(jb::core::priv::EventLoopTestAccess::active_timer_count(*f.loop.loop) == 0);
}

TEST_CASE("Disabled warnings still count wait and threshold edits use current queue policy",
          "[jobu][telemetry][scheduler][warning][policy]")
{
    Fixture f;
    auto    queue = f.queue(1, 0ms);
    f.job(queue);
    auto waiting = f.scheduled(f.job(queue).id);
    REQUIRE(f.core->process_cycle());
    f.time.advance(20ms);
    auto disabled = f.core->process_cycle();
    REQUIRE(disabled);
    CHECK_FALSE(disabled->next_warning);
    CHECK(f.warnings.empty());
    REQUIRE(f.service->update_queue({.queue = queue.id, .runnable_wait_warning = 30ms}));
    auto higher = f.core->process_cycle();
    REQUIRE(higher);
    CHECK(higher->next_warning == f.time.monotonic_now() + 10001us);
    CHECK(f.warnings.empty());
    REQUIRE(f.service->update_queue({.queue = queue.id, .runnable_wait_warning = 5ms}));
    REQUIRE(f.core->process_cycle());
    REQUIRE(f.warnings.size() == 1);
    CHECK(f.warnings.front().threshold == 5ms);
    CHECK(f.warnings.front().runnable_wait == 20ms);
    CHECK(f.timing(waiting.id).runnable_wait_us == 20000);
    REQUIRE(f.service->update_queue({.queue = queue.id, .runnable_wait_warning = 0ms}));
    REQUIRE(f.service->update_queue({.queue = queue.id, .runnable_wait_warning = 1ms}));
    REQUIRE(f.core->process_cycle());
    CHECK(f.warnings.size() == 1);
}

TEST_CASE("UTC corrections change observed due eligibility without charging corrected wall time",
          "[jobu][telemetry][scheduler][warning][clock]")
{
    Fixture f;
    auto    queue = f.queue(1, 10ms);
    f.job(queue);
    auto waiting = f.scheduled(f.job(queue, {}, OnceSchedule{.planned_at = UtcTimePoint{200s}}).id);
    REQUIRE(f.core->process_cycle());
    f.time.set_utc(UtcTimePoint{200s});
    REQUIRE(f.core->process_cycle());
    f.time.advance(4ms);
    f.time.set_utc(UtcTimePoint{150s});
    REQUIRE(f.core->process_cycle());
    CHECK(f.timing(waiting.id).runnable_wait_us == 4000);
    CHECK_FALSE(f.timing(waiting.id).open_epoch);
    f.time.advance(100s);
    REQUIRE(f.core->process_cycle());
    CHECK(f.warnings.empty());
    f.time.advance(6001us);
    REQUIRE(f.core->process_cycle());
    REQUIRE(f.warnings.size() == 1);
    CHECK(f.warnings.front().runnable_wait == 10001us);
}

TEST_CASE("Suspended and barrier-blocked work cannot acquire warnings",
          "[jobu][telemetry][scheduler][warning][eligibility]")
{
    Fixture f;
    auto    queue = f.queue(1, 10ms);
    f.job(queue);
    auto job       = f.job(queue, {}, OnceSchedule{.planned_at = UtcTimePoint{101s}});
    auto scheduled = f.scheduled(job.id);
    auto manual    = f.service->run_now({.job_id = job.id});
    REQUIRE(manual);
    REQUIRE(f.service->suspend_job(job.id));
    REQUIRE(f.core->process_cycle());
    f.time.advance(1s);
    REQUIRE(f.core->process_cycle());
    REQUIRE(f.warnings.size() == 1);
    CHECK(f.warnings.front().run_id == manual->id); // Accepted manual work bypasses job suspension.
    CHECK_FALSE(f.timing(scheduled.id).open_epoch);
    auto other = f.job(queue, {}, OnceSchedule{.planned_at = UtcTimePoint{102s}});
    REQUIRE(f.service->suspend_queue(queue.id));
    f.time.advance(1s);
    REQUIRE(f.core->process_cycle());
    CHECK(f.warnings.size() == 1);
    CHECK_FALSE(f.timing(f.scheduled(other.id).id).open_epoch);
}

TEST_CASE("Future retries are discovered automatically and warnings survive subsequent attempts",
          "[jobu][telemetry][scheduler][warning][retry]")
{
    auto    mode = GENERATE(std::string{"blocking"}, std::string{"reschedule"});
    Fixture f;
    auto    queue = f.queue(2, 10ms);
    auto    retry = f.scheduled(f.job(queue, retry_attributes(mode, "fixed", 1s)).id);
    REQUIRE(f.core->process_cycle());
    f.finish(true);
    // A competing attempt consumes the global runner, including when the retry retains a slot.
    f.job(queue);
    REQUIRE(f.core->process_cycle());
    f.time.advance(500ms);
    REQUIRE(f.core->process_cycle());
    CHECK_FALSE(f.timing(retry.id).open_epoch);
    CHECK(f.warnings.empty());
    f.time.advance(500ms);
    REQUIRE(f.core->process_cycle());
    CHECK(f.timing(retry.id).open_epoch);
    CHECK(f.timing(retry.id).runnable_wait_us == 0);
    f.time.advance(10001us);
    REQUIRE(f.core->process_cycle());
    REQUIRE(f.warnings.size() == 1);
    CHECK(f.warnings.front().run_id == retry.id);
    f.finish();
    REQUIRE(f.core->process_cycle());
    CHECK(f.run(retry.id).state == RunState::Running);
    f.finish(true);
    f.time.advance(2s);
    REQUIRE(f.core->process_cycle());
    f.finish();
    CHECK(f.warnings.size() == 1);
    CHECK(f.timing(retry.id).delay_warned);
    CHECK(f.timing(retry.id).runnable_wait_us == 10001);
}

TEST_CASE("Moving eligible work changes threshold ownership without resetting the warning claim",
          "[jobu][telemetry][scheduler][warning][move]")
{
    Fixture f;
    auto    queue = f.queue(1, 100ms);
    f.job(queue);
    auto job     = f.job(queue);
    auto waiting = f.scheduled(job.id);
    REQUIRE(f.core->process_cycle());
    f.time.advance(20ms);
    auto destination = f.service->create_queue({.name = "destination", .runnable_wait_warning = 10ms});
    REQUIRE(destination);
    auto suspended = f.service->suspend_job(job.id);
    REQUIRE(suspended);
    auto moved = f.service->move_job(
        {.job_id = job.id, .expected_revision = suspended->revision, .target_queue = destination->id});
    REQUIRE(moved);
    REQUIRE(f.service->resume_job(job.id));
    REQUIRE(f.core->process_cycle());
    REQUIRE(f.warnings.size() == 1);
    CHECK(f.warnings.front().run_id == waiting.id);
    CHECK(f.warnings.front().queue_id == destination->id);
    CHECK(f.warnings.front().threshold == 10ms);
    suspended = f.service->suspend_job(job.id);
    REQUIRE(suspended);
    REQUIRE(
        f.service->move_job({.job_id = job.id, .expected_revision = suspended->revision, .target_queue = queue.id}));
    REQUIRE(f.service->resume_job(job.id));
    REQUIRE(f.service->update_queue({.queue = queue.id, .runnable_wait_warning = 1ms}));
    REQUIRE(f.core->process_cycle());
    CHECK(f.warnings.size() == 1);
}

TEST_CASE("A dispatch crossing after observation claims its warning in the Running transaction",
          "[jobu][telemetry][scheduler][warning][dispatch]")
{
    Fixture f;
    auto    queue      = f.queue(1, 10ms);
    auto    run        = f.scheduled(f.job(queue).id);
    bool    crossed    = false;
    f.faults->classify = [&f, &crossed](std::string_view sql) {
        if (!crossed && sql.find("ORDER BY jobu_runs.priority DESC") != std::string_view::npos) {
            crossed = true;
            f.time.advance(1us);
        }
        return std::string{classify(sql)};
    };
    f.telemetry->delayed.connect(&f.receiver, [&f, id = run.id](DelayedRun const&) {
        CHECK(f.run(id).state == RunState::Running);
        CHECK_FALSE(f.timing(id).open_epoch);
        auto transaction = jb::db::Transaction::begin(f.storage.database);
        REQUIRE(transaction);
        REQUIRE(transaction->rollback());
    });
    f.time.advance(10ms);
    REQUIRE(f.core->process_cycle());
    REQUIRE(f.warnings.size() == 1);
    CHECK(f.warnings.front().runnable_wait == 10001us);
    CHECK(f.timing(run.id).runnable_wait_us == 10001);
    CHECK(f.executor.fake.start_requests().size() == 1);
}

TEST_CASE("Observation warning failures roll back the page and emit no diagnostic",
          "[jobu][telemetry][scheduler][warning][fault]")
{
    auto    operation = GENERATE(DatabaseOperation::Prepare,
                                 DatabaseOperation::Bind,
                                 DatabaseOperation::Execute,
                                 DatabaseOperation::Finish,
                                 DatabaseOperation::Commit);
    Fixture f;
    auto    queue = f.queue(1, 10ms);
    f.job(queue);
    f.job(queue);
    REQUIRE(f.core->process_cycle());
    f.time.advance(11ms);
    auto before = storage_snapshot(f.storage.database);
    f.fault(operation == DatabaseOperation::Commit ? "connection" : "warning.claim", operation);
    REQUIRE_FALSE(f.core->process_cycle());
    require_consumed_faults(*f.faults);
    CHECK(f.warnings.empty());
    CHECK(storage_snapshot(f.storage.database) == before);
    REQUIRE(f.timing_failures.size() == 1);
    check_safe_error(f.timing_failures.front(), "db.io");
}

TEST_CASE("Observation commit uncertainty and rollback poison gate delivery and subsequent work",
          "[jobu][telemetry][scheduler][warning][fault]")
{
    auto    committed = GENERATE(false, true);
    Fixture f;
    auto    queue = f.queue(1, 10ms);
    f.job(queue);
    auto waiting = f.scheduled(f.job(queue).id);
    REQUIRE(f.core->process_cycle());
    f.time.advance(11ms);
    if (committed) {
        f.fault("connection", DatabaseOperation::Commit, DatabaseFaultPhase::AfterSuccess);
    }
    else {
        f.fault("warning.claim");
        f.fault("connection", DatabaseOperation::Rollback, DatabaseFaultPhase::Before, "db.connection_failed");
    }
    REQUIRE_FALSE(f.core->process_cycle());
    require_consumed_faults(*f.faults);
    CHECK(f.warnings.empty());
    auto calls = f.faults->calls.size();
    REQUIRE_FALSE(f.core->process_cycle());
    CHECK(f.faults->calls.size() == calls);
    f.reopen();
    CHECK(f.timing(waiting.id).delay_warned == committed);
    CHECK(f.run(waiting.id).state == RunState::Scheduled);
}

TEST_CASE("Delayed receivers can shut down or destroy telemetry after page commit",
          "[jobu][telemetry][scheduler][warning][lifetime]")
{
    auto    destroy = GENERATE(false, true);
    Fixture f;
    auto    queue = f.queue(1, 10ms);
    auto    run   = f.scheduled(f.job(queue).id);
    f.job(queue);
    f.telemetry->delayed.connect(&f.receiver, [&f, destroy](DelayedRun const& value) {
        auto transaction = jb::db::Transaction::begin(f.storage.database);
        REQUIRE(transaction);
        REQUIRE(transaction->rollback());
        if (destroy) {
            f.telemetry.reset();
        }
        else {
            f.registrar->shutdown();
            f.telemetry->request_stop();
        }
        CHECK(value.runnable_wait == 11ms); // Owning emission survives sender destruction.
    });
    f.time.advance(11ms);
    auto started = f.registrar->start();
    REQUIRE_FALSE(started);
    CHECK(started.error().code == "jobu.scheduler.stopping");
    CHECK(f.registrar->state() == SchedulerState::Shutdown);
    CHECK(f.executor.fake.start_requests().empty());
    CHECK(f.timing(run.id).delay_warned);
    CHECK(f.warnings.size() == 1);
    CHECK(jb::core::priv::EventLoopTestAccess::active_timer_count(*f.loop.loop) == 0);
}

TEST_CASE("Closed durable warning claims survive a new activation without duplicate delivery",
          "[jobu][telemetry][scheduler][warning][restart]")
{
    Fixture f;
    auto    queue = f.queue(1, 10ms);
    f.job(queue);
    auto job     = f.job(queue);
    auto waiting = f.scheduled(job.id);
    REQUIRE(f.core->process_cycle());
    f.time.advance(10001us);
    REQUIRE(f.core->process_cycle());
    REQUIRE(f.warnings.size() == 1);
    REQUIRE(f.service->suspend_job(job.id));
    f.finish();
    CHECK_FALSE(f.timing(waiting.id).open_epoch);

    // Restart only closed timing rows. Repair of lost old-epoch tails belongs to Stage 9.13.
    f.reopen();
    ExecutionTelemetry telemetry{f.storage.database, f.storage.registry, f.time, f.generator};
    Scheduler          scheduler{f.storage.database,
                                 f.storage.registry,
                                 f.cron,
                                 f.generator,
                                 f.time,
                                 f.executor,
                                 f.secrets,
                                 SchedulerOptions{.telemetry = &telemetry}};
    REQUIRE(telemetry.start());
    ManagementService management{f.storage.database,
                                 f.storage.registry,
                                 f.cron,
                                 f.generator,
                                 f.time,
                                 ManagementServiceOptions{.telemetry = &telemetry}};
    telemetry.delayed.connect(&f.receiver, [&f](DelayedRun const& value) { f.warnings.push_back(value); });
    REQUIRE(management.resume_job(job.id));
    REQUIRE(scheduler.start());
    CHECK(f.warnings.size() == 1);
    CHECK(f.timing(waiting.id).delay_warned);
    CHECK(f.timing(waiting.id).runnable_wait_us == 10001);
}

TEST_CASE("Queued warning delivery owns committed values after telemetry stops",
          "[jobu][telemetry][scheduler][warning][lifetime]")
{
    Fixture                   f;
    auto                      queue = f.queue(1, 10ms);
    auto                      run   = f.scheduled(f.job(queue).id);
    Object                    receiver;
    std::optional<DelayedRun> delivered;
    f.telemetry->delayed.connect(
        &receiver,
        [&delivered](DelayedRun const& value) { delivered = value; },
        ConnectionType::Queued);
    f.telemetry->delayed.connect(&f.receiver, [&f](DelayedRun const&) { f.telemetry->request_stop(); });
    f.time.advance(11ms);
    REQUIRE(f.core->process_cycle());
    CHECK_FALSE(delivered);
    f.time.advance(20ms);
    CHECK(f.loop.loop->process_events(EventFlag::Events) == ProcessEventsResult::Stopped);
    REQUIRE(delivered);
    CHECK(delivered->run_id == run.id);
    CHECK(delivered->runnable_wait == 11ms);
    CHECK(delivered->threshold == 10ms);
}

TEST_CASE("Delayed receiver mutations invalidate old samples and preserve an immediate rescan",
          "[jobu][telemetry][scheduler][warning][reentrancy]")
{
    Fixture f;
    auto    queue = f.queue(1, 10ms);
    f.job(queue);
    f.job(queue);
    f.job(queue, {}, OnceSchedule{.planned_at = UtcTimePoint{200s}});
    f.telemetry->delayed.connect(&f.receiver, [&f, id = queue.id](DelayedRun const&) {
        REQUIRE(f.service->update_queue({.queue = id, .runnable_wait_warning = 0ms}));
    });
    f.service->mutation_committed.connect(f.registrar.get(), [&f] { f.registrar->request_rescan(); });
    REQUIRE(f.registrar->start());
    f.time.advance(11ms);
    jb::core::priv::EventLoopTestAccess::fire_next_timer(*f.loop.loop);
    REQUIRE(f.warnings.size() == 1);
    // The future UTC wake must not replace the receiver's immediate rescan.
    CHECK(jb::core::priv::EventLoopTestAccess::active_timer_count(*f.loop.loop) == 1);
    auto rescan_deadline = jb::core::priv::EventLoopTestAccess::next_timer_deadline(*f.loop.loop);
    REQUIRE(rescan_deadline);
    CHECK(*rescan_deadline <= Clock::now());
    auto calls = f.faults->calls.size();
    jb::core::priv::EventLoopTestAccess::fire_next_timer(*f.loop.loop);
    CHECK(f.faults->calls.size() > calls);
    CHECK(f.registrar->state() == SchedulerState::Running);
    CHECK(f.timing_failures.empty());
}

TEST_CASE("Unavailable executor types never open wait or arm warning wakes",
          "[jobu][telemetry][scheduler][warning][eligibility]")
{
    Fixture f{true, true, 1, false};
    auto    queue = f.queue(1, 10ms);
    auto    run   = f.scheduled(f.job(queue, {}, ImmediateSchedule{}, JobType::Http).id);
    f.time.advance(1s);
    auto cycle = f.core->process_cycle();
    REQUIRE(cycle);
    CHECK_FALSE(cycle->next_warning);
    CHECK_FALSE(cycle->next_wake);
    CHECK_FALSE(f.timing(run.id).open_epoch);
    CHECK(f.timing(run.id).runnable_wait_us == 0);
    CHECK(f.warnings.empty());
    CHECK(f.executor.fake.start_requests().empty());
}

TEST_CASE("Future UTC work and warning deadlines share one wake without charging future wait",
          "[jobu][telemetry][scheduler][warning][wake]")
{
    Fixture f;
    auto    queue = f.queue(1, 10ms);
    f.job(queue);
    auto waiting = f.scheduled(f.job(queue).id);
    auto future  = f.scheduled(f.job(queue, {}, OnceSchedule{.planned_at = UtcTimePoint{100s + 5ms}}).id);
    REQUIRE(f.registrar->start());
    auto cycle = f.core->process_cycle();
    REQUIRE(cycle);
    CHECK(cycle->next_wake == UtcTimePoint{100s + 5ms});
    CHECK(cycle->next_warning == f.time.monotonic_now() + 10001us);
    CHECK(jb::core::priv::EventLoopTestAccess::active_timer_count(*f.loop.loop) == 1);
    f.time.advance(5ms);
    jb::core::priv::EventLoopTestAccess::fire_next_timer(*f.loop.loop);
    CHECK(f.warnings.empty());
    CHECK(f.timing(future.id).open_epoch);
    f.time.advance(5001us);
    jb::core::priv::EventLoopTestAccess::fire_next_timer(*f.loop.loop);
    REQUIRE(f.warnings.size() == 1);
    CHECK(f.warnings.front().run_id == waiting.id);
    CHECK_FALSE(f.timing(future.id).delay_warned);
    CHECK(jb::core::priv::EventLoopTestAccess::active_timer_count(*f.loop.loop) == 1);
}

TEST_CASE("Unrepresentable warning deadlines cannot wrap into immediate wakes",
          "[jobu][telemetry][scheduler][warning][clock]")
{
    Fixture f;
    auto    queue = f.queue(1, std::chrono::milliseconds{std::numeric_limits<std::int64_t>::max() / 1000});
    f.job(queue);
    auto waiting = f.scheduled(f.job(queue).id);
    auto cycle   = f.core->process_cycle();
    REQUIRE(cycle);
    CHECK_FALSE(cycle->next_warning);
    CHECK(f.timing(waiting.id).open_epoch);
    CHECK(f.warnings.empty());
    CHECK(f.timing_failures.empty());
}

TEST_CASE("An earned dispatch warning rolls back with a failed Running write",
          "[jobu][telemetry][scheduler][warning][dispatch][fault]")
{
    Fixture f;
    auto    queue      = f.queue(1, 10ms);
    auto    run        = f.scheduled(f.job(queue).id);
    auto    before     = storage_snapshot(f.storage.database);
    bool    crossed    = false;
    f.faults->classify = [&f, &crossed](std::string_view sql) {
        if (!crossed && sql.find("ORDER BY jobu_runs.priority DESC") != std::string_view::npos) {
            crossed = true;
            f.time.advance(1us);
        }
        return std::string{classify(sql)};
    };
    f.fault("dispatch.run");
    f.time.advance(10ms);
    REQUIRE_FALSE(f.core->process_cycle());
    require_consumed_faults(*f.faults);
    CHECK(f.warnings.empty());
    CHECK_FALSE(f.timing(run.id).delay_warned);
    CHECK(f.executor.fake.start_requests().empty());
    CHECK(storage_snapshot(f.storage.database) == before);
}
