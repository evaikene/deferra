#include "runtime_priv.hpp"

#include "attempt_repository_priv.hpp"
#include "control_json.hpp"
#include "control_rpc.hpp"
#include "framing.hpp"
#include "history_json.hpp"
#include "http/http_attempt_executor.hpp"
#include "json.hpp"
#include "local_socket.hpp"
#include "management.hpp"
#include "protocol_priv.hpp"
#include "query.hpp"
#include "run_repository_priv.hpp"
#include "secret_service.hpp"
#include "server.hpp"
#include "sqlite/sqlite_schema.hpp"
#include "statistics_service.hpp"
#include "support/fake_cron_engine.hpp"
#include "support/fake_event_loop_backend.hpp"
#include "support/fake_http_client.hpp"
#include "support/fake_time_source.hpp"
#include "support/fault_database_driver.hpp"
#include "support/memory_io_device.hpp"
#include "support/recovery_fixture.hpp"
#include "support/storage_fault_helpers.hpp"
#include "utc_timestamp.hpp"
#include "uuid.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace jb::jobud::detail {

struct RuntimeTestAccess {
    static auto management(DaemonRuntime& runtime) { return runtime.management(); }

    static auto secrets(DaemonRuntime& runtime) { return runtime.secrets(); }

    static auto statistics(DaemonRuntime& runtime) { return runtime.statistics(); }

    static auto scheduler(DaemonRuntime& runtime) { return runtime.scheduler(); }

    static auto rpc(DaemonRuntime& runtime) { return runtime.rpc_server(); }
};

} // namespace jb::jobud::detail

namespace {

using namespace jb::core;
using namespace jb::jobu;
using namespace jb::jobud::detail;
using namespace jb::test;
using namespace std::chrono_literals;
using RunnersResult = Result<RuntimeRunners, Error>;

auto success(AttemptKey key) -> AttemptCompletion
{
    return {.key = key, .outcome = AttemptOutcome::Succeeded, .result = {.data = JsonValue::Object{}}};
}

auto failure() -> Error
{
    return {.category = ErrorCategory::Internal,
            .code     = "net.http.backend_failed",
            .message  = "sensitive message",
            .detail   = "sensitive detail"};
}

struct ExecutionRecord {
    std::vector<AttemptStartRequest>      starts;
    std::vector<AttemptCompletionHandler> completions;
    std::vector<std::string>              destruction;
    std::function<void()>                 on_start;
};

/// Probes retained completions during final drains and child teardown while the group and runtime still exist.
class ObservedExecutor final : public AttemptExecutor {
public:
    explicit ObservedExecutor(ExecutionRecord& record)
        : _record{record}
    {}

    ~ObservedExecutor() override
    {
        _record.destruction.emplace_back("executor");
        if (!_record.completions.empty()) {
            _record.completions.front()(success(_record.starts.front().key));
        }
    }

    auto is_available(JobType type) const noexcept -> bool override { return type == JobType::Cli; }

    auto start(AttemptStartRequest request, AttemptCompletionHandler completion) -> Result<void, Error> override
    {
        _record.starts.push_back(std::move(request));
        _record.completions.push_back(std::move(completion));
        if (_record.on_start) {
            _record.on_start();
        }
        return Result<void, Error>::success();
    }

    auto cancel(AttemptKey const& /*key*/) -> Result<void, Error> override { return Result<void, Error>::success(); }

private:
    ExecutionRecord& _record;
};

struct RuntimeFixture {
    explicit RuntimeFixture(RecoveryFixtureSchema schema = RecoveryFixtureSchema::Current)
        : storage{[this](std::unique_ptr<jb::db::Driver> driver) {
                      return std::make_unique<FaultDatabaseDriver>(std::move(driver), faults);
                  },
                  schema}
    {
        faults->classify = [this](std::string_view sql) -> std::string {
            if (sql.starts_with("SELECT value_blob FROM jobu_secrets")) {
                return "dispatch.secret";
            }
            if (sql.starts_with("SELECT id FROM jobu_runs WHERE 1 = 1") &&
                sql.find("AND state = :state") == std::string_view::npos) {
                auto committed = std::ranges::find(faults->calls,
                                                   DatabaseCall{.boundary  = "connection",
                                                                .operation = DatabaseOperation::Commit,
                                                                .phase     = DatabaseFaultPhase::AfterSuccess});
                return committed == faults->calls.end() ? "recovery.scan" : "recovery.final_scan";
            }
            if (sql.starts_with("INSERT INTO jobu_attempt_output")) {
                return "recovery.output";
            }
            if (sql.starts_with("SELECT id AS run_id, type AS run_type, payload_json AS run_payload_json")) {
                return "secrets.snapshots";
            }
            if (sql.starts_with("INSERT INTO jobu_secret_refs")) {
                return "management.references";
            }
            if (sql.starts_with("INSERT INTO jobu_secrets")) {
                return "secrets.insert";
            }
            if (sql.starts_with("INSERT INTO jobu_queues")) {
                return "management.queue";
            }
            if (sql.starts_with("UPDATE jobu_runs SET state = 'cancelled'")) {
                return "cancellation.run";
            }
            return "other";
        };
        time.set_utc(UtcTimePoint{120s});
        auto queue              = recovery_queue(recovery_id(1));
        queue.concurrency_limit = 2;
        storage.insert_queue(queue);
        options.socket_path      = storage.directory.path() / "daemon.sock";
        options.cli_concurrency  = 1;
        options.http_concurrency = 1;
    }

    auto seed(std::uint32_t suffix = 1, JobType type = JobType::Cli, RunState state = RunState::Scheduled)
        -> RecoveryRunFixture
    {
        auto job = storage.make_job(recovery_id(suffix + 10), recovery_id(1), type);
        auto run = storage.make_run(recovery_id(suffix + 100), job, state);
        storage.insert_job(job);
        storage.insert_run(run);
        return run;
    }

    void create_runtime(std::function<bool()> should_stop = {}, CronEngine const* cron_override = nullptr)
    {
        runtime = std::make_unique<DaemonRuntime>(*loop.loop,
                                                  storage.database,
                                                  storage.registry,
                                                  cron_override ? *cron_override : cron,
                                                  generator,
                                                  time,
                                                  options,
                                                  std::move(should_stop));
    }

    auto make_runners() -> RunnersResult
    {
        ++factory_calls;
        CHECK(runtime->state() == RuntimeState::Recovering);
        CHECK_FALSE(std::filesystem::exists(options.socket_path));
        auto client = std::make_unique<FakeHttpClient>();
        http        = client.get();
        client->destroyed.connect(runtime.get(), [this] { record.destruction.emplace_back("http"); });
        auto group = std::make_unique<AttemptExecutorGroup>();
        REQUIRE(group->add(JobType::Cli, std::make_unique<ObservedExecutor>(record)));
        REQUIRE(group->add(JobType::Http, std::make_unique<http::HttpAttemptExecutor>(*client, time)));
        return RunnersResult::success({.http = std::move(client), .executors = std::move(group)});
    }

