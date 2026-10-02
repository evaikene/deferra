#include "execution_telemetry.hpp"

#include "attempt_executor.hpp"
#include "database.hpp"
#include "event_loop.hpp"
#include "execution_telemetry_priv.hpp"
#include "object_priv.hpp"
#include "storage_failure_priv.hpp"
#include "thread_context.hpp"
#include "time_source.hpp"
#include "timer.hpp"
#include "transaction.hpp"
#include "uuid.hpp"
#include "wait_repository_priv.hpp"

#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <ratio>
#include <string>
#include <string_view>
#include <utility>

namespace jb::jobu {

namespace {

using ServiceResult = jb::core::Result<void, jb::core::Error>;

auto invalid_state(std::string_view reason) -> detail::TelemetryFailure
{
    return {
        .error =
            {
                    .category = jb::core::ErrorCategory::Internal,
                    .code     = "jobu.telemetry.invalid_state",
                    .message  = "Telemetry activation or sample is invalid",
                    .detail   = "reason=" + std::string{reason},
                    },
    };
}

auto invalid_options() -> jb::core::Error
{
    return {
        .category = jb::core::ErrorCategory::InvalidArgument,
        .code     = "jobu.telemetry.invalid_options",
        .message  = "Telemetry checkpoint interval or batch size is invalid",
    };
}

auto arithmetic_failure(std::string_view code) -> detail::TelemetryFailure
{
    return {
        .error =
            {
                    .category = jb::core::ErrorCategory::Internal,
                    .code     = std::string{code},
                    .message  = "Telemetry monotonic elapsed time cannot be represented",
                    },
    };
}

auto elapsed_microseconds(jb::core::TimePoint origin, jb::core::TimePoint now) -> detail::TelemetryResult<std::int64_t>
{
    // Ordering is checked by the owner. A negative origin can still make a positive
    // elapsed duration overflow the clock's signed representation before conversion.
    auto const origin_duration = origin.time_since_epoch();
    if (origin_duration < jb::core::Duration::zero() &&
        now.time_since_epoch() > jb::core::Duration::max() + origin_duration) {
        return detail::TelemetryResult<std::int64_t>::failure(arithmetic_failure("jobu.telemetry.counter_overflow"));
    }

    auto const elapsed = (now - origin).count();
    using Conversion   = std::ratio_divide<jb::core::Duration::period, std::chrono::microseconds::period>;
    if (!std::in_range<std::int64_t>(elapsed) || elapsed > std::numeric_limits<std::int64_t>::max() / Conversion::num) {
        return detail::TelemetryResult<std::int64_t>::failure(arithmetic_failure("jobu.telemetry.counter_overflow"));
    }
    return detail::TelemetryResult<std::int64_t>::success(static_cast<std::int64_t>(elapsed) * Conversion::num /
                                                          Conversion::den);
}

} // namespace

struct ExecutionTelemetry::Private : jb::core::priv::ObjectPrivate {
    enum class State : std::uint8_t {
        Fresh,
        Active,
        Stopped,
        Failed,
    };

    Private(jb::db::Database&        database_value,
            AttributeRegistry const& attributes_value,
            jb::core::TimeSource&    clock_value,
            jb::core::UuidGenerator& generator_value,
            TelemetryOptions         options_value)
        : database{database_value}
        , attributes{attributes_value}
        , clock{clock_value}
        , generator{generator_value}
        , options{options_value}
        , repository{database_value}
        , construction_thread{jb::core::ThreadCtx::current()}
    {}

    void bind_owner(ExecutionTelemetry& value)
    {
        owner = &value;
        timer.timeout.connect(&value, [this] { checkpoint(); });
    }

    auto valid_affinity() const noexcept -> bool
    {
        auto* loop = owner->event_loop();
        return owner->thread_ctx() == construction_thread && construction_thread == jb::core::ThreadCtx::current() &&
               loop != nullptr && loop->is_valid() && loop->thread_ctx() == construction_thread &&
               timer.event_loop() == loop;
    }

