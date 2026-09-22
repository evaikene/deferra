#include "scheduler.hpp"

#include "attempt_repository_priv.hpp"
#include "event_loop.hpp"
#include "job_repository_priv.hpp"
#include "query.hpp"
#include "queue_repository_priv.hpp"
#include "run_repository_priv.hpp"
#include "scheduler_core_priv.hpp"
#include "secret_provider_priv.hpp"
#include "secret_repository_priv.hpp"
#include "support/fake_attempt_executor.hpp"
#include "support/fake_cron_engine.hpp"
#include "support/fake_event_loop_backend.hpp"
#include "support/fake_time_source.hpp"
#include "support/fault_database_driver.hpp"
#include "support/recovery_fixture.hpp"
#include "support/rejecting_secret_provider.hpp"
#include "support/sequence_uuid_generator.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
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
using namespace jb::test;
using namespace std::chrono_literals;

namespace {

using Operation = DatabaseOperation;
using Phase     = DatabaseFaultPhase;
using Snapshot  = std::map<std::string, std::vector<std::vector<Value>>>;

auto at(std::int64_t seconds) -> UtcTimePoint
{
    return UtcTimePoint{std::chrono::seconds{seconds}};
}

auto storage_error(std::string code = "db.io") -> Error
{
    return {.category = ErrorCategory::Io,
            .code     = std::move(code),
            .message  = "Injected storage failure",
            .detail   = "private-backend-marker"};
}

auto successor_error() -> Error
{
    return {.category = ErrorCategory::ResourceExhausted,
            .code     = "test.cron.next_failed",
            .message  = "Configured cron next-occurrence failure"};
}

// Match logical SQL shapes, never a global query ordinal. Every injected test asserts that its named fault fired,
// so a repository rewrite cannot silently turn a rollback case into a successful control.
auto boundary(std::string_view sql) -> std::string
{
    if (sql.starts_with("SELECT value_blob FROM jobu_secrets")) {
        return "dispatch.secret";
    }
    if (sql.find("AS running_run_count") != std::string_view::npos) {
        return "startup.running_state";
    }
    if (sql.find("AS manual_sibling_count") != std::string_view::npos) {
        return "dispatch.context";
    }
    if (sql.find("WHERE jobu_runs.id = :run_id") != std::string_view::npos &&
        sql.find("AS running_attempt_count") != std::string_view::npos) {
        return "completion.context";
    }
    if (sql.starts_with("INSERT INTO jobu_attempts")) {
        return "dispatch.attempt";
    }
    if (sql.starts_with("UPDATE jobu_runs SET state = 'running'")) {
        return "dispatch.run";
    }
    if (sql.starts_with("UPDATE jobu_runs SET state = 'cancelled'")) {
        return "cancellation.run";
    }
    if (sql.starts_with("UPDATE jobu_attempts SET completed_at_us")) {
        return "completion.attempt";
    }
    if (sql.starts_with("INSERT INTO jobu_attempt_output")) {
        return "completion.output";
    }
    if (sql.starts_with("UPDATE jobu_runs SET state = 'retry_wait'")) {
        return "completion.retry";
    }
    if (sql.starts_with("UPDATE jobu_runs SET state = :state, completed_at_us")) {
        return "completion.run";
    }
    if (sql.starts_with("INSERT INTO jobu_runs")) {
        return "completion.successor";
    }
    if (sql.starts_with("UPDATE jobu_queues SET state = :next_state")) {
        return "completion.queue_suspension";
    }
    if (sql.starts_with("UPDATE jobu_jobs SET state = :next_state")) {
        return "completion.job_suspension";
    }
    return "other";
}

void execute(Database& database, std::string_view sql)
{
    Query query{database};
    REQUIRE(query.exec(sql));
}

auto snapshot(Database& database) -> Snapshot
{
    // Compare every persisted column, including owner revisions and output bytes, rather than just terminal states.
    Snapshot result;
    for (auto const* table : {"jobu_queues", "jobu_jobs", "jobu_runs", "jobu_attempts", "jobu_attempt_output"}) {
        Query query{database};
        auto  sql =
            std::string{"SELECT * FROM "} + table +
            (std::string_view{table}.starts_with("jobu_attempt") ? " ORDER BY run_id, attempt_number" : " ORDER BY id");
        REQUIRE(query.exec(sql));
        auto& rows = result[table];
        while (true) {
            auto next = query.next();
            REQUIRE(next);
            if (!*next) {
                break;
            }
            auto& row = rows.emplace_back();
            for (std::size_t i = 0; i < query.record().count(); ++i) {
                row.push_back(query.value(i));
            }
        }
    }
    return result;
}

class ObservingExecutor final : public AttemptExecutor {
public:
    explicit ObservingExecutor(std::shared_ptr<DatabaseFaultState> state)
        : _state{std::move(state)}
    {}

    FakeAttemptExecutor fake;

    auto is_available(JobType type) const noexcept -> bool override { return fake.is_available(type); }

    auto start(AttemptStartRequest request, AttemptCompletionHandler completion) -> Result<void, Error> override
    {
        // All test launches must be immediately downstream of a successfully acknowledged durable dispatch.
        REQUIRE_FALSE(_state->calls.empty());
        CHECK(_state->calls.back() ==
              DatabaseCall{.boundary = "connection", .operation = Operation::Commit, .phase = Phase::AfterSuccess});
        return fake.start(std::move(request), std::move(completion));
    }