    auto run(std::function<int()> const& execute) -> int
    {
        return runtime->run([this] { return make_runners(); }, execute);
    }

    void require_running(RecoveryRunFixture const& expected)
    {
        detail::RunRepository runs{storage.database, storage.registry};
        auto                  run = runs.find_by_id(expected.run.id);
        REQUIRE(run);
        REQUIRE(run->has_value());
        REQUIRE(run->value().state == RunState::Running);
        detail::AttemptRepository attempts{storage.database};
        auto                      attempt = attempts.find(expected.run.id, 1);
        REQUIRE(attempt);
        REQUIRE(attempt->has_value());
        REQUIRE(attempt->value().state == AttemptState::Running);
        auto output = attempts.find_output(expected.run.id, 1);
        REQUIRE(output);
        REQUIRE_FALSE(output->has_value());
    }

    jb::core::priv::FakeEventLoop          loop{jb::core::priv::make_fake_event_loop()};
    jb::core::priv::ScopedCurrentEventLoop current{loop.loop.get()};
    std::shared_ptr<DatabaseFaultState>    faults = std::make_shared<DatabaseFaultState>();
    RecoveryFixture                        storage;
    FakeTimeSource                         time;
    FakeCronEngine                         cron;
    UuidV7Generator                        generator{time};
    StartupOptions                         options;
    ExecutionRecord                        record;
    FakeHttpClient*                        http{};
    int                                    factory_calls{};
    std::unique_ptr<DaemonRuntime>         runtime;
};

auto request(std::string_view method, std::string_view name) -> std::string
{
    auto json = jb::rpc::detail::encode_request(
        1,
        method,
        JsonValue{.data = JsonValue::Object{{"name", JsonValue{.data = std::string{name}}}}});
    auto serialized = serialize_json(json);
    REQUIRE(serialized);
    auto frame = jb::rpc::frame_message(*serialized);
    REQUIRE(frame);
    return std::move(*frame);
}

class RuntimeRpcEndpoint {
public:
    explicit RuntimeRpcEndpoint(jb::rpc::Server& server)
    {
        auto device = std::make_unique<MemoryIODevice>();
        _device     = device.get();
        device->open();
        REQUIRE(server.add_connection(std::move(device)));
    }

    auto call(std::string_view method, JsonValue params) -> jb::rpc::detail::ResponseEnvelope
    {
        auto document = jb::rpc::detail::encode_request(_next_id++, method, params);
        auto body     = serialize_json(document);
        REQUIRE(body);
        auto framed = jb::rpc::frame_message(*body);
        REQUIRE(framed);
        _device->inject_input(*framed);

        jb::rpc::StreamFramer framer;
        auto                  bodies = framer.append(_device->take_written_data());
        REQUIRE(bodies);
        REQUIRE(bodies->size() == 1);
        auto parsed = parse_json(bodies->front());
        REQUIRE(parsed);
        auto decoded = jb::rpc::detail::decode_response_document(*parsed);
        REQUIRE(decoded);
        REQUIRE(decoded->entries.size() == 1);
        return std::move(decoded->entries.front());
    }

private:
    MemoryIODevice* _device{};
    std::uint64_t   _next_id{1};
};

auto rpc_result(jb::rpc::detail::ResponseEnvelope const& response) -> JsonValue const&
{
    REQUIRE(std::holds_alternative<JsonValue>(response.payload));
    return std::get<JsonValue>(response.payload);
}

auto rpc_error(jb::rpc::detail::ResponseEnvelope const& response) -> jb::rpc::RpcError const&
{
    REQUIRE(std::holds_alternative<jb::rpc::RpcError>(response.payload));
    return std::get<jb::rpc::RpcError>(response.payload);
}

void check_application_error(jb::rpc::detail::ResponseEnvelope const& response,
                             std::string_view                         category,
                             std::string_view                         code)
{
    auto const& error = rpc_error(response);
    CHECK(error.code == static_cast<std::int64_t>(jb::rpc::ErrorCode::ApplicationError));
    REQUIRE(error.data);
    REQUIRE(error.data->is_object());
    auto const& data = error.data->as_object();
    REQUIRE(data.size() == 2);
    CHECK(data.at("category").as_string() == category);
    CHECK(data.at("code").as_string() == code);
    CHECK(error.message.find("private-backend-marker") == std::string::npos);
}

} // namespace

TEST_CASE("Daemon recovery failure prevents runner construction listening and dispatch")
{
    RuntimeFixture fixture;
    fixture.seed(1, JobType::Cli, RunState::Running);
    {
        jb::db::Query query{fixture.storage.database};
        REQUIRE(query.exec("DELETE FROM jobu_attempts"));
    }
    fixture.create_runtime();
    auto result = fixture.run([] {
        FAIL("must not enter event loop");
        return EXIT_SUCCESS;
    });
    REQUIRE(result == EXIT_FAILURE);
    REQUIRE(fixture.factory_calls == 0);
    REQUIRE(fixture.record.starts.empty());
    REQUIRE_FALSE(std::filesystem::exists(fixture.options.socket_path));
    REQUIRE(fixture.runtime->state() == RuntimeState::Stopped);
}

TEST_CASE("Daemon injected recovery failures never construct runners or enter serving")
{
    using Operation = DatabaseOperation;
    using Phase     = DatabaseFaultPhase;
    for (auto const& fault : std::vector<DatabaseCall>{
             {.boundary = "recovery.scan", .operation = Operation::Prepare},
             {.boundary = "recovery.output", .operation = Operation::Execute},
             {.boundary = "connection", .operation = Operation::Commit},
             {.boundary = "connection", .operation = Operation::Commit, .phase = Phase::AfterSuccess},
             {.boundary = "recovery.final_scan", .operation = Operation::Fetch}
    }) {
        DYNAMIC_SECTION(fault.boundary << ' ' << static_cast<int>(fault.operation) << ' '
                                       << static_cast<int>(fault.phase))
        {
            RuntimeFixture fixture;
            auto           original = fixture.seed(1, JobType::Cli, RunState::Running);
            fixture.create_runtime();
            fixture.faults->calls.clear();
            fixture.faults->faults.push_back({.at = fault, .error = fault_error()});
            auto result = fixture.run([] {
                FAIL("failed recovery must not enter the event loop");
                return EXIT_SUCCESS;
            });
            CHECK(result == EXIT_FAILURE);
            require_consumed_faults(*fixture.faults);
            CHECK(fixture.factory_calls == 0);
            CHECK(fixture.record.starts.empty());
            CHECK_FALSE(std::filesystem::exists(fixture.options.socket_path));
            CHECK(fixture.runtime->state() == RuntimeState::Stopped);

            fixture.runtime.reset();
            fixture.storage.reopen();
            if (fault.phase == Phase::AfterSuccess || fault.boundary == "recovery.final_scan") {
                detail::RunRepository runs{fixture.storage.database, fixture.storage.registry};
                auto                  repaired = runs.find_by_id(original.run.id);
                REQUIRE(repaired);
                REQUIRE(*repaired);
                CHECK((*repaired)->state == RunState::Interrupted);
            }
            else {
                fixture.storage.require_run(original);
            }
        }
    }
}