    auto arm_timer(jb::core::Duration delay) -> detail::TelemetryResult<void>
    {
        if (!valid_affinity()) {
            return detail::TelemetryResult<void>::failure(invalid_state("timer_affinity"));
        }
        if (jb::core::Clock::now() > jb::core::TimePoint::max() - delay) {
            return detail::TelemetryResult<void>::failure(arithmetic_failure("jobu.telemetry.counter_overflow"));
        }
        timer.start(delay);
        if (!timer.is_active()) {
            return detail::TelemetryResult<void>::failure(invalid_state("timer_unavailable"));
        }
        return detail::TelemetryResult<void>::success();
    }

    auto poisoned_failure() const -> detail::TelemetryFailure
    {
        return {
            .error = database.last_error().value_or(jb::core::Error{
                .category = jb::core::ErrorCategory::Internal,
                .code     = "db.connection_failed",
                .message  = "Database connection is unusable",
            }),
        };
    }

    auto active() const -> detail::TelemetryResult<void>
    {
        if (!valid_affinity()) {
            return detail::TelemetryResult<void>::failure(invalid_state("owner_affinity"));
        }
        if (first_failure) {
            return detail::TelemetryResult<void>::failure(*first_failure);
        }
        if (state != State::Active) {
            return detail::TelemetryResult<void>::failure(invalid_state("inactive_owner"));
        }
        if (database.is_poisoned()) {
            return detail::TelemetryResult<void>::failure(poisoned_failure());
        }
        return detail::TelemetryResult<void>::success();
    }

    auto start() -> ServiceResult
    {
        if (!valid_affinity()) {
            return ServiceResult::failure(invalid_state("owner_affinity").error);
        }
        if (first_failure) {
            return ServiceResult::failure(first_failure->error);
        }
        if (options.checkpoint_interval < std::chrono::seconds{1} ||
            options.checkpoint_interval > std::chrono::seconds{86400} || options.batch_size < 1 ||
            options.batch_size > 1000) {
            return ServiceResult::failure(invalid_options());
        }
        if (state == State::Stopped) {
            return ServiceResult::failure(invalid_state("stopped_owner").error);
        }
        if (database.is_poisoned()) {
            return ServiceResult::failure(
                detail::sanitized_storage_error(poisoned_failure().error, detail::StorageOperation::Mutation));
        }
        if (state == State::Active) {
            return ServiceResult::success();
        }

        auto generated = generator.generate();
        if (!generated) {
            return ServiceResult::failure(
                detail::sanitized_storage_error(generated.error(), detail::StorageOperation::Mutation));
        }
        if (generated->is_nil()) {
            return ServiceResult::failure(invalid_state("nil_activation_epoch").error);
        }

        auto armed = arm_timer(std::chrono::duration_cast<jb::core::Duration>(options.checkpoint_interval));
        if (!armed) {
            return ServiceResult::failure(std::move(armed).error().error);
        }

        epoch          = *generated;
        origin         = clock.monotonic_now();
        last_monotonic = origin;
        state          = State::Active;
        return ServiceResult::success();
    }

    auto capture_sample() -> detail::TelemetryResult<detail::WaitSample>
    {
        auto ready = active();
        if (!ready) {
            return detail::TelemetryResult<detail::WaitSample>::failure(std::move(ready).error());
        }

        auto const utc_now = clock.utc_now();
        auto const now     = clock.monotonic_now();
        if (now < last_monotonic) {
            return detail::TelemetryResult<detail::WaitSample>::failure(
                arithmetic_failure("jobu.telemetry.clock_regression"));
        }
        auto tick = elapsed_microseconds(origin, now);
        if (!tick) {
            return detail::TelemetryResult<detail::WaitSample>::failure(std::move(tick).error());
        }

        last_monotonic = now;
        return detail::TelemetryResult<detail::WaitSample>::success(
            {.utc_now = utc_now, .epoch = epoch, .tick_us = *tick, .monotonic_now = now});
    }

