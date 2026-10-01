#include "retention.hpp"

#include "database.hpp"
#include "event_loop.hpp"
#include "object_priv.hpp"
#include "retention_repository_priv.hpp"
#include "storage_failure_priv.hpp"
#include "thread_context.hpp"
#include "time_source.hpp"
#include "timer.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace jb::jobu {

namespace {

using ServiceResult = jb::core::Result<void, jb::core::Error>;

auto invalid_options() -> jb::core::Error
{
    return {.category = jb::core::ErrorCategory::InvalidArgument,
            .code     = "jobu.retention.invalid_options",
            .message  = "Retention policy, timer delays or batch size are invalid"};
}

auto event_loop_unavailable() -> jb::core::Error
{
    return {.category = jb::core::ErrorCategory::Unavailable,
            .code     = "jobu.retention.event_loop_unavailable",
            .message  = "Retention requires a valid owner-thread event loop"};
}

template <typename Duration>
auto valid_delay(Duration delay) noexcept -> bool
{
    if (delay <= Duration::zero() || delay > std::chrono::duration_cast<Duration>(jb::core::Duration::max())) {
        return false;
    }
    auto const timer_delay = std::chrono::duration_cast<jb::core::Duration>(delay);
    return jb::core::Clock::now() <= jb::core::TimePoint::max() - timer_delay;
}

auto failure_origin(jb::core::Error const& error) noexcept -> detail::StorageFailureOrigin
{
    return error.code == "jobu.retention.invalid_relationship" || error.code.starts_with("jobu.storage.")
             ? detail::StorageFailureOrigin::PersistedData
             : detail::StorageFailureOrigin::Operation;
}

} // namespace

struct RetentionService::Private : jb::core::priv::ObjectPrivate {
    /// Identity, rather than an incrementing counter, fences reentrant stop/start without wraparound.
    struct Activation {};

    Private(jb::db::Database&        database_value,
            AttributeRegistry const& attributes,
            jb::core::TimeSource&    clock_value,
            RetentionOptions         options_value)
        : database{database_value}
        , clock{clock_value}
        , options{options_value}
        , repository{database_value, attributes}
    {}

    void bind_owner(RetentionService& value)
    {
        owner = &value;
        timer.timeout.connect(&value, [this] { process_batch(); });
    }

    auto valid_affinity() const noexcept -> bool
    {
        auto* loop = owner->event_loop();
        return loop != nullptr && loop->is_valid() && loop->thread_ctx() == jb::core::ThreadCtx::current() &&
               timer.event_loop() == loop;
    }

    auto arm_timer(jb::core::Duration delay) -> ServiceResult
    {
        if (!valid_affinity()) {
            return ServiceResult::failure(event_loop_unavailable());
        }
        // Timer ultimately adds a steady-clock delay; reject an unrepresentable deadline before that addition.
        if (jb::core::Clock::now() > jb::core::TimePoint::max() - delay) {
            return ServiceResult::failure(invalid_options());
        }
        timer.start(delay);
        if (!timer.is_active()) {
            return ServiceResult::failure(event_loop_unavailable());
        }
        return ServiceResult::success();
    }

    auto start() -> ServiceResult
    {
        if (first_failure) {
            return ServiceResult::failure(*first_failure);
        }
        if (options.default_retention < std::chrono::seconds::zero() || !valid_delay(options.sweep_interval) ||
            !valid_delay(options.inter_batch_delay) || options.batch_size < 1 || options.batch_size > 1000) {
            return ServiceResult::failure(invalid_options());
        }
        if (!valid_affinity()) {
            return ServiceResult::failure(event_loop_unavailable());
        }
        if (activation) {
            return ServiceResult::success();
        }

        auto armed = arm_timer(std::chrono::duration_cast<jb::core::Duration>(options.inter_batch_delay));
        if (!armed) {
            return armed;
        }
        activation = std::make_shared<Activation>();
        return ServiceResult::success();
    }

