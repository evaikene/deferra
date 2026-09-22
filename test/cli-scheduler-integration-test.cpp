#include "scheduler.hpp"

#include "application.hpp"
#include "attempt_executor_group.hpp"
#include "attempt_repository_priv.hpp"
#include "attribute_registry.hpp"
#include "byte_buffer.hpp"
#include "cli/cli_attempt_executor.hpp"
#include "database.hpp"
#include "event_loop_types.hpp"
#include "http/http_attempt_executor.hpp"
#include "http/system_http_client.hpp"
#include "json.hpp"
#include "management.hpp"
#include "process.hpp"
#include "run_repository_priv.hpp"
#include "sqlite/sqlite_driver.hpp"
#include "sqlite/sqlite_schema.hpp"
#include "support/fake_cron_engine.hpp"
#include "support/fake_time_source.hpp"
#include "support/http_test_server.hpp"
#include "support/rejecting_secret_provider.hpp"
#include "support/sequence_uuid_generator.hpp"
#include "support/temporary_directory.hpp"

#if defined(__linux__)
#  include "event_loop_backend_epoll_priv.hpp"
#elif defined(__APPLE__)
#  include <sys/event.h>
#endif

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fcntl.h>
#if defined(__linux__)
#  include <poll.h>
#endif
#include <sqlite3.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

using namespace jb::core;
using namespace jb::db;
using namespace jb::jobu;
using jb::jobu::cli::CliAttemptExecutor;
using jb::jobu::cli::CliAttemptExecutorOptions;
using jb::jobu::detail::AttemptRepository;
using jb::jobu::detail::RunRepository;
using jb::jobu::http::HttpAttemptExecutor;
using jb::net::http::SystemHttpClient;
using namespace jb::test;
using namespace std::chrono_literals;

namespace {

auto test_ids() -> std::vector<Uuid>
{
    auto values = std::vector<Uuid>{};
    for (auto suffix = std::uint16_t{1}; suffix <= 250; ++suffix) {
        auto storage = Uuid::Storage{};
        storage[6]   = std::byte{0x70};
        storage[8]   = std::byte{0x80};
        storage[15]  = static_cast<std::byte>(suffix);
        values.emplace_back(storage);
    }
    return values;
}

auto at_seconds(std::int64_t seconds) -> UtcTimePoint
{
    return UtcTimePoint{std::chrono::seconds{seconds}};
}

auto make_database(std::filesystem::path path) -> Database
{
    return Database{std::make_unique<jb::db::sqlite::Driver>(jb::db::sqlite::Options{
        .database_file = std::move(path),
        .busy_timeout  = 1000ms,
        .durability    = jb::db::sqlite::Durability::Normal,
    })};
}

auto json_string(std::string value) -> JsonValue
{
    return {.data = std::move(value)};
}

auto helper_payload(std::vector<std::string> arguments, std::vector<std::uint64_t> const& expected = {0}) -> JsonValue
{
    auto args = JsonValue::Array{};
    for (auto& argument : arguments) {
        args.push_back(json_string(std::move(argument)));
    }
    auto codes = JsonValue::Array{};
    for (auto code : expected) {
        codes.push_back(JsonValue{.data = code});
    }
    return {
        .data = JsonValue::Object{
                                  {"command", json_string(PROCESS_TEST_HELPER)},
                                  {"arguments", JsonValue{.data = std::move(args)}},
                                  {"expected_exit_codes", JsonValue{.data = std::move(codes)}},
                                  }
    };
}

struct CreatedJob {
    JobDefinition definition;
    JobRun        run;
};

struct StartObservation {
    AttemptKey key;
    Uuid       queue_id;
    JobType    type;
};

struct HeldCompletion {
    AttemptCompletion        value;
    AttemptCompletionHandler handler;
};

struct DurableState {
    std::string run_state;
    std::string attempt_state;
    bool        has_output;
};

/// Native read-only access follows the daemon-test probe: a second JobU Driver would contend for its ownership lock.
class DurableReader {
public:
    void open(std::filesystem::path const& path)
    {
        sqlite3*   raw{};
        auto const result = sqlite3_open_v2(path.c_str(), &raw, SQLITE_OPEN_READONLY, nullptr);
        _database.reset(raw);
        REQUIRE(result == SQLITE_OK);
    }

    void begin_snapshot() { REQUIRE(sqlite3_exec(_database.get(), "BEGIN", nullptr, nullptr, nullptr) == SQLITE_OK); }

    void end_snapshot() { REQUIRE(sqlite3_exec(_database.get(), "ROLLBACK", nullptr, nullptr, nullptr) == SQLITE_OK); }

    auto state(AttemptKey const& key) -> DurableState
    {
        constexpr auto sql =
            "SELECT r.state, a.state, o.run_id IS NOT NULL FROM jobu_runs r "
            "JOIN jobu_attempts a ON a.run_id = r.id "
            "LEFT JOIN jobu_attempt_output o ON o.run_id = a.run_id AND o.attempt_number = a.attempt_number "
            "WHERE a.run_id = ? AND a.attempt_number = ?";
        sqlite3_stmt* raw{};
        auto const    prepared  = sqlite3_prepare_v2(_database.get(), sql, -1, &raw, nullptr);
        auto          statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>{raw, sqlite3_finalize};
        REQUIRE(prepared == SQLITE_OK);
        REQUIRE(sqlite3_bind_blob(statement.get(), 1, key.run_id.bytes().data(), 16, SQLITE_TRANSIENT) == SQLITE_OK);
        REQUIRE(sqlite3_bind_int64(statement.get(), 2, static_cast<sqlite3_int64>(key.attempt_number)) == SQLITE_OK);
        REQUIRE(sqlite3_step(statement.get()) == SQLITE_ROW);
        auto result = DurableState{
            .run_state     = reinterpret_cast<char const*>(sqlite3_column_text(statement.get(), 0)),
            .attempt_state = reinterpret_cast<char const*>(sqlite3_column_text(statement.get(), 1)),
            .has_output    = sqlite3_column_int(statement.get(), 2) != 0,
        };
        REQUIRE(sqlite3_step(statement.get()) == SQLITE_DONE);
        return result;
    }

private:
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> _database{nullptr, sqlite3_close};
};

/// Records actual runner boundaries without supplying synthetic starts, output, or outcomes.
struct ExecutionProbe {
    DurableReader&                reader;
    RunRepository&                runs;
    AttemptRepository&            attempts;
    std::vector<StartObservation> starts;
    std::vector<AttemptKey>       active;
    std::vector<HeldCompletion>   held;
    bool                          hold_completions{false};
    std::size_t                   committed_completions{0};
    bool                          verify{true};

