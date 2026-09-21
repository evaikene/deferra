#include "runtime_priv.hpp"

#include "cli/cli_attempt_executor.hpp"
#include "cron.hpp"
#include "event_loop.hpp"
#include "framing.hpp"
#include "http/http_attempt_executor.hpp"
#include "http/system_http_client.hpp"
#include "management.hpp"
#include "protocol_priv.hpp"
#include "recovery.hpp"
#include "server.hpp"
#include "support/fault_database_driver.hpp"
#include "support/memory_io_device.hpp"
#include "support/shutdown_fixture.hpp"
#include "support/storage_fault_helpers.hpp"
#include "time_source.hpp"
#include "uuid.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace jb::jobud::detail {
struct RuntimeTestAccess {
    static auto management(DaemonRuntime& runtime) { return runtime.management(); }

    static auto scheduler(DaemonRuntime& runtime) { return runtime.scheduler(); }

    static auto rpc(DaemonRuntime& runtime) { return runtime.rpc_server(); }
};
} // namespace jb::jobud::detail

namespace {
using namespace jb::core;
using namespace jb::jobu;
using namespace jb::jobud::detail;
using namespace jb::test;

struct RetainedCompletion {
    AttemptKey               key;
    AttemptCompletionHandler handler;

    void deliver() const
    {
        handler({
            .key     = key,
            .outcome = AttemptOutcome::Succeeded,
            .result  = {.data = JsonValue::Object{}},
            .output  = AttemptOutput{.primary = AttemptOutputChannel{}, .diagnostic = AttemptOutputChannel{}}
        });
    }
};

/// Real runners stay active while tests retain duplicate notifications for the post-latch lifetime checks.
class RecordingExecutor final : public AttemptExecutor {
public:
    RecordingExecutor(std::unique_ptr<AttemptExecutor> runner, std::vector<RetainedCompletion>& completions)
        : _runner{std::move(runner)}
        , _completions{completions}
    {}

    auto is_available(JobType type) const noexcept -> bool override { return _runner->is_available(type); }

    auto start(AttemptStartRequest request, AttemptCompletionHandler completion) -> Result<void, Error> override
    {
        _completions.push_back({.key = request.key, .handler = completion});
        return _runner->start(std::move(request), std::move(completion));
    }

