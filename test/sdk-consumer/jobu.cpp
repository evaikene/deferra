#include <jb/jobu/scheduler.hpp>

#include <jb/core/application.hpp>
#include <jb/core/byte_buffer.hpp>
#include <jb/core/connection.hpp>
#include <jb/core/error.hpp>
#include <jb/core/object.hpp>
#include <jb/core/result.hpp>
#include <jb/core/time_source.hpp>
#include <jb/core/uuid.hpp>
#include <jb/db/database.hpp>
#include <jb/db/driver.hpp>
#include <jb/db/driver_query.hpp>
#include <jb/jobu/attempt.hpp>
#include <jb/jobu/attempt_executor.hpp>
#include <jb/jobu/attribute_registry.hpp>
#include <jb/jobu/cron.hpp>
#include <jb/jobu/execution_events.hpp>
#include <jb/jobu/job.hpp>
#include <jb/jobu/retry_mode.hpp>
#include <jb/jobu/run.hpp>
#include <jb/jobu/secret_provider.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <memory>
#include <optional>
#include <string_view>

namespace {

template <typename T = void>
auto unexpected_operation() -> jb::core::Result<T, jb::core::Error>
{
    return jb::core::Result<T, jb::core::Error>::failure({.category = jb::core::ErrorCategory::Unavailable,
                                                          .code     = "sdk.unexpected_operation",
                                                          .message  = "The SDK consumer does not dispatch work"});
}

/// An open, query-free backend lets the generic consumer obey Scheduler's construction
/// contract without discovering SQLite or implementing a scheduling repository.
class IdleDriver final : public jb::db::Driver {
public:
    auto name() const noexcept -> std::string_view override { return "sdk-idle"; }

private:
    auto open() -> jb::core::Result<void, jb::core::Error> override
    {
        _open = true;
        return jb::core::Result<void, jb::core::Error>::success();
    }

    auto close() -> jb::core::Result<void, jb::core::Error> override
    {
        _open = false;
        return jb::core::Result<void, jb::core::Error>::success();
    }

    auto is_open() const noexcept -> bool override { return _open; }

    auto create_query() -> jb::core::Result<std::unique_ptr<jb::db::DriverQuery>, jb::core::Error> override
    {
        return unexpected_operation<std::unique_ptr<jb::db::DriverQuery>>();
    }

    auto begin([[maybe_unused]] jb::db::TransactionMode mode) -> jb::core::Result<void, jb::core::Error> override
    {
        return unexpected_operation();
    }

    auto commit() -> jb::core::Result<void, jb::core::Error> override { return unexpected_operation(); }

    auto rollback() -> jb::core::Result<void, jb::core::Error> override { return unexpected_operation(); }

    bool _open{false};
};

class IdleExecutor final : public jb::jobu::AttemptExecutor {
public:
    auto is_available([[maybe_unused]] jb::jobu::JobType type) const noexcept -> bool override { return false; }

    auto start([[maybe_unused]] jb::jobu::AttemptStartRequest      request,
               [[maybe_unused]] jb::jobu::AttemptCompletionHandler completion)
        -> jb::core::Result<void, jb::core::Error> override
    {
        return unexpected_operation();
    }