    auto cancel(AttemptKey const& key) -> Result<void, Error> override { return fake.cancel(key); }

private:
    std::shared_ptr<DatabaseFaultState> _state;
};

enum class Scenario : std::uint8_t {
    Terminal,
    BlockingRetry,
    RescheduledRetry,
    Recurring,
    Suspension,
    RetrySuspension
};

auto retries(Scenario scenario) -> bool
{
    return scenario == Scenario::BlockingRetry || scenario == Scenario::RescheduledRetry ||
           scenario == Scenario::RetrySuspension;
}

auto suspends(Scenario scenario) -> bool
{
    return scenario == Scenario::Suspension || scenario == Scenario::RetrySuspension;
}

struct Fixture {
    std::shared_ptr<DatabaseFaultState>    faults = std::make_shared<DatabaseFaultState>();
    RecoveryFixture                        store{[this](std::unique_ptr<Driver> driver) {
        return std::make_unique<FaultDatabaseDriver>(std::move(driver), faults);
    }};
    jb::core::priv::FakeEventLoop          loop{jb::core::priv::make_fake_event_loop()};
    jb::core::priv::ScopedCurrentEventLoop current_loop{loop.loop.get()};
    FakeCronEngine                         cron;
    SequenceUuidGenerator                  generator{
        {recovery_id(100), recovery_id(101)}
    };
    RejectingSecretProvider    secrets;
    DatabaseSecretProvider     database_secrets{store.database};
    FakeTimeSource             time;
    ObservingExecutor          executor{faults};
    std::vector<Error>         failures;
    std::unique_ptr<Scheduler> scheduler;
    Queue                      queue{recovery_queue(recovery_id(1))};
    JobDefinition              job;
    RecoveryRunFixture         original;

    explicit Fixture(Scenario      scenario   = Scenario::Terminal,
                     RunState      initial    = RunState::Scheduled,
                     std::uint32_t capacity   = 1,
                     bool          references = false)
    {
        faults->classify = boundary;
        time.set_utc(at(100));
        queue.concurrency_limit = capacity;
        store.insert_queue(queue);
        job = store.make_job(recovery_id(2), queue.id);
        if (references) {
            auto payload = parse_json(R"({"command":"/bin/tool","arguments":[{"secret":"token"}]})");
            REQUIRE(payload);
            job.payload = std::move(*payload);
            SecretRepository repository{store.database};
            REQUIRE(repository.set("token", as_bytes("private-secret-sentinel"), at(100)));
        }
        job.attributes.at("output.capture").data = std::string{"always"};
        job.attributes.at("retry.mode").data =
            std::string{scenario == Scenario::BlockingRetry ? "blocking" : "reschedule"};
        if (scenario == Scenario::Recurring) {
            auto schedule = CronSchedule{.expression = "* * * * *", .timezone = "UTC"};
            cron.set_occurrences(schedule, {at(200), at(300)});
            job.schedule = schedule;
        }
        store.insert_job(job);
        original = store.make_run(recovery_id(3), job, initial, initial == RunState::RetryWait ? 1 : 0);
        store.insert_run(original);

        // A second eligible definition witnesses whether a failed completion accidentally releases queue capacity.
        auto waiting_job = store.make_job(recovery_id(4), queue.id);
        store.insert_job(waiting_job);
        auto waiting         = store.make_run(recovery_id(5), waiting_job);
        waiting.run.priority = -1;
        store.insert_run(waiting);
        executor.fake.set_available(JobType::Cli, true);
        auto& provider = references ? static_cast<SecretProvider&>(database_secrets) : secrets;
        scheduler =
            std::make_unique<Scheduler>(store.database, store.registry, cron, generator, time, executor, provider);
        scheduler->failed.connect(scheduler.get(), [this](Error const& error) { failures.push_back(error); });
    }

    void arm(DatabaseCall call, std::string code = "db.io")
    {
        faults->faults.push_back({.at = std::move(call), .error = storage_error(std::move(code))});
    }

    auto insert_future_recurring_run() -> RecoveryRunFixture
    {
        auto       recurring_job = store.make_job(recovery_id(6), queue.id);
        auto const schedule      = CronSchedule{.expression = "* * * * *", .timezone = "UTC"};
        recurring_job.schedule   = schedule;
        cron.set_occurrences(schedule, {at(200), at(300)});
        store.insert_job(recurring_job);

        // Keep cancellation's target pending while startup dispatches an unrelated attempt.
        auto pending            = store.make_run(recovery_id(7), recurring_job);
        pending.run.planned_at  = at(200);
        pending.run.runnable_at = at(200);
        store.insert_run(pending);
        return pending;
    }

    auto start_completion(Scenario scenario) -> AttemptKey
    {
        REQUIRE(scheduler->start());
        REQUIRE(executor.fake.start_requests().size() == 1U);
        auto key = executor.fake.pending_keys().front();
        REQUIRE(key.run_id == original.run.id);

        // Model an admitted suspension while the attempt is still running. Only completion may drain these owners.
        if (suspends(scenario)) {
            execute(store.database, "UPDATE jobu_queues SET state = 'suspending'");
            execute(store.database, "UPDATE jobu_jobs SET state = 'suspending', revision = revision + 1");
        }
        time.set_utc(at(110));
        return key;
    }

    void require_failure(std::string_view code = "db.io") const
    {
        REQUIRE(failures.size() == 1U);
        CHECK(failures.front().code == code);
        CHECK(failures.front().detail.find("private-backend-marker") == std::string::npos);
        CHECK(failures.front().message.find("private-backend-marker") == std::string::npos);
        CHECK(scheduler->state() == SchedulerState::Failed);
        CHECK(scheduler->failure() == failures.front());
        REQUIRE_FALSE(faults->faults.empty());
        for (auto const& fault : faults->faults) {
            CHECK(fault.fired);
        }
    }

