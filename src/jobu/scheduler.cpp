#include "scheduler.hpp"

#include "database.hpp"
#include "event_loop.hpp"
#include "object_priv.hpp"
#include "scheduler_core_priv.hpp"
#include "scheduler_repository_priv.hpp"
#include "storage_failure_priv.hpp"
#include "thread_context.hpp"
#include "timer.hpp"

#include <algorithm>
#include <chrono>
#include <string>
#include <utility>

namespace jb::jobu {

namespace {

template <typename T = void>
using SchedulerResult = jb::core::Result<T, jb::core::Error>;

auto scheduler_error(jb::core::ErrorCategory category, std::string code, std::string message) -> jb::core::Error
{
    return {
        .category = category,
        .code     = std::move(code),
        .message  = std::move(message),
    };
}

auto invalid_options() -> jb::core::Error
{
    return scheduler_error(
        jb::core::ErrorCategory::InvalidArgument,
        "jobu.scheduler.invalid_options",
        "Scheduler concurrency limits, candidate batch size, and wall-clock recheck must be positive");
}

auto invalid_state() -> jb::core::Error
{
    return scheduler_error(jb::core::ErrorCategory::Conflict,
                           "jobu.scheduler.invalid_state",
                           "The scheduler operation is not valid in its current state");
}

auto stopping() -> jb::core::Error
{
    return scheduler_error(jb::core::ErrorCategory::Unavailable,
                           "jobu.scheduler.stopping",
                           "The scheduler has permanently stopped");
}

auto recovery_required() -> jb::core::Error
{
    return scheduler_error(jb::core::ErrorCategory::Conflict,
                           "jobu.scheduler.recovery_required",
                           "Persisted running work requires scheduler startup recovery");
}

auto event_loop_unavailable() -> jb::core::Error
{
    return scheduler_error(jb::core::ErrorCategory::Unavailable,
                           "jobu.scheduler.event_loop_unavailable",
                           "The scheduler requires a valid owner-thread event loop");
}

auto valid_options(SchedulerOptions const& options) noexcept -> bool
{
    return options.cli_concurrency > 0 && options.http_concurrency > 0 && options.candidate_batch_size > 0 &&
           options.wall_clock_recheck.count() > 0;
}

} // anonymous namespace

struct Scheduler::Private : jb::core::priv::ObjectPrivate {
    Private(jb::db::Database&        database_value,
            AttributeRegistry const& attributes_value,
            CronEngine const&        cron,
            jb::core::UuidGenerator& uuid_generator,
            jb::core::TimeSource&    time_source,
            AttemptExecutor&         executor,
            SchedulerOptions         options_value)
        : database{
              database_value
    }
        , attributes{attributes_value}
        , options{options_value}
        , core{database_value,
               attributes_value,
               cron,
               uuid_generator,
               time_source,
               executor,
               {.cli_concurrency      = options.cli_concurrency,
                .http_concurrency     = options.http_concurrency,
                .candidate_batch_size = options.candidate_batch_size},
               {.rescan_requested = [this]() -> void { request_rescan(); },
                .failure_reported = [this](jb::core::Error const& error) -> void {
                    fail(error, detail::StorageOperation::Completion);
                }}}
    {
        if (!valid_options(options)) {
            initialization_error = invalid_options();
        }
    }

    void bind_owner(Scheduler& value)
    {
        owner = &value;
        // Receiver tracking prevents the stored wake slot from outliving the Scheduler it drives.
        wake_timer.timeout.connect(&value, [this]() -> void { process_wake(); });
    }

    Scheduler*                     owner{nullptr};
    jb::db::Database&              database;
    AttributeRegistry const&       attributes;
    SchedulerOptions               options;
    SchedulerState                 state{SchedulerState::Stopped};
    bool                           terminal{false};
    std::optional<jb::core::Error> initialization_error;
    std::optional<jb::core::Error> stored_failure;
    jb::core::Timer                wake_timer;
    detail::SchedulerCore          core;

    [[nodiscard]] auto start() -> SchedulerResult<>
    {
        if (terminal) {
            return SchedulerResult<>::failure(stopping());
        }
        if (state == SchedulerState::Running) {
            return SchedulerResult<>::success();
        }
        if (initialization_error) {
            return SchedulerResult<>::failure(*initialization_error);
        }

        // Object and Timer capture affinity independently, so both must still identify this owner-thread loop.
        auto* loop = owner->event_loop();
        if (!loop || !loop->is_valid() || loop->thread_ctx() != jb::core::ThreadCtx::current() ||
            wake_timer.event_loop() != loop) {
            return SchedulerResult<>::failure(event_loop_unavailable());
        }

        detail::SchedulerRepository repository{database, attributes};
        auto                        running = repository.has_any_running_state();
        if (!running) {
            return SchedulerResult<>::failure(std::move(running).error());
        }
        if (*running) {
            return SchedulerResult<>::failure(recovery_required());
        }

        // A fresh start rebuilds durable capacity and begins with deterministic fairness state.
        core.reset();
        state = SchedulerState::Running;
        return process_cycle();
    }

    void stop()
    {
        wake_timer.stop();
        if (state == SchedulerState::Running) {
            state = SchedulerState::Stopped;
        }
    }