    void before_start(AttemptStartRequest const& request)
    {
        // This connection cannot see the writer's uncommitted rows. Check before calling the real executor, so a
        // fast target cannot make an after-the-fact database check look like proof of durable-before-exec ordering.
        auto const durable = reader.state(request.key);
        CHECK(durable.run_state == "running");
        CHECK(durable.attempt_state == "running");
        CHECK_FALSE(durable.has_output);
        auto run     = runs.find_by_id(request.key.run_id);
        auto attempt = attempts.find(request.key.run_id, request.key.attempt_number);
        REQUIRE(run);
        REQUIRE(run->has_value());
        REQUIRE(attempt);
        REQUIRE(attempt->has_value());
        CHECK(run->value().state == RunState::Running);
        CHECK(attempt->value().state == AttemptState::Running);
        CHECK(attempt->value().started_at == request.started_at);
        starts.push_back({.key = request.key, .queue_id = request.queue_id, .type = request.type});
    }

    void deliver(AttemptCompletion value, AttemptCompletionHandler const& handler)
    {
        auto const key             = value.key;
        auto const expected_output = value.output;
        // Accepted scheduler cancellation intentionally uses the established runner-neutral result. Other results
        // round-trip through JSON, whose parser may normalize a positive signed integer to the unsigned alternative.
        auto const expected_result = value.outcome == AttemptOutcome::Cancelled
                                       ? JsonValue{.data = JsonValue::Object{{"reason", json_string("cancelled")}}}
                                       : value.result;
        std::erase(active, key);
        if (!verify) {
            handler(std::move(value));
            return;
        }

        // Pin an independent reader to the pre-completion snapshot. The writer must commit the completion while this
        // reader still sees the running attempt and no output, then a fresh snapshot must see all selected output.
        reader.begin_snapshot();
        auto const before = reader.state(key);
        CHECK(before.run_state == "running");
        CHECK(before.attempt_state == "running");
        CHECK_FALSE(before.has_output);

        handler(std::move(value));
        auto const pinned = reader.state(key);
        CHECK(pinned.run_state == "running");
        CHECK(pinned.attempt_state == "running");
        CHECK_FALSE(pinned.has_output);
        reader.end_snapshot();
        auto const fresh = reader.state(key);
        CHECK(fresh.run_state != "running");
        CHECK(fresh.attempt_state == "completed");
        CHECK(fresh.has_output == expected_output.has_value());

        auto completed = attempts.find(key.run_id, key.attempt_number);
        REQUIRE(completed);
        REQUIRE(completed->has_value());
        CHECK(completed->value().state == AttemptState::Completed);
        REQUIRE(completed->value().result);
        auto const stored_json   = serialize_json(*completed->value().result);
        auto const expected_json = serialize_json(expected_result);
        REQUIRE(stored_json);
        REQUIRE(expected_json);
        CHECK(*stored_json == *expected_json);
        auto run = runs.find_by_id(key.run_id);
        REQUIRE(run);
        REQUIRE(run->has_value());
        CHECK(run->value().state != RunState::Running);
        auto output = attempts.find_output(key.run_id, key.attempt_number);
        REQUIRE(output);
        CHECK(output->has_value() == expected_output.has_value());
        if (expected_output && output->has_value()) {
            auto const& persisted = output->value();
            CHECK(persisted.capture_lost == expected_output->capture_lost);
            if (expected_output->primary) {
                CHECK(persisted.stdout_bytes == expected_output->primary->bytes);
                CHECK(persisted.stdout_truncated == expected_output->primary->truncated);
            }
            if (expected_output->diagnostic) {
                CHECK(persisted.stderr_bytes == expected_output->diagnostic->bytes);
                CHECK(persisted.stderr_truncated == expected_output->diagnostic->truncated);
            }
        }
        ++committed_completions;
    }

    void complete(AttemptCompletion value, AttemptCompletionHandler handler)
    {
        if (hold_completions) {
            held.push_back({.value = std::move(value), .handler = std::move(handler)});
            return;
        }
        deliver(std::move(value), handler);
    }

    void release_completions()
    {
        hold_completions = false;
        auto pending     = std::exchange(held, {});
        for (auto& completion : pending) {
            deliver(std::move(completion.value), completion.handler);
        }
    }
};

class ObservedExecutor final : public AttemptExecutor {
public:
    ObservedExecutor(std::unique_ptr<AttemptExecutor> executor, ExecutionProbe& probe)
        : _executor{std::move(executor)}
        , _probe{probe}
    {}

    auto is_available(JobType type) const noexcept -> bool override { return _executor->is_available(type); }

    auto start(AttemptStartRequest request, AttemptCompletionHandler handler) -> Result<void, Error> override
    {
        _probe.before_start(request);
        auto const key = request.key;
        auto       result =
            _executor->start(std::move(request), [this, handler = std::move(handler)](AttemptCompletion value) mutable {
                _probe.complete(std::move(value), std::move(handler));
            });
        if (result) {
            _probe.active.push_back(key);
        }
        return result;
    }