    auto sample() -> detail::TelemetryResult<detail::TelemetrySample>
    {
        // Invalidate even equal-tick boundaries by identity, without a wrapping generation counter.
        latest_sample.reset();
        auto captured = capture_sample();
        if (!captured) {
            return detail::TelemetryResult<detail::TelemetrySample>::failure(std::move(captured).error());
        }
        latest_sample = std::make_shared<detail::WaitSample const>(*captured);
        return detail::TelemetryResult<detail::TelemetrySample>::success(latest_sample);
    }

    auto validate_sample(detail::TelemetrySample const& sample_value) const -> detail::TelemetryResult<void>
    {
        auto ready = active();
        if (!ready) {
            return ready;
        }
        if (!sample_value || sample_value != latest_sample) {
            return detail::TelemetryResult<void>::failure(invalid_state("foreign_or_stale_sample"));
        }
        return detail::TelemetryResult<void>::success();
    }

    void disarm() noexcept
    {
        timer.stop();
        if (state != State::Failed) {
            state = State::Stopped;
        }
        latest_sample.reset();
        checkpoint_after.reset();
    }

    void request_stop() noexcept
    {
        if (state == State::Active) {
            // Capture only once, even when stop is requested while a caller's transaction
            // is active. No error is emitted on that stack; finish_stop reports it after unwind.
            auto captured = capture_sample();
            if (captured) {
                stop_sample = *captured;
            }
            else {
                stop_failure = std::move(captured).error();
            }
        }
        disarm();
    }

    struct AccountingPage {
        std::optional<jb::core::Uuid> after;
        bool                          complete{false};
    };

    auto accounting_page(detail::WaitSample const& boundary, std::optional<jb::core::Uuid> after, bool close)
        -> detail::TelemetryResult<AccountingPage>
    {
        using PageResult = detail::TelemetryResult<AccountingPage>;
        auto transaction = jb::db::Transaction::begin(database);
        if (!transaction) {
            return PageResult::failure({.error = std::move(transaction).error()});
        }
        auto ids = repository.list_open(options.batch_size, after);
        if (!ids) {
            return PageResult::failure(std::move(ids).error());
        }
        for (auto const& id : *ids) {
            auto result = close ? repository.settle(id, boundary) : repository.rebase(id, boundary);
            if (!result) {
                return PageResult::failure(std::move(result).error());
            }
        }

        // A stop requested during a checkpoint must roll the page back, retaining its old
        // tails for healthy settlement at the captured boundary. Stop settlement itself runs
        // with ordinary accounting disabled and never reopens an interval.
        if (!close && state != State::Active) {
            return PageResult::failure({
                .error =
                    {
                            .category = jb::core::ErrorCategory::Cancelled,
                            .code     = "jobu.telemetry.stopping",
                            .message  = "Telemetry checkpoint was stopped",
                            },
            });
        }
        auto committed = transaction->commit();
        if (!committed) {
            return PageResult::failure({.error = std::move(committed).error()});
        }
        return PageResult::success({
            .after    = ids->empty() ? after : std::optional{ids->back()},
            .complete = ids->size() < options.batch_size,
        });
    }

    void checkpoint()
    {
        if (state != State::Active) {
            return;
        }

        // Each page gets a fresh boundary. A mutation between yields may have opened an
        // interval at a later tick than the preceding page; a sweep-wide sample would regress.
        auto boundary = sample();
        if (!boundary) {
            report_failure(std::move(boundary).error(), detail::StorageOperation::Mutation);
            return;
        }
        auto page = accounting_page(**boundary, checkpoint_after, false);
        if (!page) {
            if (page.error().error.code == "jobu.telemetry.stopping") {
                if (database.is_poisoned()) {
                    report_failure(poisoned_failure(), detail::StorageOperation::Mutation);
                }
                return;
            }
            report_failure(std::move(page).error(), detail::StorageOperation::Mutation);
            return;
        }
        if (state != State::Active) {
            return;
        }

        checkpoint_after = page->complete ? std::nullopt : page->after;
        auto const delay = page->complete
                             ? std::chrono::duration_cast<jb::core::Duration>(options.checkpoint_interval)
                             : std::chrono::duration_cast<jb::core::Duration>(std::chrono::milliseconds{10});
        auto       armed = arm_timer(delay);
        if (!armed) {
            report_failure(std::move(armed).error(), detail::StorageOperation::Mutation);
        }
    }