    void require_closed_gate()
    {
        auto const calls  = faults->calls.size();
        auto const starts = executor.fake.start_requests().size();
        scheduler->request_rescan();
        auto restarted = scheduler->start();
        REQUIRE_FALSE(restarted);
        CHECK(restarted.error().code == "jobu.scheduler.stopping");
        CHECK(loop.loop->process_events(EventFlag::All) == ProcessEventsResult::Stopped);
        CHECK(faults->calls.size() == calls);
        CHECK(executor.fake.start_requests().size() == starts);
        CHECK(failures.size() == 1U);
    }

    void reopen()
    {
        scheduler.reset();
        store.reopen();
    }
};

auto completion(AttemptKey key, bool retry = false) -> AttemptCompletion
{
    return {
        .key                 = key,
        .outcome             = retry ? AttemptOutcome::Failed : AttemptOutcome::Succeeded,
        .failure_disposition = retry ? std::optional{FailureDisposition::Retryable}
            : std::nullopt,
        .result              = {.data = JsonValue::Object{{"observed", {.data = true}}}},
        .output              = jb::jobu::AttemptOutput{
                                                     .primary =
                AttemptOutputChannel{.bytes = {std::byte{0}, std::byte{0xff}}, .total_bytes = 3, .truncated = true},
                                                     .diagnostic = AttemptOutputChannel{.bytes = {std::byte{0x41}}, .total_bytes = 1}}
    };
}

auto dispatch_faults() -> std::vector<DatabaseCall>
{
    return {
        {.boundary = "connection", .operation = Operation::Begin},
        {.boundary = "dispatch.context", .operation = Operation::Prepare},
        {.boundary = "dispatch.context", .operation = Operation::Execute},
        {.boundary = "dispatch.context", .operation = Operation::Fetch},
        {.boundary = "dispatch.context", .operation = Operation::Finish},
        {.boundary = "dispatch.attempt", .operation = Operation::Prepare},
        {.boundary = "dispatch.attempt", .operation = Operation::Bind},
        {.boundary = "dispatch.attempt", .operation = Operation::Execute},
        {.boundary = "dispatch.attempt", .operation = Operation::Execute, .phase = Phase::AfterSuccess},
        {.boundary = "dispatch.run", .operation = Operation::Prepare},
        {.boundary = "dispatch.run", .operation = Operation::Execute},
        {.boundary = "dispatch.run", .operation = Operation::Execute, .phase = Phase::AfterSuccess},
        {.boundary = "connection", .operation = Operation::Commit}
    };
}

auto completion_faults(Scenario scenario) -> std::vector<DatabaseCall>
{
    auto result = std::vector<DatabaseCall>{
        {.boundary = "connection",         .operation = Operation::Begin  },
        {.boundary = "completion.context", .operation = Operation::Prepare},
        {.boundary = "completion.context", .operation = Operation::Execute},
        {.boundary = "completion.context", .operation = Operation::Fetch  },
        {.boundary = "completion.context", .operation = Operation::Finish },
        {.boundary = "connection",         .operation = Operation::Commit }
    };
    auto writes = std::vector<std::string>{"completion.attempt",
                                           "completion.output",
                                           retries(scenario) ? "completion.retry" : "completion.run"};
    if (scenario == Scenario::Recurring) {
        writes.emplace_back("completion.successor");
    }
    if (suspends(scenario)) {
        writes.emplace_back("completion.queue_suspension");
        writes.emplace_back("completion.job_suspension");
    }
    for (auto const& write : writes) {
        result.push_back({.boundary = write, .operation = Operation::Prepare});
        result.push_back({.boundary = write, .operation = Operation::Bind});
        result.push_back({.boundary = write, .operation = Operation::Execute});
        result.push_back({.boundary = write, .operation = Operation::Execute, .phase = Phase::AfterSuccess});
    }
    return result;
}

void require_committed_completion(Fixture& fixture, AttemptKey key, Scenario scenario)
{
    auto&             database = fixture.store.database;
    RunRepository     runs{database, fixture.store.registry};
    AttemptRepository attempts{database};
    auto              run = runs.find_by_id(key.run_id);
    REQUIRE(run);
    REQUIRE(run->has_value());
    CHECK(run->value().state == (retries(scenario) ? RunState::RetryWait : RunState::Succeeded));
    CHECK(run->value().started_at == at(100));
    CHECK(run->value().completed_at == (retries(scenario) ? std::nullopt : std::optional{at(110)}));
    CHECK(run->value().result == (retries(scenario) ? std::nullopt : std::optional{completion(key).result}));
    auto history = attempts.list_for_run(key.run_id, 10);
    REQUIRE(history);
    REQUIRE(history->size() == 1U);
    CHECK(history->front().state == AttemptState::Completed);
    CHECK(history->front().outcome == (retries(scenario) ? AttemptOutcome::Failed : AttemptOutcome::Succeeded));
    CHECK(history->front().completed_at == at(110));
    auto output = attempts.find_output(key.run_id, key.attempt_number);
    REQUIRE(output);
    REQUIRE(output->has_value());
    CHECK(output->value().stdout_bytes == std::optional{
                                              ByteBuffer{std::byte{0}, std::byte{0xff}}
    });
    CHECK(output->value().stderr_bytes == std::optional{ByteBuffer{std::byte{0x41}}});
    CHECK(output->value().stdout_truncated);
    CHECK_FALSE(output->value().stderr_truncated);
    CHECK_FALSE(output->value().capture_lost);

    auto successor = runs.find_schedule_owned(fixture.job.id);
    REQUIRE(successor);
    if (scenario == Scenario::Recurring) {
        REQUIRE(successor->has_value());
        CHECK(successor->value().id == recovery_id(100));
        CHECK(successor->value().state == RunState::Scheduled);
        CHECK(successor->value().planned_at == at(200));
    }
    else if (retries(scenario)) {
        REQUIRE(successor->has_value());
        CHECK(successor->value().id == key.run_id);
    }
    else {
        CHECK_FALSE(successor->has_value());
    }

    if (suspends(scenario)) {
        QueueRepository queues{database, fixture.store.registry};
        JobRepository   jobs{database, fixture.store.registry};
        auto            queue = queues.find_by_id(fixture.queue.id, false);
        auto            job   = jobs.find_by_id(fixture.job.id, false);
        REQUIRE(queue);
        REQUIRE(job);
        REQUIRE(queue->has_value());
        REQUIRE(job->has_value());
        CHECK(queue->value().state == QueueState::Suspended);
        CHECK(queue->value().updated_at == at(110));
        CHECK(job->value().state == JobState::Suspended);
        CHECK(job->value().revision == fixture.job.revision + 2);
        CHECK(job->value().updated_at == at(110));
    }
}

} // namespace