    auto cancel(AttemptKey const& key) -> Result<void, Error> override { return _executor->cancel(key); }

private:
    std::unique_ptr<AttemptExecutor> _executor;
    ExecutionProbe&                  _probe;
};

/// Owner-side nonblocking FIFO used by the existing helper's wait and group modes.
class HelperChannel {
public:
    explicit HelperChannel(std::filesystem::path path)
        : _path{std::move(path)}
    {
        REQUIRE(::mkfifo(_path.c_str(), 0600) == 0);
        _fd = ::open(_path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
        REQUIRE(_fd >= 0);
    }

    ~HelperChannel() { ::close(_fd); }

    HelperChannel(HelperChannel const&)                    = delete;
    auto operator=(HelperChannel const&) -> HelperChannel& = delete;

    auto path() const -> std::string { return _path.string(); }

    void release() const { REQUIRE(::write(_fd, "R", 1) == 1); }

    auto read_group() const -> std::optional<std::array<pid_t, 2>>
    {
        // The helper emits both PIDs in one PIPE_BUF-sized write only after both TERM dispositions are installed.
        std::array<pid_t, 2> identities{};
        auto const           count = ::read(_fd, identities.data(), sizeof(identities));
        if (count < 0 && (errno == EAGAIN || errno == EINTR)) {
            return {};
        }
        REQUIRE(count == sizeof(identities));
        return identities;
    }

private:
    std::filesystem::path _path;
    int                   _fd{-1};
};

/// A native watch observes helper exit without reaping the scheduler's child or relying on a reused numeric PID.
class TerminationWatch {
public:
    /// @throws Catch::TestFailureException when the coordinated helper cannot be watched.
    explicit TerminationWatch(pid_t pid)
        : _pid{pid}
    {
        REQUIRE(pid > 0);
#if defined(__APPLE__)
        // The helper waits for cancellation, so registration precedes exit and the owner's eventual reap.
        _fd = ::kqueue();
        REQUIRE(_fd >= 0);
        struct kevent change;
        EV_SET(&change, pid, EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT, 0, nullptr);
        int registered;
        do {
            registered = ::kevent(_fd, &change, 1, nullptr, 0, nullptr);
        } while (registered < 0 && errno == EINTR);
        if (registered < 0) {
            auto const error = errno;
            ::close(_fd);
            _fd = -1;
            FAIL("kevent registration failed with errno " << error);
        }
#else
        jb::core::priv::EpollProcessOperations operations;
        _fd = operations.open_pidfd(_pid);
        REQUIRE(_fd >= 0);
#endif
    }

    ~TerminationWatch() { ::close(_fd); }

    TerminationWatch(TerminationWatch const&)                    = delete;
    auto operator=(TerminationWatch const&) -> TerminationWatch& = delete;

    /// @throws Catch::TestFailureException when native exit observation fails.
    auto terminated() -> bool
    {
        // A one-shot kqueue event is consumed on read. Retain it while another watched group member is still alive.
        if (_terminated) {
            return true;
        }

#if defined(__APPLE__)
        struct kevent   event;
        struct timespec timeout{};
        int             ready;
        do {
            ready = ::kevent(_fd, nullptr, 0, &event, 1, &timeout);
        } while (ready < 0 && errno == EINTR);
        REQUIRE(ready >= 0);
        if (ready == 1) {
            REQUIRE(event.filter == EVFILT_PROC);
            REQUIRE(event.ident == static_cast<std::uintptr_t>(_pid));
            REQUIRE((event.fflags & NOTE_EXIT) != 0);
            _terminated = true;
        }
#else
        pollfd descriptor{.fd = _fd, .events = POLLIN, .revents = 0};
        int    ready;
        do {
            ready = ::poll(&descriptor, 1, 0);
        } while (ready < 0 && errno == EINTR);
        REQUIRE(ready >= 0);
        _terminated = ready == 1 && (descriptor.revents & POLLIN) != 0;
#endif
        return _terminated;
    }

private:
    pid_t _pid;
    int   _fd{-1};
    bool  _terminated{false};
};

struct MixedSchedulerFixture {
    explicit MixedSchedulerFixture(SchedulerOptions options = {})
        : app{0, nullptr}
        , database_file{directory.path() / "jobu.sqlite"}
        , database{make_database(database_file)}
        , generator{test_ids()}
        , runs{database, registry}
        , attempts{database}
        , probe{.reader = observer, .runs = runs, .attempts = attempts}
    {
        REQUIRE(database.open());
        REQUIRE(jb::jobu::sqlite::ensure_schema(database));
        observer.open(database_file);
        time.set_utc(at_seconds(100));
        auto created_client = SystemHttpClient::create(*app.event_loop());
        REQUIRE(created_client);
        client         = std::move(created_client).value();
        group          = std::make_unique<AttemptExecutorGroup>();
        auto cli_owner = std::make_unique<CliAttemptExecutor>(CliAttemptExecutorOptions{.allow_root = true});
        cli            = cli_owner.get();
        REQUIRE(cli->parent() == nullptr);
        REQUIRE(group->add(JobType::Cli, std::make_unique<ObservedExecutor>(std::move(cli_owner), probe)));
        REQUIRE(group->add(
            JobType::Http,
            std::make_unique<ObservedExecutor>(std::make_unique<HttpAttemptExecutor>(*client, time), probe)));
        management = std::make_unique<ManagementService>(database, registry, cron, generator, time);
        scheduler  = std::make_unique<Scheduler>(database, registry, cron, generator, time, *group, secrets, options);
    }