    auto finish_stop() -> ServiceResult
    {
        if (first_failure) {
            return ServiceResult::failure(first_failure->error);
        }
        if (state != State::Stopped) {
            return ServiceResult::failure(invalid_state("finish_before_stop").error);
        }

        auto result = detail::TelemetryResult<void>::success();
        if (stop_failure) {
            result = detail::TelemetryResult<void>::failure(*stop_failure);
        }
        else if (stop_sample) {
            result = finish_batches();
        }
        if (!result) {
            // Preserve an owning return error before a direct receiver can destroy this owner.
            auto failure = normalize_failure(std::move(result).error(), detail::StorageOperation::Mutation);
            auto error   = failure.error;
            report_failure(std::move(failure), detail::StorageOperation::Mutation);
            return ServiceResult::failure(std::move(error));
        }
        stop_sample.reset();
        return ServiceResult::success();
    }

    auto finish_batches() -> detail::TelemetryResult<void>
    {
        if (!valid_affinity()) {
            return detail::TelemetryResult<void>::failure(invalid_state("owner_affinity"));
        }
        if (database.is_poisoned()) {
            return detail::TelemetryResult<void>::failure(poisoned_failure());
        }

        std::optional<jb::core::Uuid> after;
        for (;;) {
            auto page = accounting_page(*stop_sample, after, true);
            if (!page) {
                return detail::TelemetryResult<void>::failure(std::move(page).error());
            }
            if (page->complete) {
                return detail::TelemetryResult<void>::success();
            }
            after = page->after;
        }
    }

    auto normalize_failure(detail::TelemetryFailure failure, detail::StorageOperation operation) const
        -> detail::TelemetryFailure
    {
        // SQL scopes have unwound. Preserve an original fatal identity when failed rollback
        // also poisoned the connection; otherwise the poisoned connection takes precedence.
        auto const original_fatal = failure.error.code.starts_with("jobu.telemetry.") ||
                                    detail::classify_storage_failure(failure.error, operation, failure.origin) ==
                                        detail::StorageFailureDisposition::Fatal;
        if (database.is_poisoned() && !original_fatal) {
            failure = poisoned_failure();
        }
        failure.error = detail::sanitized_storage_error(failure.error, operation, failure.origin);
        return failure;
    }

    void report_failure(detail::TelemetryFailure failure, detail::StorageOperation operation)
    {
        if (first_failure) {
            return;
        }

        // Clock/accounting errors explicitly fail this owner even when SQL classification
        // alone would call them an ordinary operation error.
        failure       = normalize_failure(std::move(failure), operation);
        first_failure = failure;
        state         = State::Failed;
        request_stop();

        // This local owning value survives direct receiver destruction. Emission is the
        // final operation: never read the public owner or this Private block afterwards.
        owner->emit(owner->failed, failure.error);
    }