TEST_CASE("Scheduler startup storage errors are sanitized without a fatal transition",
          "[jobu][scheduler][fault][startup][sqlite]")
{
    for (auto const operation : {Operation::Prepare, Operation::Execute, Operation::Fetch}) {
        for (auto const category : {ErrorCategory::Io, ErrorCategory::Internal}) {
            CAPTURE(operation, category);
            Fixture     fixture;
            auto const  before = snapshot(fixture.store.database);
            auto const  timers = jb::core::priv::EventLoopTestAccess::active_timer_count(*fixture.loop.loop);
            auto const* code   = category == ErrorCategory::Io ? "db.io" : "db.corrupt";

            // Fail the running-state preflight with private text in both human-readable fields.
            // Even corruption keeps the existing startup contract: return to the owner before entering Running.
            fixture.faults->faults.push_back({
                .at    = {.boundary = "startup.running_state", .operation = operation},
                .error = {.category = category,
                          .code     = code,
                          .message  = "private-startup-message",
                          .detail   = "private-startup-detail"}
            });
            auto started = fixture.scheduler->start();
            REQUIRE_FALSE(started);
            REQUIRE(fixture.faults->faults.front().fired);
            CHECK(started.error().category == category);
            CHECK(started.error().code == code);
            CHECK(started.error().message.find("private-startup") == std::string::npos);
            CHECK(started.error().detail.find("private-startup") == std::string::npos);
            CHECK(fixture.scheduler->state() == SchedulerState::Stopped);
            CHECK_FALSE(fixture.scheduler->failure());
            CHECK(fixture.failures.empty());
            CHECK(fixture.executor.fake.start_requests().empty());
            CHECK(jb::core::priv::EventLoopTestAccess::active_timer_count(*fixture.loop.loop) == timers);
            CHECK(snapshot(fixture.store.database) == before);

            // The synthetic fault is consumed and the real database remains healthy. No terminal gate was latched.
            REQUIRE(fixture.scheduler->start());
            CHECK(fixture.scheduler->state() == SchedulerState::Running);
            CHECK(fixture.executor.fake.start_requests().size() == 1U);
            CHECK(fixture.failures.empty());
        }
    }
}

TEST_CASE("Dispatch transaction faults never launch and restore the full SQLite snapshot",
          "[jobu][scheduler][fault][sqlite]")
{
    for (auto initial : {RunState::Scheduled, RunState::RetryWait}) {
        for (auto const& fault : dispatch_faults()) {
            DYNAMIC_SECTION("initial " << static_cast<int>(initial) << ' ' << fault.boundary << ' '
                                       << static_cast<int>(fault.operation) << ' ' << static_cast<int>(fault.phase))
            {
                Fixture fixture{Scenario::Terminal, initial};
                auto    before = snapshot(fixture.store.database);
                fixture.arm(fault);
                auto started = fixture.scheduler->start();
                REQUIRE_FALSE(started);
                fixture.require_failure();
                CHECK(fixture.executor.fake.start_requests().empty());
                fixture.require_closed_gate();
                fixture.reopen();
                CHECK(snapshot(fixture.store.database) == before);
                fixture.store.require_run(fixture.original);
            }
        }
    }
}

TEST_CASE("Acknowledged dispatch commits the first or retry attempt before launch", "[jobu][scheduler][fault][sqlite]")
{
    for (auto initial : {RunState::Scheduled, RunState::RetryWait}) {
        Fixture fixture{Scenario::Terminal, initial};
        REQUIRE(fixture.scheduler->start());
        REQUIRE(fixture.executor.fake.start_requests().size() == 1U);
        auto const key      = fixture.executor.fake.pending_keys().front();
        auto       expected = fixture.original;
        expected.run.state  = RunState::Running;
        if (!expected.run.started_at) {
            expected.run.started_at = at(100);
        }
        expected.attempts.push_back({
            .attempt = {.run_id         = key.run_id,
                        .attempt_number = key.attempt_number,
                        .due_at         = expected.run.runnable_at,
                        .started_at     = at(100),
                        .state          = AttemptState::Running}
        });
        fixture.reopen();
        fixture.store.require_run(expected);
    }
}