TEST_CASE("Daemon recovery precedes runner construction and scheduler startup")
{
    RuntimeFixture fixture;
    auto           interrupted = fixture.seed(1, JobType::Cli, RunState::Running);
    auto           runnable    = fixture.seed(2);
    fixture.create_runtime();
    auto result = fixture.run([&] {
        REQUIRE(fixture.runtime->state() == RuntimeState::Serving);
        REQUIRE(std::filesystem::is_socket(fixture.options.socket_path));
        REQUIRE(fixture.record.starts.size() == 1);
        REQUIRE(fixture.record.starts.front().key.run_id == runnable.run.id);
        detail::RunRepository runs{fixture.storage.database, fixture.storage.registry};
        auto                  run = runs.find_by_id(interrupted.run.id);
        REQUIRE(run);
        REQUIRE(run->has_value());
        REQUIRE(run->value().state == RunState::Interrupted);
        fixture.runtime->request_stop();
        return EXIT_SUCCESS;
    });
    REQUIRE(result == EXIT_SUCCESS);
    REQUIRE(fixture.record.destruction == std::vector<std::string>{"executor", "http"});
    fixture.require_running(runnable);
    REQUIRE_FALSE(std::filesystem::exists(fixture.options.socket_path));
}

TEST_CASE("Daemon startup failures unwind runners without terminalizing durable attempts")
{
    RuntimeFixture fixture;
    auto           seeded = fixture.seed();
    bool           listener_failure{false};
    SECTION("scheduler startup")
    {
        fixture.options.cli_concurrency = 0;
    }
    SECTION("listener after dispatch")
    {
        listener_failure            = true;
        fixture.options.socket_path = fixture.storage.directory.path() / "missing" / "daemon.sock";
    }
    fixture.create_runtime();
    auto result = fixture.run([] {
        FAIL("must not enter event loop");
        return EXIT_SUCCESS;
    });
    REQUIRE(result == EXIT_FAILURE);
    REQUIRE(fixture.factory_calls == 1);
    REQUIRE(fixture.record.destruction == std::vector<std::string>{"executor", "http"});
    if (listener_failure) {
        REQUIRE(fixture.record.starts.size() == 1);
        fixture.require_running(seeded);
    }
    else {
        REQUIRE(fixture.record.starts.empty());
        fixture.storage.require_run(seeded);
    }
    REQUIRE_FALSE(std::filesystem::exists(fixture.options.socket_path));
}

TEST_CASE("Daemon signal polling skips startup and preserves successful exit")
{
    RuntimeFixture fixture;
    fixture.seed();
    bool stop{false};
    SECTION("before recovery")
    {
        stop = true;
    }
    SECTION("during recovery")
    {}
    fixture.create_runtime([&] { return stop || fixture.runtime->state() == RuntimeState::Recovering; });
    REQUIRE(fixture.run([] {
        FAIL("must not enter event loop");
        return EXIT_FAILURE;
    }) == EXIT_SUCCESS);
    REQUIRE(fixture.factory_calls == 0);
    REQUIRE_FALSE(std::filesystem::exists(fixture.options.socket_path));
}

TEST_CASE("Daemon signal during synchronous dispatch prevents listening and further dispatch")
{
    RuntimeFixture fixture;
    auto           first  = fixture.seed();
    auto           second = fixture.seed(2);
    bool           signal_requested{false};
    fixture.record.on_start = [&] { signal_requested = true; };
    fixture.create_runtime([&] { return signal_requested; });
    REQUIRE(fixture.run([] {
        FAIL("must not enter event loop");
        return EXIT_FAILURE;
    }) == EXIT_SUCCESS);
    REQUIRE(fixture.record.starts.size() == 1);
    fixture.require_running(first);
    fixture.storage.require_run(second);
    REQUIRE_FALSE(std::filesystem::exists(fixture.options.socket_path));
}

TEST_CASE("Daemon gates remain latched through final task drains and retained completions")
{
    RuntimeFixture fixture;
    auto           seeded = fixture.seed();
    fixture.create_runtime();
    bool drained{false};
    auto result = fixture.run([&] {
        auto* management = RuntimeTestAccess::management(*fixture.runtime);
        auto* scheduler  = RuntimeTestAccess::scheduler(*fixture.runtime);
        REQUIRE(fixture.loop.loop->post([&, management, scheduler] {
            fixture.runtime->request_stop();
            fixture.runtime->request_stop();
            // This task is added after the normal task snapshot. It executes in run()'s final drain.
            REQUIRE(fixture.loop.loop->post([&, management, scheduler] {
                drained = true;
                REQUIRE(fixture.record.destruction.empty());
                REQUIRE(scheduler->state() == SchedulerState::Shutdown);
                REQUIRE_FALSE(scheduler->start());
                auto created = management->create_queue({.name = "late"});
                REQUIRE_FALSE(created);
                REQUIRE(created.error().code == "jobu.service.stopping");
                auto secret = RuntimeTestAccess::secrets(*fixture.runtime)->set({.name = "late"});
                REQUIRE_FALSE(secret);
                REQUIRE(secret.error().code == "jobu.service.stopping");
                fixture.record.completions.front()(success(fixture.record.starts.front().key));
                fixture.require_running(seeded);
            }));
        }));
        return fixture.loop.loop->run() ? EXIT_SUCCESS : EXIT_FAILURE;
    });
    REQUIRE(result == EXIT_SUCCESS);
    REQUIRE(drained);
    fixture.runtime.reset();
    // Child-to-group callbacks borrow their owners; post-destruction scheduler-token tests live separately.
    fixture.require_running(seeded);
}

TEST_CASE("Daemon HTTP shared failure wins before failed completion persistence")
{
    RuntimeFixture fixture;
    auto           cli  = fixture.seed();
    auto           http = fixture.seed(2, JobType::Http);
    fixture.seed(3);
    fixture.seed(4, JobType::Http);
    fixture.create_runtime();
    auto result = fixture.run([&] {
        REQUIRE(fixture.http->pending_request_ids().size() == 1);
        auto const calls = fixture.faults->calls.size();
        REQUIRE(fixture.http->inject_shared_failure(failure()));
        REQUIRE(fixture.runtime->state() == RuntimeState::Stopping);
        REQUIRE(fixture.record.destruction.empty());
        fixture.record.completions.front()(success(fixture.record.starts.front().key));
        auto rejected = RuntimeTestAccess::management(*fixture.runtime)->create_queue({.name = "late"});
        REQUIRE_FALSE(rejected);
        REQUIRE(rejected.error().code == "jobu.service.stopping");
        auto secret = RuntimeTestAccess::secrets(*fixture.runtime)->set({.name = "late"});
        REQUIRE_FALSE(secret);
        REQUIRE(secret.error().code == "jobu.service.stopping");
        REQUIRE(fixture.faults->calls.size() == calls);
        REQUIRE(fixture.record.starts.size() == 1);
        REQUIRE(fixture.http->start_records().size() == 1);
        fixture.require_running(cli);
        fixture.require_running(http);
        fixture.runtime->request_stop();
        return EXIT_SUCCESS;
    });
    REQUIRE(result == EXIT_FAILURE);
    REQUIRE(fixture.record.destruction == std::vector<std::string>{"executor", "http"});
    fixture.require_running(cli);
    fixture.require_running(http);
}