    ExecutionTelemetry*                      owner{};
    jb::db::Database&                        database;
    AttributeRegistry const&                 attributes;
    jb::core::TimeSource&                    clock;
    jb::core::UuidGenerator&                 generator;
    TelemetryOptions                         options;
    detail::WaitRepository                   repository;
    jb::core::Timer                          timer;
    jb::core::ThreadCtx const*               construction_thread;
    State                                    state{State::Fresh};
    jb::core::Uuid                           epoch;
    jb::core::TimePoint                      origin;
    jb::core::TimePoint                      last_monotonic;
    detail::TelemetrySample                  latest_sample;
    std::optional<jb::core::Uuid>            checkpoint_after;
    std::optional<detail::WaitSample>        stop_sample;
    std::optional<detail::TelemetryFailure>  stop_failure;
    std::optional<detail::TelemetryFailure>  first_failure;
    std::optional<detail::AvailableJobTypes> available;
    AttemptExecutor const*                   registered_executor{};
};

ExecutionTelemetry::ExecutionTelemetry(jb::db::Database&        database,
                                       AttributeRegistry const& attributes,
                                       jb::core::TimeSource&    time_source,
                                       jb::core::UuidGenerator& uuid_generator,
                                       TelemetryOptions         options,
                                       jb::core::Object*        parent)
    : Object{
          *new Private{database, attributes, time_source, uuid_generator, options},
          parent
}
{
    // Bind only after Object has taken ownership of the single derived Private block.
    d_ptr<Private>()->bind_owner(*this);
}

ExecutionTelemetry::~ExecutionTelemetry()
{
    d_ptr<Private>()->disarm();
}

auto ExecutionTelemetry::start() -> jb::core::Result<void, jb::core::Error>
{
    return d_ptr<Private>()->start();
}

void ExecutionTelemetry::request_stop() noexcept
{
    d_ptr<Private>()->request_stop();
}

auto ExecutionTelemetry::finish_stop() -> jb::core::Result<void, jb::core::Error>
{
    return d_ptr<Private>()->finish_stop();
}

namespace detail {

auto TelemetryAccess::register_executor(ExecutionTelemetry&      owner,
                                        jb::db::Database&        database,
                                        AttributeRegistry const& attributes,
                                        jb::core::TimeSource&    clock,
                                        AttemptExecutor const&   executor) -> TelemetryResult<void>
{
    auto* data = owner.d_ptr<ExecutionTelemetry::Private>();
    if (!data->valid_affinity() || &data->database != &database || &data->attributes != &attributes ||
        &data->clock != &clock || data->state != ExecutionTelemetry::Private::State::Fresh ||
        (data->registered_executor && data->registered_executor != &executor)) {
        return TelemetryResult<void>::failure(invalid_state("executor_registration"));
    }
    AvailableJobTypes const types{
        .cli  = executor.is_available(JobType::Cli),
        .http = executor.is_available(JobType::Http),
    };
    if (data->available && *data->available != types) {
        return TelemetryResult<void>::failure(invalid_state("executor_registration_changed"));
    }
    data->available           = types;
    data->registered_executor = &executor;
    return TelemetryResult<void>::success();
}

auto TelemetryAccess::available_types(ExecutionTelemetry& owner) -> TelemetryResult<AvailableJobTypes>
{
    auto* data  = owner.d_ptr<ExecutionTelemetry::Private>();
    // Runtime can wire a dormant owner before the activation stage. Its registered
    // executor snapshot already applies, while mutation boundaries remain unmeasured.
    auto  ready = data->state == ExecutionTelemetry::Private::State::Fresh && data->valid_affinity()
                    ? TelemetryResult<void>::success()
                    : data->active();
    if (!ready) {
        return TelemetryResult<AvailableJobTypes>::failure(std::move(ready).error());
    }
    if (!data->available) {
        return TelemetryResult<AvailableJobTypes>::failure(invalid_state("missing_executor_registration"));
    }
    return TelemetryResult<AvailableJobTypes>::success(*data->available);
}

auto TelemetryAccess::mutation_boundary(ExecutionTelemetry*      owner,
                                        jb::db::Database&        database,
                                        AttributeRegistry const& attributes,
                                        jb::core::TimeSource&    clock) -> TelemetryResult<MutationTiming>
{
    MutationTiming boundary{.owner = owner, .database = &database};
    if (owner) {
        auto* data = owner->d_ptr<ExecutionTelemetry::Private>();
        if (!data->valid_affinity() || &data->database != &database || &data->attributes != &attributes ||
            &data->clock != &clock) {
            return TelemetryResult<MutationTiming>::failure(invalid_state("mixed_collaborators"));
        }
        if (data->first_failure) {
            return TelemetryResult<MutationTiming>::failure(*data->first_failure);
        }
        if (data->state == ExecutionTelemetry::Private::State::Active) {
            auto types = available_types(*owner);
            if (!types) {
                return TelemetryResult<MutationTiming>::failure(std::move(types).error());
            }
            auto sampled = sample(*owner);
            if (!sampled) {
                return TelemetryResult<MutationTiming>::failure(std::move(sampled).error());
            }
            boundary.sample    = std::move(*sampled);
            boundary.utc_now   = boundary.sample->utc_now;
            boundary.available = *types;
            return TelemetryResult<MutationTiming>::success(std::move(boundary));
        }
    }
    boundary.utc_now = clock.utc_now();
    return TelemetryResult<MutationTiming>::success(std::move(boundary));
}

auto TelemetryAccess::lifetime(ExecutionTelemetry& owner) -> std::weak_ptr<jb::core::priv::ObjectLifetime>
{
    return owner.d_ptr<ExecutionTelemetry::Private>()->lifetime;
}

auto TelemetryAccess::deliver_delayed(ExecutionTelemetry& owner, DelayedRun value) -> bool
{
    auto* data = owner.d_ptr<ExecutionTelemetry::Private>();
    if (data->state != ExecutionTelemetry::Private::State::Active) {
        return false;
    }
    auto guard = data->lifetime;
    owner.emit(owner.delayed, value);
    // The owning argument survives direct destruction. Never inspect the private block until
    // its lifetime confirms that this receiver left the owner alive.
    return guard->alive.load() && data->state == ExecutionTelemetry::Private::State::Active;
}

auto TelemetryAccess::sample_is_current(ExecutionTelemetry& owner, TelemetrySample const& sample) -> bool
{
    auto* data = owner.d_ptr<ExecutionTelemetry::Private>();
    return data->state == ExecutionTelemetry::Private::State::Active && data->latest_sample == sample;
}

auto MutationTiming::measurement() const noexcept -> InitialRunMeasurement
{
    return sample ? InitialRunMeasurement::Complete : InitialRunMeasurement::Unmeasured;
}

namespace {

auto visit_scope(MutationTiming const& boundary, EligibilityScope scope, bool reopen) -> TelemetryResult<void>
{
    std::optional<jb::core::Uuid> after;
    WaitRepository                repository{*boundary.database};
    if (!boundary.sample) {
        for (;;) {
            auto ids = list_timing_scope(*boundary.database, scope, after);
            if (!ids) {
                return TelemetryResult<void>::failure(std::move(ids).error());
            }
            if (ids->empty()) {
                return TelemetryResult<void>::success();
            }
            for (auto const& id : *ids) {
                auto timing = repository.read(id);
                if (!timing) {
                    return TelemetryResult<void>::failure(std::move(timing).error());
                }
                if (timing->open_epoch) {
                    auto failure   = invalid_state("open_interval_without_owner");
                    failure.origin = StorageFailureOrigin::PersistedData;
                    return TelemetryResult<void>::failure(std::move(failure));
                }
            }
            after = ids->back();
        }
    }

    for (;;) {
        auto rows = list_eligibility(*boundary.database, scope, after);
        if (!rows) {
            return TelemetryResult<void>::failure(std::move(rows).error());
        }
        if (rows->empty()) {
            return TelemetryResult<void>::success();
        }
        for (auto const& row : *rows) {
            auto accounted = reopen && row.eligible(boundary.utc_now, boundary.available)
                               ? TelemetryAccess::open_interval(*boundary.owner, row.id, boundary.sample)
                               : TelemetryAccess::settle(*boundary.owner, row.id, boundary.sample);
            if (!accounted) {
                return TelemetryResult<void>::failure(std::move(accounted).error());
            }
        }
        after = rows->back().id;
    }
}

} // namespace

auto MutationTiming::settle_scope(EligibilityScope scope) const -> TelemetryResult<void>
{
    return visit_scope(*this, scope, false);
}

auto MutationTiming::reconcile_scope(EligibilityScope scope) const -> TelemetryResult<void>
{
    return visit_scope(*this, scope, true);
}

auto MutationTiming::validate_closed_run(jb::core::Uuid const& run_id) const -> TelemetryResult<void>
{
    WaitRepository repository{*database};
    auto           timing = repository.read(run_id);
    if (!timing) {
        return TelemetryResult<void>::failure(std::move(timing).error());
    }
    if (timing->open_epoch) {
        auto failure   = invalid_state("open_interval_without_owner");
        failure.origin = StorageFailureOrigin::PersistedData;
        return TelemetryResult<void>::failure(std::move(failure));
    }
    return TelemetryResult<void>::success();
}

auto MutationTiming::claim_run(JobRun const& run, std::chrono::milliseconds threshold) const
    -> TelemetryResult<std::optional<DelayedRun>>
{
    using ClaimResult = TelemetryResult<std::optional<DelayedRun>>;
    if (!sample) {
        auto valid = validate_closed_run(run.id);
        return valid ? ClaimResult::success(std::nullopt) : ClaimResult::failure(std::move(valid).error());
    }

    // Selection is already validated by the scheduler. Opening at this boundary preserves
    // an existing tail and marks a late first observation Partial, even with zero new wait.
    auto opened = TelemetryAccess::open_interval(*owner, run.id, sample);
    if (!opened) {
        return ClaimResult::failure(std::move(opened).error());
    }
    auto closed = TelemetryAccess::settle(*owner, run.id, sample);
    if (!closed) {
        return ClaimResult::failure(std::move(closed).error());
    }
    WaitRepository repository{*database};
    auto           warning = repository.claim_warning(run.id, *sample, threshold);
    if (!warning) {
        return ClaimResult::failure(std::move(warning).error());
    }
    if (!warning->claimed) {
        return ClaimResult::success(std::nullopt);
    }
    return ClaimResult::success(DelayedRun{
        .run_id        = run.id,
        .job_id        = run.job_id,
        .queue_id      = run.queue_id,
        .type          = run.type,
        .runnable_wait = warning->known_wait,
        .threshold     = threshold,
    });
}

namespace {

auto warning_deadline(WaitSample const& sample, std::chrono::microseconds delay) -> std::optional<jb::core::TimePoint>
{
    using Conversion = std::ratio_divide<std::chrono::microseconds::period, jb::core::Duration::period>;
    // A valid policy need not fit into the remaining steady-clock range. Omit an unreachable
    // deadline rather than wrapping it; an ordinary later observation still evaluates the policy.
    if (delay.count() > jb::core::Duration::max().count() / Conversion::num) {
        return std::nullopt;
    }
    auto const ticks        = delay.count() * Conversion::num;
    auto const native_delay = jb::core::Duration{(ticks / Conversion::den) + (ticks % Conversion::den != 0 ? 1 : 0)};
    if (sample.monotonic_now.time_since_epoch() > jb::core::Duration::max() - native_delay) {
        return std::nullopt;
    }
    return sample.monotonic_now + native_delay;
}

} // namespace

auto MutationTiming::observe_page(std::optional<jb::core::Uuid> after) const -> TelemetryResult<ObservationPage>
{
    if (!sample) {
        return TelemetryResult<ObservationPage>::success({});
    }
    if (!TelemetryAccess::sample_is_current(*owner, sample)) {
        return TelemetryResult<ObservationPage>::failure(invalid_state("foreign_or_stale_sample"));
    }
    auto rows = list_eligibility(*database, {.kind = EligibilityScope::Kind::All}, after);
    if (!rows) {
        return TelemetryResult<ObservationPage>::failure(std::move(rows).error());
    }
    ObservationPage effects;
    WaitRepository  repository{*database};
    for (auto const& row : *rows) {
        auto const eligible = row.eligible(utc_now, available);
        auto       timing   = eligible ? TelemetryAccess::open_interval(*owner, row.id, sample)
                                       : TelemetryAccess::settle(*owner, row.id, sample);
        if (!timing) {
            return TelemetryResult<ObservationPage>::failure(std::move(timing).error());
        }
        if (!eligible) {
            continue;
        }
        auto warning = repository.claim_warning(row.id, *sample, row.warning_threshold);
        if (!warning) {
            return TelemetryResult<ObservationPage>::failure(std::move(warning).error());
        }
        if (warning->claimed) {
            effects.delayed.push_back({
                .run_id        = row.id,
                .job_id        = row.job_id,
                .queue_id      = row.queue_id,
                .type          = row.type,
                .runnable_wait = warning->known_wait,
                .threshold     = row.warning_threshold,
            });
        }
        if (warning->until_warning) {
            auto deadline = warning_deadline(*sample, *warning->until_warning);
            if (deadline && (!effects.next_warning || *deadline < *effects.next_warning)) {
                effects.next_warning = deadline;
            }
        }
    }
    if (!rows->empty()) {
        effects.after = rows->back().id;
    }
    return TelemetryResult<ObservationPage>::success(std::move(effects));
}

auto TelemetryAccess::sample(ExecutionTelemetry& owner) -> TelemetryResult<TelemetrySample>
{
    return owner.d_ptr<ExecutionTelemetry::Private>()->sample();
}

auto TelemetryAccess::initial_measurement(ExecutionTelemetry& owner) -> TelemetryResult<InitialRunMeasurement>
{
    auto ready = owner.d_ptr<ExecutionTelemetry::Private>()->active();
    if (!ready) {
        return TelemetryResult<InitialRunMeasurement>::failure(std::move(ready).error());
    }
    return TelemetryResult<InitialRunMeasurement>::success(InitialRunMeasurement::Complete);
}

auto TelemetryAccess::open_interval(ExecutionTelemetry&    owner,
                                    jb::core::Uuid const&  run_id,
                                    TelemetrySample const& sample) -> TelemetryResult<WaitTiming>
{
    auto* data  = owner.d_ptr<ExecutionTelemetry::Private>();
    auto  valid = data->validate_sample(sample);
    if (!valid) {
        return TelemetryResult<WaitTiming>::failure(std::move(valid).error());
    }
    return data->repository.open_interval(run_id, *sample);
}

auto TelemetryAccess::settle(ExecutionTelemetry& owner, jb::core::Uuid const& run_id, TelemetrySample const& sample)
    -> TelemetryResult<WaitTiming>
{
    auto* data  = owner.d_ptr<ExecutionTelemetry::Private>();
    auto  valid = data->validate_sample(sample);
    if (!valid) {
        return TelemetryResult<WaitTiming>::failure(std::move(valid).error());
    }
    return data->repository.settle(run_id, *sample);
}

auto TelemetryAccess::rebase(ExecutionTelemetry& owner, jb::core::Uuid const& run_id, TelemetrySample const& sample)
    -> TelemetryResult<WaitTiming>
{
    auto* data  = owner.d_ptr<ExecutionTelemetry::Private>();
    auto  valid = data->validate_sample(sample);
    if (!valid) {
        return TelemetryResult<WaitTiming>::failure(std::move(valid).error());
    }
    return data->repository.rebase(run_id, *sample);
}

void TelemetryAccess::report_failure(ExecutionTelemetry& owner, TelemetryFailure failure, StorageOperation operation)
{
    owner.d_ptr<ExecutionTelemetry::Private>()->report_failure(std::move(failure), operation);
}

} // namespace detail
} // namespace jb::jobu