TEST_CASE("Completion transaction faults roll back output retry recurrence and suspension atomically",
          "[jobu][scheduler][fault][sqlite]")
{
    for (auto scenario : {Scenario::Terminal,
                          Scenario::BlockingRetry,
                          Scenario::RescheduledRetry,
                          Scenario::Recurring,
                          Scenario::Suspension,
                          Scenario::RetrySuspension}) {
        for (auto const& fault : completion_faults(scenario)) {
            DYNAMIC_SECTION("scenario " << static_cast<int>(scenario) << ' ' << fault.boundary << ' '
                                        << static_cast<int>(fault.operation) << ' ' << static_cast<int>(fault.phase))
            {
                Fixture fixture{scenario};
                auto    key    = fixture.start_completion(scenario);
                auto    before = snapshot(fixture.store.database);
                fixture.arm(fault);
                REQUIRE(fixture.executor.fake.complete(key, completion(key, retries(scenario))));
                fixture.require_failure();
                fixture.require_closed_gate();
                CHECK(fixture.executor.fake.start_requests().size() == 1U);
                fixture.reopen();
                CHECK(snapshot(fixture.store.database) == before);
            }
        }
    }
}

TEST_CASE("Completion controls persist the entire intended transition", "[jobu][scheduler][fault][sqlite]")
{
    for (auto scenario : {Scenario::Terminal,
                          Scenario::BlockingRetry,
                          Scenario::RescheduledRetry,
                          Scenario::Recurring,
                          Scenario::Suspension,
                          Scenario::RetrySuspension}) {
        DYNAMIC_SECTION("scenario " << static_cast<int>(scenario))
        {
            Fixture fixture{scenario};
            auto    key = fixture.start_completion(scenario);
            REQUIRE(fixture.executor.fake.complete(key, completion(key, retries(scenario))));
            CHECK(fixture.failures.empty());
            fixture.reopen();
            require_committed_completion(fixture, key, scenario);
        }
    }
}

TEST_CASE("Lost dispatch commit acknowledgement leaves running rows without an external launch",
          "[jobu][scheduler][fault][sqlite]")
{
    Fixture fixture;
    fixture.arm({.boundary = "connection", .operation = Operation::Commit, .phase = Phase::AfterSuccess});
    REQUIRE_FALSE(fixture.scheduler->start());
    fixture.require_failure();
    CHECK(fixture.executor.fake.start_requests().empty());
    fixture.require_closed_gate();
    fixture.reopen();
    auto expected           = fixture.original;
    expected.run.state      = RunState::Running;
    expected.run.started_at = at(100);
    expected.attempts.push_back({
        .attempt = {.run_id         = expected.run.id,
                    .attempt_number = 1,
                    .due_at         = expected.run.runnable_at,
                    .started_at     = at(100),
                    .state          = AttemptState::Running}
    });
    fixture.store.require_run(expected);
}

TEST_CASE("Lost completion commit acknowledgement fails closed but preserves committed SQLite state",
          "[jobu][scheduler][fault][sqlite]")
{
    for (auto scenario : {Scenario::Terminal,
                          Scenario::BlockingRetry,
                          Scenario::RescheduledRetry,
                          Scenario::Recurring,
                          Scenario::Suspension,
                          Scenario::RetrySuspension}) {
        DYNAMIC_SECTION("scenario " << static_cast<int>(scenario))
        {
            Fixture fixture{scenario};
            auto    key = fixture.start_completion(scenario);
            fixture.arm({.boundary = "connection", .operation = Operation::Commit, .phase = Phase::AfterSuccess});
            REQUIRE(fixture.executor.fake.complete(key, completion(key, retries(scenario))));
            fixture.require_failure();
            fixture.require_closed_gate();
            CHECK(fixture.executor.fake.start_requests().size() == 1U);
            fixture.reopen();
            require_committed_completion(fixture, key, scenario);
        }
    }
}

TEST_CASE("First completion failure suppresses reentrant and queued callbacks before database access",
          "[jobu][scheduler][fault][sqlite]")
{
    for (bool reentrant : {false, true}) {
        DYNAMIC_SECTION("reentrant " << reentrant)
        {
            Fixture fixture{Scenario::Terminal, RunState::Scheduled, 2};
            auto    waiting_job = fixture.store.make_job(recovery_id(6), fixture.queue.id);
            fixture.store.insert_job(waiting_job);
            fixture.store.insert_run(fixture.store.make_run(recovery_id(7), waiting_job));
            REQUIRE(fixture.scheduler->start());
            auto keys = fixture.executor.fake.pending_keys();
            REQUIRE(keys.size() == 2U);
            auto before         = snapshot(fixture.store.database);
            auto deliver_second = [&] {
                auto calls = fixture.faults->calls.size();
                REQUIRE(fixture.executor.fake.complete(keys[1], completion(keys[1])));
                CHECK(fixture.faults->calls.size() == calls);
            };
            if (reentrant) {
                fixture.scheduler->failed.connect(fixture.scheduler.get(), [&](Error const&) { deliver_second(); });
            }
            else {
                REQUIRE(fixture.loop.loop->post(deliver_second));
            }

            fixture.arm({.boundary = "completion.output", .operation = Operation::Execute});
            REQUIRE(fixture.executor.fake.complete(keys[0], completion(keys[0])));
            fixture.require_failure();
            fixture.require_closed_gate();
            CHECK(fixture.executor.fake.pending_keys().empty());
            CHECK(fixture.executor.fake.start_requests().size() == 2U);
            fixture.reopen();
            CHECK(snapshot(fixture.store.database) == before);
        }
    }
}