TEST_CASE("Daemon scheduler failure shuts management admission before the notifying callback returns")
{
    RuntimeFixture fixture;
    auto           seeded = fixture.seed();
    fixture.create_runtime();
    auto result = fixture.run([&] {
        // A mismatched completion identity is a fatal scheduler invariant, not a normal failed job.
        fixture.record.completions.front()(success({.run_id = recovery_id(999), .attempt_number = 1}));
        REQUIRE(fixture.runtime->state() == RuntimeState::Stopping);
        REQUIRE(RuntimeTestAccess::scheduler(*fixture.runtime)->state() == SchedulerState::Failed);
        auto secret = RuntimeTestAccess::secrets(*fixture.runtime)->erase("late");
        REQUIRE_FALSE(secret);
        REQUIRE(secret.error().code == "jobu.service.stopping");
        auto rejected = RuntimeTestAccess::management(*fixture.runtime)->create_queue({.name = "late"});
        REQUIRE_FALSE(rejected);
        REQUIRE(rejected.error().code == "jobu.service.stopping");
        REQUIRE(fixture.record.destruction.empty());
        fixture.record.completions.front()(success(fixture.record.starts.front().key));
        fixture.require_running(seeded);
        return EXIT_SUCCESS;
    });
    REQUIRE(result == EXIT_FAILURE);
    fixture.require_running(seeded);
}

TEST_CASE("Daemon connection admission stays closed during already-ready listener callbacks")
{
    RuntimeFixture fixture;
    auto           cli  = fixture.seed();
    auto           http = fixture.seed(2, JobType::Http);
    fixture.create_runtime();
    REQUIRE(fixture.run([&] {
        auto const listener_fd = fixture.loop.backend->last_added_fd;
        auto       callback    = jb::core::priv::EventLoopTestAccess::fd_callback(*fixture.loop.loop, listener_fd);
        REQUIRE(callback);
        jb::net::LocalSocket client;
        client.connect_to_server(fixture.options.socket_path);
        REQUIRE(client.state() != jb::net::LocalSocketState::Unconnected);
        fixture.runtime->request_stop();
        auto const calls = fixture.faults->calls.size();

        // Simulate a listener readiness notification already present in the current poll batch.
        // The listener is still alive, but its notification must not transfer a connection to RPC.
        callback(listener_fd, FdEvent::Read);
        REQUIRE(RuntimeTestAccess::rpc(*fixture.runtime)->connection_count() == 0);
        REQUIRE(fixture.record.destruction.empty());
        fixture.record.completions.front()(success(fixture.record.starts.front().key));
        REQUIRE(fixture.faults->calls.size() == calls);
        fixture.require_running(cli);
        fixture.require_running(http);
        return EXIT_SUCCESS;
    }) == EXIT_SUCCESS);
    REQUIRE_FALSE(std::filesystem::exists(fixture.options.socket_path));
}

TEST_CASE("Daemon management failure gates buffered RPC requests without destroying callback owners")
{
    RuntimeFixture fixture;
    auto           seeded = fixture.seed();
    fixture.create_runtime();
    auto result = fixture.run([&] {
        auto* server = RuntimeTestAccess::rpc(*fixture.runtime);
        auto  device = std::make_unique<MemoryIODevice>();
        auto* peer   = device.get();
        device->open();
        REQUIRE(server->add_connection(std::move(device)));
        {
            jb::db::Query query{fixture.storage.database};
            REQUIRE(query.exec("CREATE TRIGGER fail_queue BEFORE INSERT ON jobu_queues "
                               "WHEN NEW.name = 'first' BEGIN SELECT RAISE(ABORT, 'sensitive SQL'); END"));
        }
        peer->inject_input(request("queue.create", "first") + request("queue.create", "second"));
        REQUIRE(fixture.runtime->state() == RuntimeState::Stopping);
        REQUIRE(fixture.record.destruction.empty());
        REQUIRE(server->connection_count() == 1);
        REQUIRE(peer->written_data().find("jobu.service.stopping") != std::string::npos);
        REQUIRE(peer->written_data().find("sensitive SQL") == std::string::npos);
        fixture.record.completions.front()(success(fixture.record.starts.front().key));
        fixture.require_running(seeded);
        return EXIT_SUCCESS;
    });
    REQUIRE(result == EXIT_FAILURE);
    REQUIRE(fixture.runtime->state() == RuntimeState::Stopped);
    fixture.require_running(seeded);
}

TEST_CASE("Daemon injected mutation failures gate buffered requests and retained completions")
{
    for (auto const* scenario : {"write", "acknowledgement", "conflict_rollback"}) {
        DYNAMIC_SECTION(scenario)
        {
            RuntimeFixture fixture;
            auto           seeded       = fixture.seed();
            bool const     conflict     = std::string_view{scenario} == "conflict_rollback";
            bool const     acknowledged = std::string_view{scenario} == "acknowledgement";
            if (conflict) {
                auto queue = recovery_queue(recovery_id(9));
                queue.name = "first";
                fixture.storage.insert_queue(queue);
            }
            fixture.create_runtime();
            auto result = fixture.run([&] {
                auto* server  = RuntimeTestAccess::rpc(*fixture.runtime);
                auto* service = RuntimeTestAccess::management(*fixture.runtime);
                auto  device  = std::make_unique<MemoryIODevice>();
                auto* peer    = device.get();
                device->open();
                REQUIRE(server->add_connection(std::move(device)));

                std::size_t calls_at_failure = 0;
                std::size_t failures         = 0;
                auto        connection       = service->failed.connect(service, [&](Error const& error) {
                    ++failures;
                    calls_at_failure = fixture.faults->calls.size();
                    check_safe_error(error, "db.io");
                });
                auto fault = DatabaseCall{.boundary = "management.queue", .operation = DatabaseOperation::Execute};
                if (conflict || acknowledged) {
                    fault = {.boundary  = "connection",
                             .operation = conflict ? DatabaseOperation::Rollback : DatabaseOperation::Commit,
                             .phase     = acknowledged ? DatabaseFaultPhase::AfterSuccess : DatabaseFaultPhase::Before};
                }
                fixture.faults->faults.push_back({.at = fault, .error = fault_error()});
                // Both frames are already admitted. The second must reach the management gate after
                // the first latched failure, with owners alive until this event-loop stack unwinds.
                peer->inject_input(request("queue.create", "first") + request("queue.create", "second"));
                CHECK(fixture.runtime->state() == RuntimeState::Stopping);
                CHECK(fixture.record.destruction.empty());
                CHECK(server->connection_count() == 1);
                CHECK(failures == 1U);
                REQUIRE(calls_at_failure > 0U);
                CHECK(fixture.faults->calls.size() == calls_at_failure);
                CHECK(peer->written_data().find("jobu.service.stopping") != std::string::npos);
                CHECK(peer->written_data().find("private-backend-marker") == std::string::npos);
                fixture.record.completions.front()(success(fixture.record.starts.front().key));
                auto secret = RuntimeTestAccess::secrets(*fixture.runtime)->set({.name = "late"});
                CHECK_FALSE(secret);
                CHECK(secret.error().code == "jobu.service.stopping");
                CHECK(fixture.faults->calls.size() == calls_at_failure);
                connection.disconnect();
                return EXIT_SUCCESS;
            });
            CHECK(result == EXIT_FAILURE);
            require_consumed_faults(*fixture.faults);
            CHECK(fixture.runtime->state() == RuntimeState::Stopped);
            fixture.runtime.reset();
            fixture.storage.reopen();
            fixture.require_running(seeded);
            jb::db::Query query{fixture.storage.database};
            REQUIRE(query.exec("SELECT name FROM jobu_queues WHERE name IN ('first', 'second') ORDER BY name"));
            auto next = query.next();
            REQUIRE(next);
            CHECK(*next == (acknowledged || conflict));
            if (*next) {
                CHECK(query.value(0) == jb::db::make_text("first"));
                auto end = query.next();
                REQUIRE(end);
                CHECK_FALSE(*end);
            }
        }
    }
}