    void stop() noexcept
    {
        timer.stop();
        activation.reset();
        sweep_now.reset();
        cursor = {};
    }

    void fail(jb::core::Error error)
    {
        if (first_failure) {
            return;
        }
        auto const origin = failure_origin(error);
        error             = detail::sanitized_storage_error(error, detail::StorageOperation::Mutation, origin);
        first_failure     = error;
        stop();
        // Borrow a local owning error: a direct receiver may destroy this Private block and its stored failure.
        owner->emit(owner->failed, error);
    }

    auto poisoned_error() const -> jb::core::Error
    {
        return database.last_error().value_or(jb::core::Error{.category = jb::core::ErrorCategory::Internal,
                                                              .code     = "db.connection_failed",
                                                              .message  = "Database connection is unusable"});
    }

    void process_batch()
    {
        if (!activation) {
            return;
        }
        if (database.is_poisoned()) {
            fail(poisoned_error());
            return;
        }
        if (!sweep_now) {
            sweep_now = clock.utc_now();
        }

        // The repository releases queries and its transaction guard before this result boundary.
        auto result = repository.purge_next_batch(*sweep_now, options.default_retention, options.batch_size, cursor);
        if (database.is_poisoned()) {
            // Rollback failure can poison the connection while unwinding an earlier fatal result.
            // Keep that result's identity/origin; otherwise report the connection's unusable state.
            auto const original_fatal = !result && detail::classify_storage_failure(result.error(),
                                                                                    detail::StorageOperation::Mutation,
                                                                                    failure_origin(result.error())) ==
                                                       detail::StorageFailureDisposition::Fatal;
            fail(original_fatal ? std::move(result).error() : poisoned_error());
            return;
        }
        if (!result) {
            fail(std::move(result).error());
            return;
        }

        cursor              = result->next;
        auto const complete = result->sweep_complete;
        if (complete) {
            sweep_now.reset();
        }

        auto const guard              = lifetime;
        auto const current_activation = activation;
        owner->emit(owner->batch_completed, result->purged);
        // Check the independent lifetime before touching members. A stopped/restarted receiver also
        // invalidates this activation, so its old callback must not replace the new activation's timer.
        if (!guard->alive.load(std::memory_order_acquire) || activation != current_activation) {
            return;
        }

        auto const delay = complete ? std::chrono::duration_cast<jb::core::Duration>(options.sweep_interval)
                                    : std::chrono::duration_cast<jb::core::Duration>(options.inter_batch_delay);
        auto       armed = arm_timer(delay);
        if (!armed) {
            fail(std::move(armed).error());
        }
    }

    RetentionService*                     owner{};
    jb::db::Database&                     database;
    jb::core::TimeSource&                 clock;
    RetentionOptions                      options;
    detail::RetentionRepository           repository;
    jb::core::Timer                       timer;
    std::shared_ptr<Activation>           activation;
    std::optional<jb::core::UtcTimePoint> sweep_now;
    detail::RetentionSweepCursor          cursor;
    std::optional<jb::core::Error>        first_failure;
};

RetentionService::RetentionService(jb::db::Database&        database,
                                   AttributeRegistry const& attributes,
                                   jb::core::TimeSource&    time_source,
                                   RetentionOptions         options,
                                   jb::core::Object*        parent)
    : Object{
          *new Private{database, attributes, time_source, options},
          parent
}
{
    // Bind the public owner only after Object owns and tracks the single Private allocation.
    d_ptr<Private>()->bind_owner(*this);
}

RetentionService::~RetentionService()
{
    d_ptr<Private>()->stop();
}

auto RetentionService::start() -> jb::core::Result<void, jb::core::Error>
{
    return d_ptr<Private>()->start();
}

void RetentionService::stop() noexcept
{
    d_ptr<Private>()->stop();
}

} // namespace jb::jobu
