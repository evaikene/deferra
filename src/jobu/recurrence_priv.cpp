#include "recurrence_priv.hpp"

#include "cron.hpp"
#include "job_repository_priv.hpp"
#include "run_repository_priv.hpp"

#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace jb::jobu::detail {
namespace {

template <typename T>
using RecurrenceResult = jb::core::Result<T, jb::core::Error>;

auto recurrence_invariant(std::string reason, jb::core::Error const* cause = nullptr) -> jb::core::Error
{
    auto error   = jb::core::Error{.category = jb::core::ErrorCategory::Internal,
                                   .code     = "jobu.storage.invariant",
                                   .message  = "Persisted recurring scheduler state is inconsistent"};
    error.detail = "reason=" + std::move(reason);
    if (cause != nullptr) {
        error.detail += ";cause=" + cause->code;
    }
    return error;
}

} // namespace

auto insert_recurring_run(jb::db::Database&        database,
                          AttributeRegistry const& attributes,
                          CronEngine const&        cron,
                          jb::core::UuidGenerator& uuid_generator,
                          JobDefinition            definition,
                          jb::core::UtcTimePoint   lower_bound) -> RecurrenceResult<bool>
{
    // Schedule ownership survives suspension. Only deletion or a current Once schedule
    // suppresses a successor; the caller has already retired or ruled out existing work.
    if (definition.state == JobState::Deleted || std::holds_alternative<OnceSchedule>(definition.schedule)) {
        return RecurrenceResult<bool>::success(false);
    }

    auto const* schedule = std::get_if<CronSchedule>(&definition.schedule);
    if (schedule == nullptr) {
        return RecurrenceResult<bool>::failure(recurrence_invariant("invalid_live_schedule"));
    }
    auto valid = cron.validate(*schedule);
    if (!valid) {
        return RecurrenceResult<bool>::failure(std::move(valid).error());
    }
    auto next = cron.next_after(*schedule, lower_bound);
    if (!next) {
        return RecurrenceResult<bool>::failure(std::move(next).error());
    }
    if (*next <= lower_bound) {
        return RecurrenceResult<bool>::failure(recurrence_invariant("non_future_successor"));
    }
    auto run_id = uuid_generator.generate();
    if (!run_id) {
        return RecurrenceResult<bool>::failure(std::move(run_id).error());
    }

    RunRepository runs{database, attributes};
    auto          inserted = runs.insert_schedule_owned(JobRun{
        .id             = *run_id,
        .job_id         = definition.id,
        .job_revision   = definition.revision,
        .queue_id       = definition.queue_id,
        .origin         = RunOrigin::Scheduled,
        .schedule_owned = true,
        .planned_at     = *next,
        .runnable_at    = *next,
        .started_at     = std::nullopt,
        .completed_at   = std::nullopt,
        .type           = definition.type,
        .priority       = definition.priority,
        .attributes     = std::move(definition.attributes),
        .payload        = std::move(definition.payload),
        .state          = RunState::Scheduled,
        .result         = std::nullopt,
    });
    if (!inserted) {
        return RecurrenceResult<bool>::failure(std::move(inserted).error());
    }
    return RecurrenceResult<bool>::success(true);
}

auto insert_recurring_successor(jb::db::Database&        database,
                                AttributeRegistry const& attributes,
                                CronEngine const&        cron,
                                jb::core::UuidGenerator& uuid_generator,
                                JobRun const&            completed_run,
                                jb::core::UtcTimePoint   lower_bound) -> RecurrenceResult<bool>
{
    if (!completed_run.schedule_owned) {
        return RecurrenceResult<bool>::success(false);
    }
    if (completed_run.origin != RunOrigin::Scheduled) {
        return RecurrenceResult<bool>::failure(recurrence_invariant("schedule_owned_origin"));
    }

    JobRepository jobs{database, attributes};
    auto          found = jobs.find_by_id(completed_run.job_id, true);
    if (!found) {
        if (found.error().code.starts_with("db.")) {
            return RecurrenceResult<bool>::failure(std::move(found).error());
        }
        return RecurrenceResult<bool>::failure(recurrence_invariant("invalid_live_definition", &found.error()));
    }
    if (!found->has_value()) {
        return RecurrenceResult<bool>::failure(recurrence_invariant("missing_live_definition"));
    }
    return insert_recurring_run(database, attributes, cron, uuid_generator, std::move(**found), lower_bound);
}

} // namespace jb::jobu::detail