TEST_CASE("Scheduler rollback failure preserves the first error and poisons the connection",
          "[jobu][scheduler][fault][sqlite]")
{
    for (bool completing : {false, true}) {
        DYNAMIC_SECTION("completing " << completing)
        {
            Fixture fixture;
            auto    key = AttemptKey{};
            if (completing) {
                key = fixture.start_completion(Scenario::Terminal);
            }
            auto before = snapshot(fixture.store.database);
            fixture.arm(
                {.boundary = completing ? "completion.output" : "dispatch.run", .operation = Operation::Execute});
            fixture.arm({.boundary = "connection", .operation = Operation::Rollback}, "db.rollback_failed");
            if (completing) {
                REQUIRE(fixture.executor.fake.complete(key, completion(key)));
            }
            else {
                REQUIRE_FALSE(fixture.scheduler->start());
            }
            fixture.require_failure();
            fixture.require_closed_gate();

            // Prove the generic connection rejects reuse without calling the wrapped backend; close then releases
            // SQLite's still-open transaction, so the reopened database can be compared with its pre-fault snapshot.
            auto calls           = fixture.faults->calls.size();
            auto attempted_reuse = fixture.store.database.transaction();
            REQUIRE_FALSE(attempted_reuse);
            CHECK(attempted_reuse.error().code == "db.connection_failed");
            CHECK(fixture.faults->calls.size() == calls);
            fixture.reopen();
            CHECK(snapshot(fixture.store.database) == before);
        }
    }
}

TEST_CASE("Scheduler cancellation promotes failed rollback before notifying retained completions",
          "[jobu][scheduler][fault][cancellation][sqlite]")
{
    Fixture    fixture;
    auto const pending    = fixture.insert_future_recurring_run();
    auto const key        = fixture.start_completion(Scenario::Terminal);
    auto const before     = snapshot(fixture.store.database);
    auto const cron_calls = fixture.cron.next_calls().size();

    // Successor generation is an ordinary operation failure until transaction cleanup also fails.
    fixture.cron.set_next_error(successor_error());
    fixture.faults->faults.push_back({
        .at    = {.boundary = "connection", .operation = Operation::Rollback, .phase = Phase::Before},
        .error = {.category = ErrorCategory::Io,
                  .code     = "db.rollback_failed",
                  .message  = "private-rollback-message",
                  .detail   = "private-rollback-detail"}
    });

    bool cancellation_returned = false;
    bool completion_delivered  = false;
    fixture.scheduler->failed.connect(fixture.scheduler.get(), [&](Error const& error) {
        CHECK_FALSE(cancellation_returned);
        CHECK(fixture.store.database.is_poisoned());
        CHECK(fixture.scheduler->state() == SchedulerState::Failed);
        CHECK(fixture.scheduler->failure() == error);
        CHECK(error.code == "db.rollback_failed");

        // Failure state must already be visible, and a completion delivered inside notification must be inert.
        auto const calls = fixture.faults->calls.size();
        REQUIRE(fixture.executor.fake.complete(key, completion(key)));
        completion_delivered = true;
        CHECK(fixture.faults->calls.size() == calls);
        CHECK(fixture.scheduler->failure() == error);
        CHECK(fixture.failures.size() == 1U);
    });

    auto cancelled        = fixture.scheduler->cancel_run(pending.run.id);
    cancellation_returned = true;
    REQUIRE_FALSE(cancelled);
    REQUIRE(fixture.cron.next_calls().size() == cron_calls + 1);
    CHECK(fixture.cron.next_calls().back().exclusive_lower_bound == pending.run.planned_at);
    REQUIRE(fixture.faults->faults.front().fired);
    REQUIRE(fixture.store.database.is_poisoned());
    CHECK(cancelled.error().code == "db.rollback_failed");
    CHECK(fixture.scheduler->state() == SchedulerState::Failed);
    CHECK(completion_delivered);
    fixture.require_failure("db.rollback_failed");
    CHECK(cancelled.error() == fixture.failures.front());
    CHECK(cancelled.error().message.find("private-rollback") == std::string::npos);
    CHECK(cancelled.error().detail.find("private-rollback") == std::string::npos);

    auto const calls    = fixture.faults->calls.size();
    auto       repeated = fixture.scheduler->cancel_run(pending.run.id);
    REQUIRE_FALSE(repeated);
    CHECK(repeated.error().code == "jobu.scheduler.stopping");
    CHECK(fixture.faults->calls.size() == calls);
    fixture.require_closed_gate();
    CHECK(fixture.executor.fake.start_requests().size() == 1U);

    // Release scheduler dependencies before closing the poisoned connection. SQLite close rolls back the uncommitted
    // cancellation; the full snapshot also proves no successor or completion of the unrelated active run persisted.
    fixture.reopen();
    CHECK(snapshot(fixture.store.database) == before);
    fixture.store.require_run(pending);
}