TEST_CASE("Daemon records event-loop failure and fatal errors after a normal stop")
{
    RuntimeFixture fixture;
    auto           seeded = fixture.seed();
    fixture.create_runtime();
    bool late_failure{false};
    SECTION("poll failure")
    {
        fixture.loop.backend->poll_result = -1;
    }
    SECTION("fatal after signal")
    {
        late_failure = true;
    }
    auto result = fixture.run([&] {
        if (late_failure) {
            fixture.runtime->request_stop();
            fixture.runtime->fail("test", failure());
            return EXIT_SUCCESS;
        }
        return fixture.loop.loop->run() ? EXIT_SUCCESS : EXIT_FAILURE;
    });
    REQUIRE(result == EXIT_FAILURE);
    REQUIRE(fixture.runtime->state() == RuntimeState::Stopped);
    REQUIRE(fixture.record.destruction == std::vector<std::string>{"executor", "http"});
    fixture.require_running(seeded);
    REQUIRE(fixture.run([] {
        FAIL("must not restart");
        return EXIT_SUCCESS;
    }) == EXIT_FAILURE);
}

TEST_CASE("Daemon closes statistics reads when their storage fails fatally")
{
    RuntimeFixture fixture;
    fixture.seed(1, JobType::Cli, RunState::Succeeded);
    fixture.create_runtime();
    fixture.faults->classify = [](std::string_view sql) {
        return sql.starts_with("SELECT r.state AS run_state") ? "statistics.runs" : "other";
    };

    auto result = fixture.run([&] {
        auto* statistics = RuntimeTestAccess::statistics(*fixture.runtime);
        REQUIRE(statistics != nullptr);
        fixture.faults->faults.push_back({
            .at    = {.boundary = "statistics.runs", .operation = DatabaseOperation::Execute},
            .error = fault_error("db.corrupt")
        });

        auto read = statistics->read(StatisticsRequest{}, StatisticsScope::System);
        REQUIRE_FALSE(read);
        CHECK(read.error().code == "db.corrupt");
        CHECK(fixture.runtime->state() == RuntimeState::Stopping);
        auto stopped = statistics->read(StatisticsRequest{}, StatisticsScope::System);
        REQUIRE_FALSE(stopped);
        CHECK(stopped.error().code == "jobu.service.stopping");
        return EXIT_SUCCESS;
    });
    CHECK(result == EXIT_FAILURE);
    require_consumed_faults(*fixture.faults);
}

TEST_CASE("Daemon runner factory failure never enters serving")
{
    RuntimeFixture fixture;
    bool           stop{false};
    bool           fail_factory{false};
    SECTION("failure after constructing runner infrastructure")
    {
        fail_factory = true;
    }
    SECTION("signal after constructing runner infrastructure")
    {}
    fixture.create_runtime([&] { return stop; });
    REQUIRE(fixture.runtime->run(
                [&] {
                    auto runners = fixture.make_runners();
                    if (fail_factory) {
                        return RunnersResult::failure(failure());
                    }
                    stop = true;
                    return runners;
                },
                [] {
                    FAIL("must not enter event loop");
                    return EXIT_SUCCESS;
                }) == (fail_factory ? EXIT_FAILURE : EXIT_SUCCESS));
    REQUIRE(fixture.runtime->state() == RuntimeState::Stopped);
    REQUIRE(fixture.record.starts.empty());
    REQUIRE(fixture.record.destruction == std::vector<std::string>{"executor", "http"});
    REQUIRE_FALSE(std::filesystem::exists(fixture.options.socket_path));
}

TEST_CASE("Daemon schema rollback poisoning prevents recovery and serving")
{
    RuntimeFixture fixture{RecoveryFixtureSchema::VersionOne};
    fixture.seed(1, JobType::Http, RunState::Running);
    fixture.create_runtime();
    fixture.faults->faults.push_back({
        .at    = {.boundary = "connection", .operation = DatabaseOperation::Commit},
        .error = fault_error()
    });
    fixture.faults->faults.push_back({
        .at    = {.boundary = "connection", .operation = DatabaseOperation::Rollback},
        .error = fault_error()
    });

    // Exercise the same schema-result/fatal gate used in main, with a real SQLite rollback failure.
    auto schema = jb::jobu::sqlite::ensure_schema(fixture.storage.database);
    REQUIRE_FALSE(schema);
    REQUIRE(fixture.storage.database.is_poisoned());
    require_consumed_faults(*fixture.faults);
    fixture.runtime->fail("schema", schema.error());
    auto const calls_before_run = fixture.faults->calls.size();
    CHECK(fixture.run([] {
        FAIL("schema failure must not enter the event loop");
        return EXIT_SUCCESS;
    }) == EXIT_FAILURE);
    CHECK(fixture.faults->calls.size() == calls_before_run);
    CHECK(fixture.factory_calls == 0);
    CHECK(fixture.record.starts.empty());
    CHECK_FALSE(std::filesystem::exists(fixture.options.socket_path));
    CHECK(fixture.runtime->state() == RuntimeState::Stopped);
}

