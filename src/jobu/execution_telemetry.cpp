#include "execution_telemetry.hpp"

#include "database.hpp"
#include "event_loop.hpp"
#include "execution_telemetry_priv.hpp"
#include "object_priv.hpp"
#include "storage_failure_priv.hpp"
#include "thread_context.hpp"
#include "time_source.hpp"
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
        .error = {.category = jb::core::ErrorCategory::Internal,
                  .code     = "jobu.telemetry.invalid_state",
                  .message  = "Telemetry activation or sample is invalid",
                  .detail   = "reason=" + std::string{reason}}
    };
}

auto invalid_options() -> jb::core::Error
{
    return {.category = jb::core::ErrorCategory::InvalidArgument,
            .code     = "jobu.telemetry.invalid_options",
            .message  = "Telemetry checkpoint interval or batch size is invalid"};
}

auto arithmetic_failure(std::string_view code) -> detail::TelemetryFailure
{
    return {
        .error = {.category = jb::core::ErrorCategory::Internal,
                  .code     = std::string{code},
                  .message  = "Telemetry monotonic elapsed time cannot be represented"}
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

    auto valid_affinity() const noexcept -> bool
    {
        auto* loop = owner->event_loop();
        return owner->thread_ctx() == construction_thread && construction_thread == jb::core::ThreadCtx::current() &&
               loop != nullptr && loop->is_valid() && loop->thread_ctx() == construction_thread;
    }

    auto poisoned_failure() const -> detail::TelemetryFailure
    {
        return {.error = database.last_error().value_or(jb::core::Error{.category = jb::core::ErrorCategory::Internal,
                                                                        .code     = "db.connection_failed",
                                                                        .message = "Database connection is unusable"})};
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

        epoch          = *generated;
        origin         = clock.monotonic_now();
        last_monotonic = origin;
        state          = State::Active;
        return ServiceResult::success();
    }

    auto sample() -> detail::TelemetryResult<detail::TelemetrySample>
    {
        auto ready = active();
        if (!ready) {
            return detail::TelemetryResult<detail::TelemetrySample>::failure(std::move(ready).error());
        }

        // Invalidate even equal-tick boundaries by identity, without a wrapping generation counter.
        latest_sample.reset();
        auto const utc_now = clock.utc_now();
        auto const now     = clock.monotonic_now();
        if (now < last_monotonic) {
            return detail::TelemetryResult<detail::TelemetrySample>::failure(
                arithmetic_failure("jobu.telemetry.clock_regression"));
        }
        auto tick = elapsed_microseconds(origin, now);
        if (!tick) {
            return detail::TelemetryResult<detail::TelemetrySample>::failure(std::move(tick).error());
        }

        last_monotonic = now;
        latest_sample  = std::make_shared<detail::WaitSample const>(
            detail::WaitSample{.utc_now = utc_now, .epoch = epoch, .tick_us = *tick});
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

    void request_stop() noexcept
    {
        if (state != State::Failed) {
            state = State::Stopped;
        }
        latest_sample.reset();
    }

    void report_failure(detail::TelemetryFailure failure, detail::StorageOperation operation)
    {
        if (first_failure) {
            return;
        }

        // The caller has unwound its SQL scopes. Clock/accounting errors explicitly fail
        // this owner; SQL classification alone does not make those errors fatal.
        auto const original_fatal = failure.error.code.starts_with("jobu.telemetry.") ||
                                    detail::classify_storage_failure(failure.error, operation, failure.origin) ==
                                        detail::StorageFailureDisposition::Fatal;
        if (database.is_poisoned() && !original_fatal) {
            failure = poisoned_failure();
        }
        failure.error = detail::sanitized_storage_error(failure.error, operation, failure.origin);
        first_failure = failure;
        state         = State::Failed;
        request_stop();

        // This local owning value survives direct receiver destruction. Emission is the
        // final operation: never read the public owner or this Private block afterwards.
        owner->emit(owner->failed, failure.error);
    }

    ExecutionTelemetry*                     owner{};
    jb::db::Database&                       database;
    AttributeRegistry const&                attributes;
    jb::core::TimeSource&                   clock;
    jb::core::UuidGenerator&                generator;
    TelemetryOptions                        options;
    detail::WaitRepository                  repository;
    jb::core::ThreadCtx const*              construction_thread;
    State                                   state{State::Fresh};
    jb::core::Uuid                          epoch;
    jb::core::TimePoint                     origin;
    jb::core::TimePoint                     last_monotonic;
    detail::TelemetrySample                 latest_sample;
    std::optional<detail::TelemetryFailure> first_failure;
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
    d_ptr<Private>()->owner = this;
}

ExecutionTelemetry::~ExecutionTelemetry()
{
    d_ptr<Private>()->request_stop();
}

auto ExecutionTelemetry::start() -> jb::core::Result<void, jb::core::Error>
{
    return d_ptr<Private>()->start();
}

void ExecutionTelemetry::request_stop() noexcept
{
    d_ptr<Private>()->request_stop();
}

namespace detail {

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