    ~MixedSchedulerFixture()
    {
        // Stop admission, then cancel accepted work directly through the group: Scheduler::stop does not cancel it.
        // Keep the scheduler and database alive until every retained callback has crossed its durable boundary.
        scheduler->stop();
        probe.verify = false;
        server.release_responses();
        server.release_response_segment();
        probe.release_completions();
        auto const active = probe.active;
        for (auto const& key : active) {
            (void)group->cancel(key);
        }
        auto const deadline = Clock::now() + 5s;
        while (!probe.active.empty() && Clock::now() < deadline) {
            if (app.process_events(EventFlag::All, 10) == ProcessEventsResult::Failed) {
                break;
            }
        }
        CHECK(probe.active.empty());
        // A failed watchdog must still suppress callbacks and kill children before their borrowed scheduler disappears.
        if (!probe.active.empty()) {
            group.reset();
        }
        scheduler.reset();
        group.reset();
        client.reset();
    }

    template <typename Predicate>
    void until(Predicate&& predicate)
    {
        auto const deadline = Clock::now() + 5s;
        while (!predicate() && Clock::now() < deadline) {
            REQUIRE(app.process_events(EventFlag::All, 10) != ProcessEventsResult::Failed);
        }
        REQUIRE(predicate());
    }

    void rescan()
    {
        scheduler->request_rescan();
        REQUIRE(app.process_events(EventFlag::Timers, 0) != ProcessEventsResult::Failed);
    }

    auto queue(std::string name, std::uint32_t weight = 1, std::uint32_t capacity = 1) const -> Queue
    {
        auto result =
            management->create_queue({.name = std::move(name), .weight = weight, .concurrency_limit = capacity});
        REQUIRE(result);
        return std::move(result).value();
    }

    auto create(Queue const& queue,
                JobType      type,
                JsonValue    payload,
                AttributeSet attributes = {},
                JobSchedule  schedule   = OnceSchedule{.planned_at = at_seconds(90)}) -> CreatedJob
    {
        // Keep assertion-unwind cancellation bounded even for helpers that deliberately ignore TERM.
        if (type == JobType::Cli) {
            attributes.try_emplace("cli.termination_grace", AttributeValue{.data = Duration{100ms}});
        }
        auto definition = management->create_job({.queue      = queue.id,
                                                  .type       = type,
                                                  .schedule   = std::move(schedule),
                                                  .attributes = std::move(attributes),
                                                  .payload    = std::move(payload)});
        REQUIRE(definition);
        auto run = runs.find_schedule_owned(definition->id);
        REQUIRE(run);
        REQUIRE(run->has_value());
        return {.definition = std::move(definition).value(), .run = std::move(run->value())};
    }

    auto http_job(Queue const& queue, std::string path) -> CreatedJob
    {
        return create(queue,
                      JobType::Http,
                      JsonValue{
                          .data = JsonValue::Object{
                                                    {"url", json_string(server.url(std::move(path)))},
                                                    }
        });
    }

    auto wait_job(Queue const& queue) -> CreatedJob
    {
        channels.push_back(
            std::make_unique<HelperChannel>(directory.path() / ("wait-" + std::to_string(channels.size()))));
        return create(queue, JobType::Cli, helper_payload({"wait", channels.back()->path()}));
    }

    void release_helpers()
    {
        for (auto const& channel : channels) {
            channel->release();
        }
    }

    auto run(Uuid const& id) -> JobRun
    {
        auto result = runs.find_by_id(id);
        REQUIRE(result);
        REQUIRE(result->has_value());
        return std::move(result->value());
    }

    auto attempt(Uuid const& id, AttemptNumber number = 1) -> JobAttempt
    {
        auto result = attempts.find(id, number);
        REQUIRE(result);
        REQUIRE(result->has_value());
        return std::move(result->value());
    }

    auto has_state(CreatedJob const& job, RunState state) -> bool { return run(job.run.id).state == state; }

    auto only_process() const -> Process&
    {
        REQUIRE(cli->children().size() == 1U);
        auto* process = dynamic_cast<Process*>(cli->children().front());
        REQUIRE(process != nullptr);
        return *process;
    }

    void finish()
    {
        until(
            [this] { return probe.active.empty() && cli->children().empty() && client->active_request_count() == 0U; });
        CHECK(scheduler->state() == SchedulerState::Running);
        CHECK_FALSE(scheduler->failure());
        CHECK(probe.committed_completions == probe.starts.size());
        scheduler->stop();
    }