TEST_CASE("Daemon secret failures gate management and retained completion persistence before failure returns")
{
    for (auto const* scenario : {"write", "acknowledgement", "missing_rollback", "snapshot_read", "reference_write"}) {
        DYNAMIC_SECTION(scenario)
        {
            RuntimeFixture fixture;
            auto           seeded = fixture.seed();
            fixture.create_runtime();
            auto result = fixture.run([&] {
                auto* secrets    = RuntimeTestAccess::secrets(*fixture.runtime);
                auto* management = RuntimeTestAccess::management(*fixture.runtime);
                auto* server     = RuntimeTestAccess::rpc(*fixture.runtime);
                auto  device     = std::make_unique<MemoryIODevice>();
                auto* peer       = device.get();
                device->open();
                REQUIRE(server->add_connection(std::move(device)));
                bool const references = std::string_view{scenario} == "reference_write";
                if (references) {
                    REQUIRE(secrets->set({.name = "token"}));
                }
                bool       observed        = false;
                auto&      failure_signal  = references ? management->failed : secrets->failed;
                auto       connection      = failure_signal.connect(secrets, [&](Error const& error) {
                    // The runtime's earlier receiver must close every gate without destroying the active service.
                    observed = true;
                    check_safe_error(error, "db.io");
                    CHECK(fixture.runtime->state() == RuntimeState::Stopping);
                    CHECK(RuntimeTestAccess::scheduler(*fixture.runtime)->state() == SchedulerState::Shutdown);
                    CHECK(fixture.record.destruction.empty());
                    auto const calls = fixture.faults->calls.size();
                    fixture.record.completions.front()(success(fixture.record.starts.front().key));
                    CHECK(fixture.faults->calls.size() == calls);
                });
                bool const missing         = std::string_view{scenario} == "missing_rollback";
                bool const acknowledgement = std::string_view{scenario} == "acknowledgement";
                auto       fault = DatabaseCall{.boundary = "secrets.insert", .operation = DatabaseOperation::Execute};
                if (missing || acknowledgement) {
                    fault = {.boundary  = "connection",
                             .operation = missing ? DatabaseOperation::Rollback : DatabaseOperation::Commit,
                             .phase = acknowledgement ? DatabaseFaultPhase::AfterSuccess : DatabaseFaultPhase::Before};
                }
                bool const snapshot = std::string_view{scenario} == "snapshot_read";
                if (snapshot) {
                    fault = {.boundary = "secrets.snapshots", .operation = DatabaseOperation::Fetch};
                }
                if (references) {
                    fault = {.boundary  = "management.references",
                             .operation = DatabaseOperation::Execute,
                             .phase     = DatabaseFaultPhase::AfterSuccess};
                }
                fixture.faults->faults.push_back({.at = fault, .error = fault_error()});
                if (references) {
                    auto payload = parse_json(R"({"command":"/bin/tool","arguments":[{"secret":"token"}]})");
                    REQUIRE(payload);
                    auto created = management->create_job(
                        {.queue = recovery_id(1), .schedule = OnceSchedule{UtcTimePoint{180s}}, .payload = *payload});
                    REQUIRE_FALSE(created);
                    check_safe_error(created.error(), "db.io");
                }
                else if (missing || snapshot) {
                    auto erased = secrets->erase("missing");
                    REQUIRE_FALSE(erased);
                    check_safe_error(erased.error(), "db.io");
                }
                else {
                    auto set = secrets->set({.name = "token"});
                    REQUIRE_FALSE(set);
                    check_safe_error(set.error(), "db.io");
                }
                REQUIRE(observed);
                auto const calls    = fixture.faults->calls.size();
                auto       rejected = management->create_queue({.name = "late"});
                REQUIRE_FALSE(rejected);
                CHECK(rejected.error().code == "jobu.service.stopping");
                auto late_secret = secrets->set({.name = "late"});
                REQUIRE_FALSE(late_secret);
                CHECK(late_secret.error().code == "jobu.service.stopping");
                // The existing connection can still deliver buffered frames before teardown; service admission wins.
                peer->inject_input(request("queue.create", "buffered"));
                CHECK(peer->written_data().find("jobu.service.stopping") != std::string::npos);
                CHECK(fixture.faults->calls.size() == calls);
                connection.disconnect();
                return EXIT_SUCCESS;
            });
            CHECK(result == EXIT_FAILURE);
            require_consumed_faults(*fixture.faults);
            CHECK(fixture.runtime->state() == RuntimeState::Stopped);
            fixture.runtime.reset();
            fixture.storage.reopen();
            fixture.require_running(seeded);
        }
    }
}

TEST_CASE("Daemon secret writes request later scheduling without adding secret RPC capabilities")
{
    RuntimeFixture fixture;
    auto           seeded = fixture.seed();
    fixture.create_runtime();
    auto result = fixture.run([&] {
        auto* secrets = RuntimeTestAccess::secrets(*fixture.runtime);
        REQUIRE(secrets->set({.name = "token"}));
        REQUIRE(secrets->erase("token"));
        CHECK(fixture.runtime->state() == RuntimeState::Serving);
        CHECK(fixture.record.starts.size() == 1);
        fixture.require_running(seeded);

        auto  device = std::make_unique<MemoryIODevice>();
        auto* peer   = device.get();
        device->open();
        REQUIRE(RuntimeTestAccess::rpc(*fixture.runtime)->add_connection(std::move(device)));
        auto frame = jb::rpc::frame_message(R"({"jsonrpc":"2.0","method":"system.info","id":1,"params":{}})");
        REQUIRE(frame);
        peer->inject_input(*frame);
        CHECK(peer->written_data().find("queue.create") != std::string::npos);
        CHECK(peer->written_data().find("secret.set") == std::string::npos);
        CHECK(peer->written_data().find("secret.list") == std::string::npos);
        CHECK(peer->written_data().find("secret.delete") == std::string::npos);
        fixture.runtime->request_stop();
        return EXIT_SUCCESS;
    });
    CHECK(result == EXIT_SUCCESS);
}