    auto cancel(AttemptKey const& key) -> Result<void, Error> override { return _runner->cancel(key); }

private:
    std::unique_ptr<AttemptExecutor> _runner;
    std::vector<RetainedCompletion>& _completions;
};

auto mutation_frame() -> std::string
{
    auto document = jb::rpc::detail::encode_request(
        1,
        "queue.create",
        JsonValue{.data = JsonValue::Object{{"name", {.data = std::string{"late-buffered"}}}}});
    auto serialized = serialize_json(document);
    REQUIRE(serialized);
    auto framed = jb::rpc::frame_message(*serialized);
    REQUIRE(framed);
    return *framed;
}

// Trigger failure only after both real runners report readiness and their starts are committed.
void trigger_stop(std::string_view                       scenario,
                  DaemonRuntime&                         runtime,
                  DatabaseFaultState&                    faults,
                  std::vector<RetainedCompletion> const& completions)
{
    auto* management = RuntimeTestAccess::management(runtime);
    if (scenario == "completion failure") {
        faults.faults.push_back({
            .at    = {.boundary = "completion.output", .operation = DatabaseOperation::Execute},
            .error = fault_error()
        });
        completions.front().deliver();
        require_consumed_faults(faults);
    }
    else if (scenario == "management failure") {
        faults.faults.push_back({
            .at    = {.boundary = "management.queue", .operation = DatabaseOperation::Execute},
            .error = fault_error()
        });
        REQUIRE_FALSE(management->create_queue({.name = "fatal"}));
        require_consumed_faults(faults);
    }
    else {
        runtime.request_stop();
        if (scenario == "fatal after stop") {
            runtime.fail("test", fault_error());
        }
    }
}

void exercise(std::string_view scenario)
{
    require_shutdown_execution_environment();
    auto const retry = GENERATE(false, true);
    CAPTURE(scenario, retry);
    auto faults      = std::make_shared<DatabaseFaultState>();
    faults->classify = [](std::string_view sql) -> std::string {
        if (sql.starts_with("INSERT INTO jobu_attempt_output")) {
            return "completion.output";
        }
        if (sql.starts_with("INSERT INTO jobu_queues")) {
            return "management.queue";
        }
        return "other";
    };
    ShutdownWork work{[&](std::unique_ptr<jb::db::Driver> driver) {
        return std::make_unique<FaultDatabaseDriver>(std::move(driver), faults);
    }};
    work.seed(retry ? RecoveryPolicy::RetryInterrupted : RecoveryPolicy::FailInterrupted);
    SystemTimeSource time;
    SystemCronEngine cron;
    UuidV7Generator  generator{time};
    StartupOptions   options;
    options.socket_path                     = work.storage.directory.path() / "runtime.sock";
    options.cli_concurrency                 = 1;
    options.http_concurrency                = 1;
    auto                            runtime = std::make_unique<DaemonRuntime>(*work.app.event_loop(),
                                                                              work.storage.database,
                                                                              work.storage.registry,
                                                                              cron,
                                                                              generator,
                                                                              time,
                                                                              options);
    std::vector<RetainedCompletion> completions;
    auto                            make_runners = [&]() -> Result<RuntimeRunners, Error> {
        auto client = jb::net::http::SystemHttpClient::create(*work.app.event_loop());
        REQUIRE(client);
        auto group = std::make_unique<AttemptExecutorGroup>();
        REQUIRE(group->add(JobType::Cli,
                           std::make_unique<RecordingExecutor>(std::make_unique<cli::CliAttemptExecutor>(
                                                                   cli::CliAttemptExecutorOptions{.allow_root = true}),
                                                               completions)));
        REQUIRE(
            group->add(JobType::Http,
                       std::make_unique<RecordingExecutor>(std::make_unique<http::HttpAttemptExecutor>(**client, time),
                                                           completions)));
        return Result<RuntimeRunners, Error>::success({.http = std::move(*client), .executors = std::move(group)});
    };

    std::vector<std::vector<std::string>> running;
    bool                                  drained{false};
    auto const                            result = runtime->run(make_runners, [&] {
        work.await_work();
        REQUIRE(completions.size() == 2);
        auto* management = RuntimeTestAccess::management(*runtime);
        running          = work.snapshot();
        auto  device     = std::make_unique<MemoryIODevice>();
        auto* peer       = device.get();
        device->open();
        REQUIRE(RuntimeTestAccess::rpc(*runtime)->add_connection(std::move(device)));

        REQUIRE(work.app.event_loop()->post([&, management, peer] {
            trigger_stop(scenario, *runtime, *faults, completions);
            REQUIRE(runtime->state() == RuntimeState::Stopping);
            runtime->request_stop();
            auto const calls = faults->calls.size();

            // Queued after the current task snapshot: this executes in the real loop's final drain.
            REQUIRE(work.app.event_loop()->post([&, management, peer, calls] {
                drained       = true;
                auto mutation = management->create_queue({.name = "late-direct"});
                REQUIRE_FALSE(mutation);
                REQUIRE(mutation.error().code == "jobu.service.stopping");
                peer->inject_input(mutation_frame() + mutation_frame());
                REQUIRE(peer->written_data().find("jobu.service.stopping") != std::string::npos);
                auto* scheduler = RuntimeTestAccess::scheduler(*runtime);
                REQUIRE_FALSE(scheduler->start());
                scheduler->request_rescan();
                for (auto const& completion : completions) {
                    completion.deliver();
                }
                REQUIRE(faults->calls.size() == calls);
                REQUIRE(completions.size() == 2);
            }));
        }));
        return work.app.exec();
    });
    REQUIRE(drained);
    REQUIRE(result == (scenario == "normal stop" ? EXIT_SUCCESS : EXIT_FAILURE));
    REQUIRE(runtime->state() == RuntimeState::Stopped);
    work.require_cleanup();
    work.require_reaped();
    REQUIRE(work.snapshot() == running);

    // These are child-to-group callbacks, whose contract ends at executor destruction. The final-drain
    // checks above exercise the terminal gate while owners are alive; scheduler-token lifetime has its own tests.
    completions.clear();
    runtime.reset();
    work.storage.reopen();
    work.hold_recovery();
    auto recovered = recover_startup(work.storage.database, work.storage.registry, cron, generator, time);
    REQUIRE(recovered);
    REQUIRE(recovered->interrupted_attempts == 2);
    work.require_recovery(retry);
    auto const snapshot = work.snapshot();
    REQUIRE(recover_startup(work.storage.database, work.storage.registry, cron, generator, time));
    REQUIRE(work.snapshot() == snapshot);
}
} // namespace

TEST_CASE("normal stop")
{
    exercise("normal stop");
}

TEST_CASE("completion failure")
{
    exercise("completion failure");
}

TEST_CASE("management failure")
{
    exercise("management failure");
}

TEST_CASE("fatal after stop")
{
    exercise("fatal after stop");
}