TEST_CASE("Scheduler cancellation retains ordinary successor errors when rollback succeeds",
          "[jobu][scheduler][fault][cancellation][sqlite]")
{
    Fixture    fixture;
    auto const pending = fixture.insert_future_recurring_run();
    (void)fixture.start_completion(Scenario::Terminal);
    auto const before = snapshot(fixture.store.database);
    fixture.cron.set_next_error(successor_error());

    auto cancelled = fixture.scheduler->cancel_run(pending.run.id);
    REQUIRE_FALSE(cancelled);
    CHECK(cancelled.error() == successor_error());
    CHECK_FALSE(fixture.store.database.is_poisoned());
    CHECK(fixture.scheduler->state() == SchedulerState::Running);
    CHECK_FALSE(fixture.scheduler->failure());
    CHECK(fixture.failures.empty());
    CHECK(snapshot(fixture.store.database) == before);
    fixture.store.require_run(pending);

    // Successful rollback leaves the same scheduler and pending run usable for a caller retry.
    fixture.cron.set_next_error(std::nullopt);
    auto retried = fixture.scheduler->cancel_run(pending.run.id);
    REQUIRE(retried);
    CHECK(retried->disposition == CancelDisposition::Completed);
    CHECK(retried->run.state == RunState::Cancelled);
    RunRepository runs{fixture.store.database, fixture.store.registry};
    auto          successor = runs.find_schedule_owned(pending.run.job_id);
    REQUIRE(successor);
    REQUIRE(successor->has_value());
    CHECK(successor->value().id == recovery_id(100));
    CHECK(successor->value().planned_at == at(300));
    CHECK(successor->value().state == RunState::Scheduled);
    CHECK(fixture.scheduler->state() == SchedulerState::Running);
    CHECK(fixture.failures.empty());
}

TEST_CASE("Scheduler cancellation preserves its first fatal error when rollback also fails",
          "[jobu][scheduler][fault][cancellation][sqlite]")
{
    Fixture    fixture;
    auto const pending = fixture.insert_future_recurring_run();
    (void)fixture.start_completion(Scenario::Terminal);
    auto const before = snapshot(fixture.store.database);

    // Fail after the cancellation update reaches SQLite, so durable restoration requires rollback on close.
    fixture.arm({.boundary = "cancellation.run", .operation = Operation::Execute, .phase = Phase::AfterSuccess});
    fixture.arm({.boundary = "connection", .operation = Operation::Rollback, .phase = Phase::Before},
                "db.rollback_failed");
    auto cancelled = fixture.scheduler->cancel_run(pending.run.id);
    REQUIRE_FALSE(cancelled);
    CHECK(fixture.store.database.is_poisoned());
    fixture.require_failure("db.io");
    CHECK(cancelled.error() == fixture.failures.front());
    CHECK(cancelled.error().message != "Injected storage failure");

    auto const calls    = fixture.faults->calls.size();
    auto       repeated = fixture.scheduler->cancel_run(pending.run.id);
    REQUIRE_FALSE(repeated);
    CHECK(repeated.error().code == "jobu.scheduler.stopping");
    CHECK(fixture.faults->calls.size() == calls);
    fixture.require_closed_gate();

    fixture.reopen();
    CHECK(snapshot(fixture.store.database) == before);
}

TEST_CASE("Scheduler core latches cancellation cleanup failure without asynchronous notification",
          "[jobu][scheduler][core][fault][cancellation][sqlite]")
{
    for (bool fatal_cleanup_error : {true, false}) {
        DYNAMIC_SECTION("fatal cleanup error " << fatal_cleanup_error)
        {
            Fixture    fixture;
            auto const pending = fixture.insert_future_recurring_run();
            fixture.scheduler.reset();
            Snapshot before;

            // Own the core directly to prove it closes completion acceptance without help from the public adapter.
            // Destroy it before reopening the connection, just as the public fixture releases its scheduler.
            {
                SchedulerCore core{
                    fixture.store.database,
                    fixture.store.registry,
                    fixture.cron,
                    fixture.generator,
                    fixture.time,
                    fixture.executor,
                    fixture.secrets,
                    {},
                    {.failure_reported = [&](Error const& error) { fixture.failures.push_back(error); }}};
                REQUIRE(core.process_cycle());
                REQUIRE(fixture.executor.fake.start_requests().size() == 1U);
                auto const key        = fixture.executor.fake.pending_keys().front();
                before                = snapshot(fixture.store.database);
                auto const cron_calls = fixture.cron.next_calls().size();
                fixture.cron.set_next_error(successor_error());
                fixture.arm({.boundary = "connection", .operation = Operation::Rollback, .phase = Phase::Before},
                            fatal_cleanup_error ? "db.rollback_failed" : "test.rollback.failed");

                auto cancelled = core.cancel_run(pending.run.id);
                REQUIRE_FALSE(cancelled);
                CHECK(fixture.cron.next_calls().size() == cron_calls + 1);
                REQUIRE(fixture.faults->faults.front().fired);
                CHECK(fixture.store.database.is_poisoned());
                CHECK(cancelled.error().code == (fatal_cleanup_error ? "db.rollback_failed" : "db.connection_failed"));
                if (!fatal_cleanup_error) {
                    CHECK(cancelled.error().category == ErrorCategory::Internal);
                    CHECK(cancelled.error().message.find("Injected") == std::string::npos);
                    CHECK(cancelled.error().detail.empty());
                }

                // A retained completion must not replace the latched error or invoke the asynchronous failure path.
                auto const calls = fixture.faults->calls.size();
                REQUIRE(fixture.executor.fake.complete(key, completion(key)));
                auto cycle = core.process_cycle();
                REQUIRE_FALSE(cycle);
                CHECK(cycle.error() == cancelled.error());
                auto repeated = core.cancel_run(pending.run.id);
                REQUIRE_FALSE(repeated);
                CHECK(repeated.error() == cancelled.error());
                CHECK(fixture.faults->calls.size() == calls);
                CHECK(fixture.executor.fake.start_requests().size() == 1U);
                CHECK(fixture.failures.empty());
            }

            fixture.reopen();
            CHECK(snapshot(fixture.store.database) == before);
            fixture.store.require_run(pending);
        }
    }
}

