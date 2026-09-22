#include "scheduler_dispatch_priv.hpp"

#include "attempt_repository_priv.hpp"
#include "database.hpp"
#include "payload_template_priv.hpp"
#include "scheduler_repository_priv.hpp"
#include "storage_failure_priv.hpp"
#include "transaction.hpp"

#include <utility>

namespace jb::jobu::detail {

namespace {

template <typename T>
using DispatchResult = jb::core::Result<T, jb::core::Error>;

auto rollback_ineligible(jb::db::Transaction& transaction) -> DispatchResult<std::optional<DispatchStart>>
{
    auto rolled_back = transaction.rollback();
    if (!rolled_back) {
        return DispatchResult<std::optional<DispatchStart>>::failure(std::move(rolled_back).error());
    }
    return DispatchResult<std::optional<DispatchStart>>::success(std::nullopt);
}

auto terminal_start_failure(AttemptKey key, jb::core::Error const& error) -> AttemptCompletion
{
    auto error_code = jb::core::JsonValue{};
    error_code.data = error.code;
    auto message    = jb::core::JsonValue{};
    message.data    = error.message;
    auto result     = jb::core::JsonValue{};
    result.data     = jb::core::JsonValue::Object{
        {"error_code", std::move(error_code)},
        {"message",    std::move(message)   },
    };
    return {
        .key                 = key,
        .outcome             = AttemptOutcome::Failed,
        .failure_disposition = FailureDisposition::Terminal,
        .result              = std::move(result),
    };
}

auto dispatch_selected_impl(jb::db::Database&        database,
                            AttributeRegistry const& attributes,
                            AttemptExecutor&         executor,
                            SecretProvider&          secrets,
                            jb::core::Uuid const&    run_id,
                            jb::core::UtcTimePoint   started_at,
                            AttemptCompletionHandler completion)
    -> jb::core::Result<std::optional<DispatchStart>, jb::core::Error>
{
    // Revalidate the optimistic candidate under an immediate transaction and make the attempt plus run transition one
    // durable start boundary. Expected eligibility loss rolls back as a normal skip.
    auto transaction = jb::db::Transaction::begin(database);
    if (!transaction) {
        return DispatchResult<std::optional<DispatchStart>>::failure(std::move(transaction).error());
    }
    auto guard = std::move(transaction).value();

    SchedulerRepository repository{database, attributes};
    auto                context = repository.find_dispatch_context(run_id, started_at);
    if (!context) {
        return DispatchResult<std::optional<DispatchStart>>::failure(std::move(context).error());
    }
    if (!context->has_value() || !executor.is_available(context->value().run.type)) {
        return rollback_ineligible(guard);
    }

    auto& selected = context->value();
    auto  prepared = prepare_payload_template(selected.run.type, selected.run.payload, secrets);
    if (!prepared && prepared.error().kind != PayloadPreparationFailureKind::Ordinary) {
        // Only Ordinary becomes an attempt outcome. Storage/PersistedData retain their sanitized identities;
        // Provider failures also abort the cycle, whose error boundary unconditionally closes acceptance.
        return DispatchResult<std::optional<DispatchStart>>::failure(std::move(prepared).error().error);
    }

    // Persist the running attempt before transitioning its run so every committed running run has a concrete owner.
    auto attempt = JobAttempt{
        .run_id         = selected.run.id,
        .attempt_number = selected.next_attempt,
        .due_at         = selected.run.runnable_at,
        .started_at     = started_at,
        .state          = AttemptState::Running,
    };
    AttemptRepository attempts{database};
    auto              inserted = attempts.insert_attempt(attempt);
    if (!inserted) {
        return DispatchResult<std::optional<DispatchStart>>::failure(std::move(inserted).error());
    }
    auto transitioned = repository.mark_dispatch_running(selected.run.id, selected.run.state, started_at);
    if (!transitioned) {
        return DispatchResult<std::optional<DispatchStart>>::failure(std::move(transitioned).error());
    }
    if (!*transitioned) {
        return rollback_ineligible(guard);
    }

    // No external executor call may occur until both durable writes commit.
    auto committed = guard.commit();
    if (!committed) {
        return DispatchResult<std::optional<DispatchStart>>::failure(std::move(committed).error());
    }

    auto const key = AttemptKey{.run_id = selected.run.id, .attempt_number = selected.next_attempt};
    if (!prepared) {
        // The core must record correlation and capacity before accepting this terminal preparation outcome.
        return DispatchResult<std::optional<DispatchStart>>::success(DispatchStart{
            .key                  = key,
            .immediate_completion = terminal_start_failure(key, prepared.error().error),
        });
    }

    auto request = AttemptStartRequest{
        .key        = key,
        .job_id     = selected.run.job_id,
        .queue_id   = selected.run.queue_id,
        .type       = selected.run.type,
        .attributes = std::move(selected.run.attributes),
        .payload    = std::move(prepared).value(),
        .started_at = started_at,
    };
    auto started = executor.start(std::move(request), std::move(completion));
    if (!started) {
        // The durable start cannot be rolled back now; route executor start failure through the normal completion path.
        return DispatchResult<std::optional<DispatchStart>>::success(DispatchStart{
            .key                  = key,
            .immediate_completion = terminal_start_failure(key, started.error()),
        });
    }
    return DispatchResult<std::optional<DispatchStart>>::success(DispatchStart{.key = key});
}

} // anonymous namespace

auto dispatch_selected(jb::db::Database&        database,
                       AttributeRegistry const& attributes,
                       AttemptExecutor&         executor,
                       SecretProvider&          secrets,
                       jb::core::Uuid const&    run_id,
                       jb::core::UtcTimePoint   started_at,
                       AttemptCompletionHandler completion)
    -> jb::core::Result<std::optional<DispatchStart>, jb::core::Error>
{
    auto dispatched =
        dispatch_selected_impl(database, attributes, executor, secrets, run_id, started_at, std::move(completion));

    // Inspect cleanup only after the transaction guard has unwound. Preserve a first fatal storage/provider error;
    // an otherwise ordinary result cannot hide a rollback failure that made the connection unusable.
    if (database.is_poisoned()) {
        auto const already_fatal =
            !dispatched && (classify_storage_failure(dispatched.error(), StorageOperation::Dispatch) ==
                                StorageFailureDisposition::Fatal ||
                            dispatched.error().code == "jobu.secret.provider_failed");
        if (!already_fatal) {
            auto error = database.last_error();
            if (!error ||
                classify_storage_failure(*error, StorageOperation::Dispatch) != StorageFailureDisposition::Fatal) {
                error = jb::core::Error{.category = jb::core::ErrorCategory::Internal,
                                        .code     = "db.connection_failed",
                                        .message  = "The database connection is unusable after transaction cleanup"};
            }
            return DispatchResult<std::optional<DispatchStart>>::failure(
                sanitized_storage_error(*error, StorageOperation::Dispatch));
        }
    }
    return dispatched;
}

} // namespace jb::jobu::detail