TEST_CASE("Daemon advertises and serves cron preview controls without changing API minor", "[jobud][control][rpc]")
{
    SystemCronEngine engine;
    RuntimeFixture   fixture;
    fixture.create_runtime({}, &engine);
    auto result = fixture.run([&] {
        RuntimeRpcEndpoint endpoint{*RuntimeTestAccess::rpc(*fixture.runtime)};
        auto               info        = endpoint.call("system.info", JsonValue{.data = JsonValue::Object{}});
        auto const&        info_fields = rpc_result(info).as_object();
        CHECK(info_fields.at("api_version").as_object().at("minor").as_uint() == 2);
        auto const& methods = info_fields.at("capabilities").as_array();
        for (auto const* name : {"job.run_now", "run.cancel", "schedule.validate", "schedule.next"}) {
            CHECK(std::ranges::count_if(methods,
                                        [name](JsonValue const& value) { return value.as_string() == name; }) == 1);
        }

        auto schedule   = CronSchedule{.expression = "@daily", .timezone = "Europe/Tallinn"};
        auto validation = schedule_validate_request_to_json(schedule);
        REQUIRE(validation);
        auto validated = endpoint.call("schedule.validate", *validation);
        CHECK(rpc_result(validated).as_object().at("valid").as_bool());

        auto next = schedule_next_request_to_json({
            .schedule = {.expression = "30 3 31 MAR *", .timezone = "Europe/Tallinn"},
            .after    = jb::jobu::parse_utc_timestamp("2024-03-30T00:00:00Z").value(),
            .count    = 1
        });
        REQUIRE(next);
        auto preview     = endpoint.call("schedule.next", *next);
        auto occurrences = schedule_next_result_from_json(rpc_result(preview));
        REQUIRE(occurrences);
        REQUIRE(occurrences->size() == 1);
        CHECK(jb::jobu::format_utc_timestamp(occurrences->front()).value() == "2024-03-31T01:30:00.000000Z");

        auto cyclic = schedule_validate_request_to_json({.expression = "0 0 * * FRI-MON", .timezone = "UTC"});
        REQUIRE(cyclic);
        CHECK(rpc_result(endpoint.call("schedule.validate", *cyclic)).as_object().at("valid").as_bool());

        auto const last_year = parse_utc_timestamp("9999-12-31T23:59:59Z");
        REQUIRE(last_year);
        auto at_limit = schedule_next_request_to_json({
            .schedule = {.expression = "* * * * *", .timezone = "UTC"},
            .after    = *last_year,
            .count    = 1
        });
        REQUIRE(at_limit);
        check_application_error(endpoint.call("schedule.next", *at_limit),
                                "resource_exhausted",
                                "jobu.schedule.out_of_range");

        auto bad_count                                       = *next;
        std::get<JsonValue::Object>(bad_count.data)["count"] = JsonValue{.data = std::uint64_t{0}};
        check_application_error(endpoint.call("schedule.next", bad_count),
                                "invalid_argument",
                                "jobu.schedule.invalid_count");
        auto bad_time = *next;
        std::get<JsonValue::Object>(bad_time.data)["after"] =
            JsonValue{.data = std::string{"2024-03-30T02:00:00+02:00"}};
        CHECK(rpc_error(endpoint.call("schedule.next", bad_time)).code ==
              static_cast<std::int64_t>(jb::rpc::ErrorCode::InvalidParams));

        auto bad_cron = schedule_validate_request_to_json({.expression = "* * *", .timezone = "UTC"});
        REQUIRE(bad_cron);
        check_application_error(endpoint.call("schedule.validate", *bad_cron),
                                "invalid_argument",
                                "jobu.schedule.invalid_expression");
        fixture.runtime->request_stop();
        return EXIT_SUCCESS;
    });
    CHECK(result == EXIT_SUCCESS);
}

TEST_CASE("Run Now RPC replays its original view and pending cancellation commits", "[jobud][control][rpc]")
{
    RuntimeFixture fixture;
    fixture.time.set_utc(UtcTimePoint{1s});
    auto scheduled = fixture.seed();
    fixture.create_runtime();
    auto result = fixture.run([&] {
        RuntimeRpcEndpoint endpoint{*RuntimeTestAccess::rpc(*fixture.runtime)};
        auto params = run_now_request_to_json({.job_id = scheduled.run.job_id, .idempotency_key = "same"});
        REQUIRE(params);
        auto first  = endpoint.call("job.run_now", *params);
        auto manual = run_details_from_json(rpc_result(first), fixture.storage.registry);
        REQUIRE(manual);
        CHECK(manual->origin == RunOrigin::Manual);
        CHECK_FALSE(manual->schedule_owned);
        CHECK(manual->state == RunState::Scheduled);
        CHECK(manual->payload == scheduled.run.payload);
        CHECK(rpc_result(endpoint.call("job.run_now", *params)) == rpc_result(first));

        auto without_key = run_now_request_to_json({.job_id = scheduled.run.job_id});
        REQUIRE(without_key);
        check_application_error(endpoint.call("job.run_now", *without_key), "conflict", "jobu.run.manual_conflict");

        auto cancel = cancel_run_request_to_json(manual->id);
        REQUIRE(cancel);
        auto cancellation =
            cancel_run_result_from_json(rpc_result(endpoint.call("run.cancel", *cancel)), fixture.storage.registry);
        REQUIRE(cancellation);
        CHECK(cancellation->disposition == CancelDisposition::Completed);
        CHECK(cancellation->run.state == RunState::Cancelled);
        check_application_error(endpoint.call("run.cancel", *cancel), "conflict", "jobu.run.state_conflict");
        CHECK(rpc_result(endpoint.call("job.run_now", *params)) == rpc_result(first));

        fixture.storage.require_run(scheduled);
        fixture.runtime->request_stop();
        return EXIT_SUCCESS;
    });
    CHECK(result == EXIT_SUCCESS);
}

TEST_CASE("Oversized Run Now replies keep the committed idempotency result", "[jobud][control][rpc]")
{
    RuntimeFixture fixture;
    fixture.time.set_utc(UtcTimePoint{1s});
    auto scheduled = fixture.seed();
    fixture.create_runtime();
    auto result = fixture.run([&] {
        auto options                      = jb::rpc::ServerOptions{};
        options.framing.max_body_bytes    = 200;
        options.response_limit_error_code = "jobu.response.too_large";
        jb::rpc::Server limited{options};
        REQUIRE(register_control_methods(limited,
                                         *RuntimeTestAccess::management(*fixture.runtime),
                                         *RuntimeTestAccess::scheduler(*fixture.runtime),
                                         fixture.cron,
                                         fixture.storage.registry));
        RuntimeRpcEndpoint endpoint{limited};
        auto               request = RunNowRequest{.job_id = scheduled.run.job_id, .idempotency_key = "oversized"};
        auto               params  = run_now_request_to_json(request);
        REQUIRE(params);
        check_application_error(endpoint.call("job.run_now", *params), "resource_exhausted", "jobu.response.too_large");

        auto replay = RuntimeTestAccess::management(*fixture.runtime)->run_now(request);
        REQUIRE(replay);
        CHECK(replay->origin == RunOrigin::Manual);
        check_application_error(endpoint.call("job.run_now", *params), "resource_exhausted", "jobu.response.too_large");
        fixture.storage.require_run(scheduled);
        fixture.runtime->request_stop();
        return EXIT_SUCCESS;
    });
    CHECK(result == EXIT_SUCCESS);
}

TEST_CASE("Active run cancellation RPC remains requested until its completion commits", "[jobud][control][rpc]")
{
    RuntimeFixture fixture;
    auto           seeded = fixture.seed();
    fixture.create_runtime();
    auto result = fixture.run([&] {
        RuntimeRpcEndpoint endpoint{*RuntimeTestAccess::rpc(*fixture.runtime)};
        auto               params = cancel_run_request_to_json(seeded.run.id);
        REQUIRE(params);
        auto reply =
            cancel_run_result_from_json(rpc_result(endpoint.call("run.cancel", *params)), fixture.storage.registry);
        REQUIRE(reply);
        CHECK(reply->disposition == CancelDisposition::Requested);
        CHECK(reply->run.state == RunState::Running);
        fixture.require_running(seeded);

        fixture.record.completions.front()(success(fixture.record.starts.front().key));
        detail::RunRepository runs{fixture.storage.database, fixture.storage.registry};
        auto                  completed = runs.find_by_id(seeded.run.id);
        REQUIRE(completed);
        REQUIRE(*completed);
        CHECK((*completed)->state == RunState::Cancelled);
        check_application_error(endpoint.call("run.cancel", *params), "conflict", "jobu.run.state_conflict");
        fixture.runtime->request_stop();
        return EXIT_SUCCESS;
    });
    CHECK(result == EXIT_SUCCESS);
}