    // Dependency declaration order is intentional: the HTTP client outlives the group, which outlives Scheduler.
    Application                                 app;
    HttpTestServer                              server;
    TemporaryDirectory                          directory;
    std::filesystem::path                       database_file;
    Database                                    database;
    DurableReader                               observer;
    RejectingSecretProvider                     secrets;
    StandardAttributeRegistry                   registry;
    FakeCronEngine                              cron;
    SequenceUuidGenerator                       generator;
    FakeTimeSource                              time;
    RunRepository                               runs;
    AttemptRepository                           attempts;
    ExecutionProbe                              probe;
    std::vector<std::unique_ptr<HelperChannel>> channels;
    std::unique_ptr<SystemHttpClient>           client;
    std::unique_ptr<AttemptExecutorGroup>       group;
    CliAttemptExecutor*                         cli{};
    std::unique_ptr<ManagementService>          management;
    std::unique_ptr<Scheduler>                  scheduler;
};

auto result_outcome(JobAttempt const& attempt) -> std::string
{
    REQUIRE(attempt.result);
    return attempt.result->as_object().at("outcome").as_string();
}

auto retained_pattern(std::size_t total, std::size_t limit, std::size_t channel) -> ByteBuffer
{
    auto       bytes  = ByteBuffer{};
    auto const prefix = (limit + 1U) / 2U;
    for (std::size_t index = 0; index < std::min(total, limit); ++index) {
        auto const position = total > limit && index >= prefix ? total - limit + index : index;
        bytes.push_back(static_cast<std::byte>((position + (channel * 73U)) % 251U));
    }
    return bytes;
}

auto retry_attributes(std::string mode) -> AttributeSet
{
    return {
        {"retry.max_attempts",  {.data = std::int64_t{2}}     },
        {"retry.strategy",      {.data = std::string{"fixed"}}},
        {"retry.initial_delay", {.data = Duration{10s}}       },
        {"retry.max_delay",     {.data = Duration{10s}}       },
        {"retry.jitter",        {.data = 0.0}                 },
        {"retry.mode",          {.data = std::move(mode)}     },
    };
}

} // namespace

TEST_CASE("mixed scheduling commits running state before real helper execution",
          "[jobu][scheduler][cli][integration][sqlite]")
{
    MixedSchedulerFixture fixture;
    auto const            queue  = fixture.queue("durable", 1, 2);
    auto const            marker = fixture.directory.path() / "executed";
    auto const            cli  = fixture.create(queue, JobType::Cli, helper_payload({"marker", marker.string()}, {37}));
    auto const            http = fixture.http_job(queue, "/durable");
    fixture.server.release_responses();
    REQUIRE(fixture.scheduler->start());
    fixture.until(
        [&] { return fixture.has_state(cli, RunState::Succeeded) && fixture.has_state(http, RunState::Succeeded); });
    CHECK(std::filesystem::exists(marker));
    CHECK(result_outcome(fixture.attempt(cli.run.id)) == "success");
    CHECK(result_outcome(fixture.attempt(http.run.id)) == "expected_status");
    CHECK(fixture.server.requests().size() == 1U);
    fixture.finish();
}

TEST_CASE("mixed scheduling keeps global resource limits independent", "[jobu][scheduler][cli][integration][sqlite]")
{
    for (auto const saturated_type : {JobType::Cli, JobType::Http}) {
        CAPTURE(saturated_type);
        MixedSchedulerFixture fixture{
            {.cli_concurrency = 2, .http_concurrency = 2}
        };
        auto const queue = fixture.queue("wide", 1, 8);
        auto       jobs  = std::vector<CreatedJob>{};
        for (int index = 0; index < 3; ++index) {
            jobs.push_back(saturated_type == JobType::Cli ? fixture.wait_job(queue) : fixture.http_job(queue, "/held"));
        }
        REQUIRE(fixture.scheduler->start());
        CHECK(fixture.probe.starts.size() == 2U);

        // Admit the other family only after the first family has filled its global budget.
        for (int index = 0; index < 3; ++index) {
            jobs.push_back(saturated_type == JobType::Cli ? fixture.http_job(queue, "/other")
                                                          : fixture.wait_job(queue));
        }
        fixture.rescan();
        REQUIRE(fixture.probe.starts.size() == 4U);
        CHECK(std::ranges::count(fixture.probe.starts, JobType::Cli, &StartObservation::type) == 2);
        CHECK(std::ranges::count(fixture.probe.starts, JobType::Http, &StartObservation::type) == 2);
        fixture.until([&] { return fixture.server.requests().size() == 2U; });
        CHECK(fixture.probe.active.size() == 4U);
        CHECK(std::ranges::count_if(jobs,
                                    [&](auto const& job) { return fixture.has_state(job, RunState::Scheduled); }) == 2);

        fixture.release_helpers();
        fixture.server.release_responses();
        fixture.until([&] {
            return std::ranges::all_of(jobs,
                                       [&](auto const& job) { return fixture.has_state(job, RunState::Succeeded); });
        });
        fixture.finish();
    }
}

TEST_CASE("one queue shares its capacity across CLI and HTTP", "[jobu][scheduler][cli][integration][sqlite]")
{
    for (auto const first_type : {JobType::Cli, JobType::Http}) {
        CAPTURE(first_type);
        MixedSchedulerFixture fixture{
            {.cli_concurrency = 2, .http_concurrency = 2}
        };
        auto const queue = fixture.queue("combined");
        auto const first = first_type == JobType::Cli ? fixture.wait_job(queue) : fixture.http_job(queue, "/first");
        REQUIRE(fixture.scheduler->start());
        auto const second = first_type == JobType::Cli ? fixture.http_job(queue, "/second") : fixture.wait_job(queue);
        fixture.rescan();
        CHECK(fixture.probe.starts.size() == 1U);
        CHECK(fixture.has_state(first, RunState::Running));
        CHECK(fixture.has_state(second, RunState::Scheduled));
        fixture.release_helpers();
        fixture.server.release_responses();
        fixture.until([&] {
            return fixture.has_state(first, RunState::Succeeded) && fixture.has_state(second, RunState::Succeeded);
        });
        fixture.finish();
    }
}

TEST_CASE("mixed scheduling maintains separate weighted fairness histories",
          "[jobu][scheduler][cli][integration][sqlite]")
{
    MixedSchedulerFixture fixture{
        {.cli_concurrency = 1, .http_concurrency = 1, .candidate_batch_size = 2}
    };
    auto const light = fixture.queue("light", 1, 2);
    auto const heavy = fixture.queue("heavy", 2, 2);
    auto       jobs  = std::vector<CreatedJob>{};
    for (auto const* queue : {&light, &heavy}) {
        for (int index = 0; index < 6; ++index) {
            jobs.push_back(fixture.create(*queue, JobType::Cli, helper_payload({"exit", "0"})));
            jobs.push_back(fixture.http_job(*queue, "/fairness"));
        }
    }
    fixture.server.release_responses();
    REQUIRE(fixture.scheduler->start());
    fixture.until([&] {
        return std::ranges::all_of(jobs, [&](auto const& job) { return fixture.has_state(job, RunState::Succeeded); });
    });
    for (auto const type : {JobType::Cli, JobType::Http}) {
        auto queues = std::vector<Uuid>{};
        for (auto const& start : fixture.probe.starts) {
            if (start.type == type) {
                queues.push_back(start.queue_id);
            }
        }
        REQUIRE(queues.size() == 12U);
        for (auto const prefix : {3U, 6U, 9U}) {
            CAPTURE(type, prefix);
            CHECK(std::count(queues.begin(), queues.begin() + prefix, light.id) == prefix / 3U);
            CHECK(std::count(queues.begin(), queues.begin() + prefix, heavy.id) == prefix * 2U / 3U);
        }
    }
    fixture.finish();
}

TEST_CASE("real CLI failures retry with durable attempt numbering and queue policy",
          "[jobu][scheduler][cli][integration][sqlite]")
{
    for (auto const* mode : {"blocking", "reschedule"}) {
        for (auto const* cause : {"unexpected_exit", "signal", "timeout"}) {
            CAPTURE(mode, cause);
            MixedSchedulerFixture fixture{
                {.cli_concurrency = 1, .http_concurrency = 1}
            };
            auto const queue      = fixture.queue("retry");
            auto       attributes = retry_attributes(mode);
            auto       arguments  = std::vector<std::string>{"exit", "7"};
            if (std::string_view{cause} == "signal") {
                arguments = {"signal", std::to_string(SIGTERM)};
            }
            else if (std::string_view{cause} == "timeout") {
                // A real wait-mode target cannot finish by itself. Only Process's monotonic deadline can finish it.
                fixture.channels.push_back(std::make_unique<HelperChannel>(fixture.directory.path() / "timeout-wait"));
                arguments = {"wait", fixture.channels.back()->path()};
                attributes.emplace("job.timeout", AttributeValue{.data = Duration{100ms}});
                attributes.emplace("cli.termination_grace", AttributeValue{.data = Duration::zero()});
            }
            auto const target = fixture.create(queue, JobType::Cli, helper_payload(arguments), attributes);
            REQUIRE(fixture.scheduler->start());
            fixture.until([&] { return fixture.has_state(target, RunState::RetryWait); });
            CHECK(result_outcome(fixture.attempt(target.run.id)) == cause);
            CHECK(fixture.run(target.run.id).runnable_at == at_seconds(110));
            CHECK_FALSE(fixture.run(target.run.id).completed_at);

            auto const follower = fixture.create(queue, JobType::Cli, helper_payload({"exit", "0"}));
            fixture.time.set_utc(at_seconds(109));
            fixture.rescan();
            if (std::string_view{mode} == "blocking") {
                CHECK(fixture.has_state(follower, RunState::Scheduled));
            }
            else {
                fixture.until([&] { return fixture.has_state(follower, RunState::Succeeded); });
            }
            auto second_before_due = fixture.attempts.find(target.run.id, 2);
            REQUIRE(second_before_due);
            CHECK_FALSE(second_before_due->has_value());

            fixture.time.set_utc(at_seconds(110));
            fixture.rescan();
            fixture.until([&] {
                return fixture.has_state(target, RunState::Failed) && fixture.has_state(follower, RunState::Succeeded);
            });
            auto const second = fixture.attempt(target.run.id, 2);
            CHECK(second.run_id == target.run.id);
            CHECK(second.due_at == at_seconds(110));
            CHECK(second.started_at == at_seconds(110));
            CHECK(second.outcome == AttemptOutcome::Failed);
            CHECK(result_outcome(second) == cause);
            auto third = fixture.attempts.find(target.run.id, 3);
            REQUIRE(third);
            CHECK_FALSE(third->has_value());
            fixture.finish();
        }
    }
}

TEST_CASE("concurrent CLI exits distinguish expected success and terminal selector exclusion",
          "[jobu][scheduler][cli][integration][sqlite]")
{
    MixedSchedulerFixture fixture{
        {.cli_concurrency = 2, .http_concurrency = 1}
    };
    auto const queue      = fixture.queue("outcomes", 1, 2);
    auto       attributes = retry_attributes("reschedule");
    attributes.emplace("cli.retry_exit_codes",
                       AttributeValue{.data = AttributeValue::List{{.data = std::string{"7"}}}});
    auto const success  = fixture.create(queue, JobType::Cli, helper_payload({"exit", "7"}, {7}), attributes);
    auto const terminal = fixture.create(queue, JobType::Cli, helper_payload({"exit", "8"}), attributes);
    REQUIRE(fixture.scheduler->start());
    CHECK(fixture.probe.active.size() == 2U);
    fixture.until([&] {
        return fixture.has_state(success, RunState::Succeeded) && fixture.has_state(terminal, RunState::Failed);
    });
    CHECK(result_outcome(fixture.attempt(success.run.id)) == "success");
    CHECK(result_outcome(fixture.attempt(terminal.run.id)) == "unexpected_exit");
    fixture.time.set_utc(at_seconds(200));
    fixture.rescan();
    CHECK(fixture.probe.starts.size() == 2U);
    fixture.finish();
}

TEST_CASE("real CLI capture persists binary first and last bytes with completion metadata",
          "[jobu][scheduler][cli][integration][sqlite]")
{
    for (auto const* mode : {"none", "on_error", "always"}) {
        for (auto const success : {false, true}) {
            CAPTURE(mode, success);
            MixedSchedulerFixture fixture;
            auto const            queue       = fixture.queue("capture");
            // More than pipe capacity on both streams proves that bounded retention does not stop draining.
            constexpr std::size_t stdout_size = (192 * 1024) + 3;
            constexpr std::size_t stderr_size = (160 * 1024) + 7;
            auto const            job =
                fixture.create(queue,
                               JobType::Cli,
                               helper_payload(
                                   {
                                       "output",
                                       std::to_string(stdout_size),
                                       std::to_string(stderr_size)
            },
                                   success ? std::vector<std::uint64_t>{37} : std::vector<std::uint64_t>{0}),
                               {
                                   {"retry.max_attempts", {.data = std::int64_t{1}}},
                                   {"output.capture", {.data = std::string{mode}}},
                                   {"output.stdout_limit", {.data = std::int64_t{9}}},
                                   {"output.stderr_limit", {.data = std::int64_t{8}}},
                               });
            REQUIRE(fixture.scheduler->start());
            fixture.until([&] { return fixture.has_state(job, success ? RunState::Succeeded : RunState::Failed); });
            auto const attempt = fixture.attempt(job.run.id);
            REQUIRE(attempt.result);
            CHECK(fixture.run(job.run.id).result == attempt.result);
            auto const& result = attempt.result->as_object();
            CHECK_FALSE(result.at("capture_lost").as_bool());
            for (auto const channel : {0U, 1U}) {
                auto const& stream = result.at(channel == 0 ? "stdout" : "stderr").as_object();
                auto const  total  = channel == 0 ? stdout_size : stderr_size;
                auto const  limit  = channel == 0 ? 9U : 8U;
                CHECK(stream.at("total_bytes").as_uint() == total);
                CHECK(stream.at("captured_bytes").as_uint() == (std::string_view{mode} == "none" ? 0U : limit));
                CHECK(stream.at("truncated").as_bool());
            }
            auto output = fixture.attempts.find_output(job.run.id, 1);
            REQUIRE(output);
            auto const retained =
                std::string_view{mode} == "always" || (std::string_view{mode} == "on_error" && !success);
            REQUIRE(output->has_value() == retained);
            if (retained) {
                CHECK(output->value().stdout_bytes == retained_pattern(stdout_size, 9, 0));
                CHECK(output->value().stderr_bytes == retained_pattern(stderr_size, 8, 1));
                CHECK(output->value().stdout_truncated);
                CHECK(output->value().stderr_truncated);
                CHECK_FALSE(output->value().capture_lost);
            }
            fixture.finish();
        }
    }
}

TEST_CASE("CLI cancellation retains capacity through group termination and durable completion",
          "[jobu][scheduler][cli][integration][sqlite]")
{
    for (auto const leader_handles_term : {false, true}) {
        CAPTURE(leader_handles_term);
        MixedSchedulerFixture fixture{
            {.cli_concurrency = 1, .http_concurrency = 1}
        };
        auto const    queue = fixture.queue("cancel");
        HelperChannel report{fixture.directory.path() / "group-report"};
        auto          arguments = leader_handles_term ? std::vector<std::string>{"group", report.path(), "1", "0"}
                                                      : std::vector<std::string>{"group-wait", report.path()};
        auto const    target    = fixture.create(queue,
                                                 JobType::Cli,
                                                 helper_payload(std::move(arguments)),
                                                 {
                                                     {"output.capture", {.data = std::string{"always"}}}
        });
        REQUIRE(fixture.scheduler->start());
        auto identities = std::optional<std::array<pid_t, 2>>{};
        fixture.until([&] {
            if (!identities) {
                identities = report.read_group();
            }
            return identities.has_value();
        });
        TerminationWatch leader{(*identities)[0]};
        TerminationWatch descendant{(*identities)[1]};
        CHECK_FALSE(leader.terminated());
        CHECK_FALSE(descendant.terminated());
        auto&      process             = fixture.only_process();
        auto const follower            = fixture.create(queue, JobType::Cli, helper_payload({"exit", "0"}));
        fixture.probe.hold_completions = true;
        REQUIRE(fixture.scheduler->cancel_run(target.run.id));
        CHECK(process.state() == ProcessState::Stopping);
        CHECK(fixture.has_state(target, RunState::Running));
        CHECK(fixture.has_state(follower, RunState::Scheduled));
        CHECK(fixture.probe.starts.size() == 1U);

        fixture.rescan();
        CHECK(fixture.probe.starts.size() == 1U);
        fixture.until([&] { return fixture.probe.held.size() == 1U; });
        fixture.until([&] { return leader.terminated() && descendant.terminated(); });
        CHECK(fixture.probe.held.front().value.outcome == AttemptOutcome::Cancelled);
        auto const& result = fixture.probe.held.front().value.result.as_object();
        CHECK(result.at("outcome").as_string() == "cancelled");
        CHECK_FALSE(result.at("capture_lost").as_bool());
        if (leader_handles_term) {
            CHECK(result.at("exit_code").as_uint() == 42U);
        }
        else {
            CHECK(result.at("signal").as_uint() == static_cast<std::uint64_t>(SIGKILL));
        }
        fixture.rescan();
        CHECK(fixture.has_state(target, RunState::Running));
        CHECK(fixture.has_state(follower, RunState::Scheduled));
        CHECK(fixture.probe.starts.size() == 1U);

        // Even after native reaping and both output terminals, the scheduler still owns capacity until its exact
        // callback commits. Releasing the held real completion allows a later scheduler wake to admit the follower.
        fixture.probe.release_completions();
        CHECK(fixture.has_state(target, RunState::Cancelled));
        CHECK(fixture.probe.starts.size() == 1U);
        fixture.until([&] { return fixture.has_state(follower, RunState::Succeeded); });
        fixture.finish();
    }
}

TEST_CASE("continuous CLI output permits other dispatch and timeout or cancellation",
          "[jobu][scheduler][cli][integration][sqlite]")
{
    for (auto const timeout : {false, true}) {
        CAPTURE(timeout);
        MixedSchedulerFixture fixture{
            {.cli_concurrency = 2, .http_concurrency = 1}
        };
        auto const queue      = fixture.queue("output-pressure", 1, 3);
        auto       attributes = AttributeSet{
            {"retry.max_attempts",    {.data = std::int64_t{1}}             },
            {"output.capture",        {.data = std::string{"always"}}       },
            {"output.stdout_limit",   {.data = std::int64_t{9}}             },
            {"output.stderr_limit",   {.data = std::int64_t{8}}             },
            {"job.timeout",           {.data = Duration{timeout ? 2s : 30s}}},
            {"cli.termination_grace", {.data = Duration::zero()}            },
        };
        auto const writer = fixture.create(queue, JobType::Cli, helper_payload({"continuous", "3"}), attributes);
        REQUIRE(fixture.scheduler->start());
        auto   observed = std::array<std::size_t, 2>{};
        Object receiver;
        auto&  process = fixture.only_process();
        auto   stdout_connection =
            process.standard_output.connect(&receiver, [&](ByteBuffer const& bytes) { observed[0] += bytes.size(); });
        auto stderr_connection =
            process.standard_error.connect(&receiver, [&](ByteBuffer const& bytes) { observed[1] += bytes.size(); });
        constexpr auto read_budget = std::size_t{256} * 1024;
        fixture.until([&] { return observed[0] > read_budget && observed[1] > read_budget; });

        auto const other_cli = fixture.create(queue, JobType::Cli, helper_payload({"exit", "0"}));
        auto const http      = fixture.http_job(queue, "/while-writing");
        fixture.server.release_responses();
        fixture.scheduler->request_rescan();
        fixture.until([&] {
            return fixture.has_state(other_cli, RunState::Succeeded) && fixture.has_state(http, RunState::Succeeded);
        });
        CHECK(fixture.has_state(writer, RunState::Running));
        if (!timeout) {
            REQUIRE(fixture.scheduler->cancel_run(writer.run.id));
        }
        fixture.until([&] { return fixture.has_state(writer, timeout ? RunState::Failed : RunState::Cancelled); });
        auto const attempt = fixture.attempt(writer.run.id);
        REQUIRE(attempt.result);
        CHECK(attempt.result->as_object().at(timeout ? "outcome" : "reason").as_string() ==
              (timeout ? "timeout" : "cancelled"));
        CHECK(observed[0] > read_budget);
        CHECK(observed[1] > read_budget);
        auto output = fixture.attempts.find_output(writer.run.id, 1);
        REQUIRE(output);
        REQUIRE(output->has_value());
        CHECK(output->value().stdout_bytes == retained_pattern(observed[0], 9, 0));
        CHECK(output->value().stderr_bytes == retained_pattern(observed[1], 8, 1));
        fixture.finish();
    }
}

TEST_CASE("real mixed completions preserve recurring successors and suspension drains",
          "[jobu][scheduler][cli][integration][sqlite]")
{
    MixedSchedulerFixture fixture{
        {.cli_concurrency = 1, .http_concurrency = 1}
    };
    auto const queue    = fixture.queue("drain", 1, 2);
    auto const schedule = CronSchedule{.expression = "*/5 * * * *", .timezone = "UTC"};
    fixture.cron.set_occurrences(schedule, {at_seconds(110), at_seconds(200), at_seconds(300)});
    fixture.channels.push_back(std::make_unique<HelperChannel>(fixture.directory.path() / "recurring"));
    auto const recurring =
        fixture.create(queue, JobType::Cli, helper_payload({"wait", fixture.channels.back()->path()}), {}, schedule);
    auto const http = fixture.http_job(queue, "/draining");
    fixture.time.set_utc(at_seconds(110));
    REQUIRE(fixture.scheduler->start());
    fixture.until([&] { return fixture.server.requests().size() == 1U; });
    auto job_suspending = fixture.management->suspend_job(recurring.definition.id);
    REQUIRE(job_suspending);
    CHECK(job_suspending->state == JobState::Suspending);
    auto queue_suspending = fixture.management->suspend_queue(queue.id);
    REQUIRE(queue_suspending);
    CHECK(queue_suspending->state == QueueState::Suspending);

    fixture.release_helpers();
    fixture.until([&] { return fixture.has_state(recurring, RunState::Succeeded); });
    auto job_suspended = fixture.management->get_job(recurring.definition.id);
    REQUIRE(job_suspended);
    CHECK(job_suspended->state == JobState::Suspended);
    auto still_draining = fixture.management->get_queue(queue.id);
    REQUIRE(still_draining);
    CHECK(still_draining->state == QueueState::Suspending);
    auto successor = fixture.runs.find_schedule_owned(recurring.definition.id);
    REQUIRE(successor);
    REQUIRE(successor->has_value());
    CHECK(successor->value().id != recurring.run.id);
    CHECK(successor->value().planned_at == at_seconds(200));
    CHECK(successor->value().state == RunState::Scheduled);
    auto const successor_id = successor->value().id;

    fixture.server.release_responses();
    fixture.until([&] { return fixture.has_state(http, RunState::Succeeded); });
    auto queue_suspended = fixture.management->get_queue(queue.id);
    REQUIRE(queue_suspended);
    CHECK(queue_suspended->state == QueueState::Suspended);
    fixture.time.set_utc(at_seconds(200));
    fixture.rescan();
    CHECK(fixture.probe.starts.size() == 2U);
    REQUIRE(fixture.management->resume_queue(queue.id));
    fixture.rescan();
    CHECK(fixture.probe.starts.size() == 2U);
    REQUIRE(fixture.management->resume_job(recurring.definition.id));
    fixture.rescan();
    CHECK(fixture.run(successor_id).state == RunState::Running);
    fixture.release_helpers();
    fixture.until([&] { return fixture.run(successor_id).state == RunState::Succeeded; });
    fixture.finish();
}