    auto cancel([[maybe_unused]] jb::jobu::AttemptKey const& key) -> jb::core::Result<void, jb::core::Error> override
    {
        return unexpected_operation();
    }
};

class IdleSecrets final : public jb::jobu::SecretProvider {
public:
    auto resolve([[maybe_unused]] std::string_view name)
        -> jb::core::Result<jb::core::ByteBuffer, jb::core::Error> override
    {
        return unexpected_operation<jb::core::ByteBuffer>();
    }
};

/// Receiver slots retain typed copies rather than borrowed signal arguments.
struct Observations {
    std::optional<jb::jobu::AttemptStarted>  started;
    std::optional<jb::jobu::AttemptFinished> finished;
    std::optional<jb::jobu::RetryScheduled>  retry;
    std::optional<jb::jobu::RunFinished>     run;
};

auto connect_execution(jb::jobu::Scheduler& scheduler, jb::core::Object& receiver, Observations& values)
    -> std::array<jb::core::Connection, 4>
{
    return {
        scheduler.attempt_started.connect(&receiver,
                                          [&values](jb::jobu::AttemptStarted const& value) { values.started = value; }),
        scheduler.attempt_finished.connect(
            &receiver,
            [&values](jb::jobu::AttemptFinished const& value) { values.finished = value; }),
        scheduler.retry_scheduled.connect(&receiver,
                                          [&values](jb::jobu::RetryScheduled const& value) { values.retry = value; }),
        scheduler.run_finished.connect(&receiver,
                                       [&values](jb::jobu::RunFinished const& value) { values.run = value; }),
    };
}

auto check_event_copies() -> bool
{
    using namespace jb::jobu;
    using namespace std::chrono_literals;

    auto id = jb::core::Uuid::parse("01900000-0000-7000-8000-000000000001");
    if (!id) {
        return false;
    }

    // Values retained by an SDK consumer must survive their source's scope and mutations.
    Observations saved;
    {
        ExecutionContext context{.queue_id = *id,
                                 .job_id   = *id,
                                 .run_id   = *id,
                                 .type     = JobType::Cli,
                                 .origin   = RunOrigin::Manual};
        auto             timestamp = jb::core::UtcTimePoint{123456us};
        AttemptStarted   started{.execution = context, .attempt_number = 1, .started_at = timestamp};
        AttemptFinished  finished{.execution      = context,
                                  .attempt_number = 1,
                                  .outcome        = AttemptOutcome::Failed,
                                  .start_phase    = AttemptStartPhase::Accepted,
                                  .completed_at   = timestamp,
                                  .elapsed        = 12ms,
                                  .exit_code      = 7};
        RetryScheduled   retry{.execution         = context,
                               .completed_attempt = 1,
                               .next_attempt      = 2,
                               .runnable_at       = timestamp,
                               .mode              = RetryMode::Reschedule};
        RunFinished      run{.execution          = context,
                             .state              = RunState::Cancelled,
                             .completed_at       = timestamp,
                             .completing_attempt = std::nullopt};
        saved = {.started = started, .finished = finished, .retry = retry, .run = run};

        started.execution.run_id = {};
        finished.exit_code       = 0;
        retry.mode               = RetryMode::Blocking;
        run.completing_attempt   = 2;
    }

    return saved.started->execution.run_id == *id && saved.finished->exit_code == 7 &&
           saved.finished->elapsed == 12ms && saved.retry->next_attempt == 2 &&
           saved.retry->mode == RetryMode::Reschedule && !saved.run->completing_attempt;
}

} // namespace

int main(int argc, char const* argv[])
{
    jb::core::Application app{argc, argv};
    jb::db::Database      database{std::make_unique<IdleDriver>()};
    if (!database.open() || !check_event_copies()) {
        return 1;
    }

    // Every borrowed dependency and retained slot value outlives the stopped scheduler.
    jb::jobu::StandardAttributeRegistry attributes;
    jb::jobu::SystemCronEngine          cron;
    jb::core::SystemTimeSource          clock;
    jb::core::UuidV7Generator           ids{clock};
    IdleExecutor                        executor;
    IdleSecrets                         secrets;
    Observations                        values;
    jb::jobu::Scheduler                 scheduler{database, attributes, cron, ids, clock, executor, secrets};
    std::array<jb::core::Connection, 4> connections;
    {
        jb::core::Object receiver;
        connections = connect_execution(scheduler, receiver, values);
        if (!std::ranges::all_of(connections, [](auto const& connection) { return connection.is_valid(); })) {
            return 2;
        }
    }

    // Real Scheduler members instantiate installed Signal support. This fixture checks
    // subscription/linkage and receiver teardown; lifecycle emission is tested in JobU.
    if (std::ranges::any_of(connections, [](auto const& connection) { return connection.is_valid(); })) {
        return 3;
    }
    scheduler.shutdown();
    return scheduler.state() == jb::jobu::SchedulerState::Shutdown && !values.started && !values.finished &&
                   !values.retry && !values.run
             ? 0
             : 4;
}