TEST_CASE("Cancellation RPC closes daemon admission after poisoned rollback", "[jobud][control][rpc]")
{
    using Operation = DatabaseOperation;
    using Phase     = DatabaseFaultPhase;
    RuntimeFixture fixture;
    fixture.time.set_utc(UtcTimePoint{1s});
    auto pending = fixture.seed();
    fixture.create_runtime();
    auto result = fixture.run([&] {
        fixture.faults->faults.push_back({
            .at    = {.boundary = "cancellation.run", .operation = Operation::Execute, .phase = Phase::AfterSuccess},
            .error = fault_error()
        });
        fixture.faults->faults.push_back({
            .at    = {.boundary = "connection", .operation = Operation::Rollback, .phase = Phase::Before},
            .error = fault_error("db.rollback_failed")
        });
        RuntimeRpcEndpoint endpoint{*RuntimeTestAccess::rpc(*fixture.runtime)};
        auto               params = cancel_run_request_to_json(pending.run.id);
        REQUIRE(params);
        check_application_error(endpoint.call("run.cancel", *params), "io", "db.io");
        CHECK(fixture.storage.database.is_poisoned());
        CHECK(fixture.runtime->state() == RuntimeState::Stopping);
        CHECK(RuntimeTestAccess::scheduler(*fixture.runtime)->state() == SchedulerState::Failed);
        check_application_error(endpoint.call("run.cancel", *params), "unavailable", "jobu.scheduler.stopping");
        return EXIT_SUCCESS;
    });
    CHECK(result == EXIT_FAILURE);
    require_consumed_faults(*fixture.faults);
}

TEST_CASE("Daemon secret lookup failure closes admission before retained completions can persist")
{
    RuntimeFixture fixture;
    fixture.options.cli_concurrency = 2;
    auto seeded                     = fixture.seed();
    fixture.create_runtime();
    auto result = fixture.run([&] {
        auto* scheduler  = RuntimeTestAccess::scheduler(*fixture.runtime);
        auto* secrets    = RuntimeTestAccess::secrets(*fixture.runtime);
        auto* management = RuntimeTestAccess::management(*fixture.runtime);
        auto  value      = as_bytes("runtime-secret-sentinel");
        REQUIRE(secrets->set({
            .name  = "token",
            .value = ByteBuffer{value.begin(), value.end()}
        }));
        auto payload = parse_json(R"({"command":"/bin/tool","arguments":[{"secret":"token"}]})");
        REQUIRE(payload);
        REQUIRE(management->create_job(
            {.queue = recovery_id(1), .schedule = OnceSchedule{fixture.time.utc_now()}, .payload = *payload}));
        bool observed   = false;
        auto connection = scheduler->failed.connect(scheduler, [&](Error const& error) {
            observed = true;
            check_safe_error(error, "db.io");
            CHECK(error.detail == "operation=dispatch reason=state_operation_failed");
            CHECK(fixture.runtime->state() == RuntimeState::Stopping);
            CHECK(scheduler->state() == SchedulerState::Failed);
            CHECK(fixture.record.destruction.empty());
            auto const calls = fixture.faults->calls.size();
            fixture.record.completions.front()(success(fixture.record.starts.front().key));
            auto queue = management->create_queue({.name = "late"});
            REQUIRE_FALSE(queue);
            CHECK(queue.error().code == "jobu.service.stopping");
            auto secret = secrets->set({.name = "late"});
            REQUIRE_FALSE(secret);
            CHECK(secret.error().code == "jobu.service.stopping");
            CHECK(fixture.faults->calls.size() == calls);
        });
        fixture.faults->faults.push_back({
            .at    = {.boundary = "dispatch.secret", .operation = DatabaseOperation::Fetch},
            .error = fault_error()
        });
        static_cast<void>(fixture.loop.loop->process_events(EventFlag::All));
        REQUIRE(observed);
        CHECK(fixture.record.starts.size() == 1);
        connection.disconnect();
        return EXIT_SUCCESS;
    });
    CHECK(result == EXIT_FAILURE);
    require_consumed_faults(*fixture.faults);
    fixture.runtime.reset();
    fixture.storage.reopen();
    fixture.require_running(seeded);
}

TEST_CASE("Daemon resolves recovered templates through its database provider")
{
    RuntimeFixture fixture;
    auto           job     = fixture.storage.make_job(recovery_id(11), recovery_id(1));
    auto           payload = parse_json(R"({"command":"/bin/tool","environment":{"TOKEN":{"secret":"token"}}})");
    REQUIRE(payload);
    job.payload = *payload;
    fixture.storage.insert_job(job);
    auto run = fixture.storage.make_run(recovery_id(101), job);
    fixture.storage.insert_run(run);
    fixture.create_runtime();
    // Recovery sees the original reference even though the secret is absent. Install its value only once Serving;
    // the first dispatch must record a safe failure rather than preventing daemon startup.
    auto result = fixture.run([&] {
        CHECK(fixture.runtime->state() == RuntimeState::Serving);
        CHECK(fixture.record.starts.empty());
        detail::RunRepository runs{fixture.storage.database, fixture.storage.registry};
        auto                  stored = runs.find_by_id(run.run.id);
        REQUIRE(stored);
        REQUIRE(stored->has_value());
        CHECK(stored->value().state == RunState::Failed);
        CHECK(stored->value().payload == *payload);
        REQUIRE(stored->value().result);
        CHECK(stored->value().result->as_object().at("error_code").as_string() == "jobu.secret.not_found");

        auto bytes = as_bytes("runtime-secret-sentinel");
        REQUIRE(RuntimeTestAccess::secrets(*fixture.runtime)
                    ->set({
                        .name  = "token",
                        .value = ByteBuffer{bytes.begin(), bytes.end()}
        }));
        REQUIRE(
            RuntimeTestAccess::management(*fixture.runtime)
                ->create_job(
                    {.queue = recovery_id(1), .schedule = OnceSchedule{fixture.time.utc_now()}, .payload = *payload}));
        static_cast<void>(fixture.loop.loop->process_events(EventFlag::All));
        REQUIRE(fixture.record.starts.size() == 1);
        CHECK(fixture.record.starts.front().payload.as_object().at("environment").as_object().at("TOKEN").as_string() ==
              "runtime-secret-sentinel");
        fixture.runtime->request_stop();
        return EXIT_SUCCESS;
    });
    CHECK(result == EXIT_SUCCESS);
}