    void shutdown() noexcept
    {
        // Latch without destroying callback traversal state. Failure slots may call this synchronously, and the
        // first failure must remain observable after the enclosing runtime also requests shutdown.
        terminal = true;
        core.shutdown();
        wake_timer.stop();
        if (state != SchedulerState::Failed) {
            state = SchedulerState::Shutdown;
        }
    }

    void request_rescan()
    {
        if (state != SchedulerState::Running) {
            return;
        }
        auto armed = arm_timer(jb::core::Duration::zero());
        if (!armed) {
            fail(std::move(armed).error());
        }
    }

    [[nodiscard]] auto process_cycle() -> SchedulerResult<>
    {
        if (state != SchedulerState::Running) {
            return SchedulerResult<>::success();
        }

        auto cycle = core.process_cycle();
        if (!cycle) {
            auto error = std::move(cycle).error();
            fail(error);
            return SchedulerResult<>::failure(*stored_failure);
        }
        // A start/completion observer may have shut down the scheduler while the core was on the stack.
        if (state == SchedulerState::Failed) {
            return SchedulerResult<>::failure(*stored_failure);
        }
        if (state != SchedulerState::Running) {
            return terminal ? SchedulerResult<>::failure(stopping()) : SchedulerResult<>::success();
        }
        auto armed = arm_next_wake(*cycle);
        if (!armed) {
            auto error = std::move(armed).error();
            fail(error);
            return SchedulerResult<>::failure(*stored_failure);
        }
        return SchedulerResult<>::success();
    }

    void process_wake()
    {
        if (state == SchedulerState::Running) {
            (void)process_cycle();
        }
    }

    [[nodiscard]] auto arm_next_wake(detail::SchedulerCycleResult const& cycle) -> SchedulerResult<>
    {
        wake_timer.stop();
        if (!cycle.next_wake) {
            return SchedulerResult<>::success();
        }

        auto delay = jb::core::Duration::zero();
        if (*cycle.next_wake > cycle.sampled_utc_now) {
            delay = std::chrono::duration_cast<jb::core::Duration>(*cycle.next_wake - cycle.sampled_utc_now);
        }
        auto const cap = std::chrono::duration_cast<jb::core::Duration>(options.wall_clock_recheck);
        return arm_timer(std::min(delay, cap));
    }

    [[nodiscard]] auto arm_timer(jb::core::Duration delay) -> SchedulerResult<>
    {
        wake_timer.start(delay);
        if (!wake_timer.is_active()) {
            return SchedulerResult<>::failure(event_loop_unavailable());
        }
        return SchedulerResult<>::success();
    }

    void fail(jb::core::Error const& error, detail::StorageOperation operation = detail::StorageOperation::Dispatch)
    {
        if (state == SchedulerState::Failed) {
            return;
        }
        state          = SchedulerState::Failed;
        // Preserve the stable identity while keeping backend details out of the public failure signal/result.
        stored_failure = detail::sanitized_storage_error(error, operation);
        shutdown();
        owner->emit(owner->failed, *stored_failure);
    }
};

Scheduler::Scheduler(jb::db::Database&        database,
                     AttributeRegistry const& attributes,
                     CronEngine const&        cron,
                     jb::core::UuidGenerator& uuid_generator,
                     jb::core::TimeSource&    time_source,
                     AttemptExecutor&         executor,
                     SchedulerOptions         options,
                     jb::core::Object*        parent)
    : Object(*new Private{database, attributes, cron, uuid_generator, time_source, executor, options}, parent)
{
    // Complete the owner back-reference only after the Object base owns the private block and tracks its lifetime.
    d_ptr<Private>()->bind_owner(*this);
}

Scheduler::~Scheduler()
{
    // Invalidate executor callbacks before Scheduler's signals and ObjectPrivate block are destroyed.
    d_ptr<Private>()->shutdown();
}

auto Scheduler::start() -> jb::core::Result<void, jb::core::Error>
{
    return d_ptr<Private>()->start();
}

void Scheduler::stop()
{
    d_ptr<Private>()->stop();
}

void Scheduler::shutdown() noexcept
{
    d_ptr<Private>()->shutdown();
}

void Scheduler::request_rescan()
{
    d_ptr<Private>()->request_rescan();
}

auto Scheduler::cancel_run(jb::core::Uuid const& run_id) -> jb::core::Result<CancelRunResult, jb::core::Error>
{
    auto* data = d_ptr<Private>();
    if (data->terminal) {
        return SchedulerResult<CancelRunResult>::failure(stopping());
    }
    if (data->state != SchedulerState::Running) {
        return SchedulerResult<CancelRunResult>::failure(invalid_state());
    }

    auto cancelled = data->core.cancel_run(run_id);
    if (!cancelled && detail::classify_storage_failure(cancelled.error(), detail::StorageOperation::Mutation) ==
                          detail::StorageFailureDisposition::Fatal) {
        data->fail(cancelled.error(), detail::StorageOperation::Mutation);
        return SchedulerResult<CancelRunResult>::failure(*data->stored_failure);
    }
    if (cancelled && cancelled->disposition == CancelDisposition::Completed) {
        data->request_rescan();
    }
    return cancelled;
}

auto Scheduler::state() const noexcept -> SchedulerState
{
    return d_ptr<Private const>()->state;
}

auto Scheduler::failure() const -> std::optional<jb::core::Error>
{
    return d_ptr<Private const>()->stored_failure;
}

} // namespace jb::jobu