TEST_CASE("Secret dispatch faults preserve the durable snapshot and close scheduler admission",
          "[jobu][scheduler][fault][secret][sqlite]")
{
    auto faults = dispatch_faults();
    for (auto operation :
         {Operation::Prepare, Operation::Bind, Operation::Execute, Operation::Fetch, Operation::Finish}) {
        faults.push_back({.boundary = "dispatch.secret", .operation = operation});
    }
    for (auto initial : {RunState::Scheduled, RunState::RetryWait}) {
        for (auto const& fault : faults) {
            DYNAMIC_SECTION(static_cast<int>(initial)
                            << ' ' << fault.boundary << ' ' << static_cast<int>(fault.operation) << ' '
                            << static_cast<int>(fault.phase))
            {
                Fixture fixture{Scenario::Terminal, initial, 1, true};
                auto    before = snapshot(fixture.store.database);
                fixture.arm(fault);
                auto started = fixture.scheduler->start();
                REQUIRE_FALSE(started);
                fixture.require_failure();
                CHECK(fixture.failures.front().detail == "operation=dispatch reason=state_operation_failed");
                CHECK(fixture.executor.fake.start_requests().empty());
                fixture.require_closed_gate();
                fixture.reopen();
                CHECK(snapshot(fixture.store.database) == before);
            }
        }
    }
}

TEST_CASE("Secret preparation cannot bypass commit acknowledgement or poisoned rollback",
          "[jobu][scheduler][fault][secret][sqlite]")
{
    for (bool missing : {false, true}) {
        for (bool lost_acknowledgement : {false, true}) {
            DYNAMIC_SECTION(missing << ' ' << lost_acknowledgement)
            {
                Fixture fixture{Scenario::Terminal, RunState::Scheduled, 1, true};
                if (missing) {
                    execute(fixture.store.database, "DELETE FROM jobu_secrets");
                }
                auto before = snapshot(fixture.store.database);
                if (lost_acknowledgement) {
                    fixture.arm(
                        {.boundary = "connection", .operation = Operation::Commit, .phase = Phase::AfterSuccess});
                }
                else {
                    fixture.arm({.boundary = "dispatch.attempt", .operation = Operation::Execute});
                    fixture.arm({.boundary = "connection", .operation = Operation::Rollback}, "db.rollback_failed");
                }
                auto started = fixture.scheduler->start();
                REQUIRE_FALSE(started);
                fixture.require_failure();
                CHECK(fixture.executor.fake.start_requests().empty());
                if (!lost_acknowledgement) {
                    CHECK(fixture.store.database.is_poisoned());
                }
                fixture.require_closed_gate();
                fixture.reopen();
                if (!lost_acknowledgement) {
                    CHECK(snapshot(fixture.store.database) == before);
                }
                else {
                    // SQLite committed even though its acknowledgement was lost. Recovery owns these Running rows;
                    // neither an executor launch nor a synthetic preparation completion may cross that uncertainty.
                    RunRepository runs{fixture.store.database, fixture.store.registry};
                    auto          run = runs.find_by_id(fixture.original.run.id);
                    REQUIRE(run);
                    REQUIRE(run->has_value());
                    CHECK(run->value().state == RunState::Running);
                    AttemptRepository attempts{fixture.store.database};
                    auto              attempt = attempts.find(fixture.original.run.id, 1);
                    REQUIRE(attempt);
                    REQUIRE(attempt->has_value());
                    CHECK(attempt->value().state == AttemptState::Running);
                }
            }
        }
    }
}

TEST_CASE("Synthetic preparation completion faults leave the committed attempt for recovery",
          "[jobu][scheduler][fault][secret][sqlite]")
{
    for (auto scenario : {Scenario::Terminal, Scenario::Recurring}) {
        for (auto const& fault : completion_faults(scenario)) {
            // Preparation produced no captured output. Begin/commit are faulted in dispatch by the separate matrix;
            // this case targets the completion transaction's own reads and terminal/recurrence writes.
            if (fault.boundary == "completion.output" || fault.boundary == "connection") {
                continue;
            }
            DYNAMIC_SECTION(static_cast<int>(scenario)
                            << ' ' << fault.boundary << ' ' << static_cast<int>(fault.operation) << ' '
                            << static_cast<int>(fault.phase))
            {
                Fixture fixture{scenario, RunState::Scheduled, 1, true};
                execute(fixture.store.database, "DELETE FROM jobu_secrets");
                fixture.arm(fault);
                auto started = fixture.scheduler->start();
                REQUIRE_FALSE(started);
                fixture.require_failure();
                CHECK(fixture.executor.fake.start_requests().empty());
                fixture.require_closed_gate();
                fixture.reopen();
                RunRepository runs{fixture.store.database, fixture.store.registry};
                auto          run = runs.find_by_id(fixture.original.run.id);
                REQUIRE(run);
                REQUIRE(run->has_value());
                CHECK(run->value().state == RunState::Running);
                AttemptRepository attempts{fixture.store.database};
                auto              attempt = attempts.find(fixture.original.run.id, 1);
                REQUIRE(attempt);
                REQUIRE(attempt->has_value());
                CHECK(attempt->value().state == AttemptState::Running);
                CHECK_FALSE(attempt->value().result);
                auto successor = runs.find_by_id(recovery_id(100));
                REQUIRE(successor);
                CHECK_FALSE(successor->has_value());
            }
        }
    }
}
