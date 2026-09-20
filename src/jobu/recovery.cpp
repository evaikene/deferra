#include "recovery.hpp"

#include "recovery_priv.hpp"
#include "recovery_repository_priv.hpp"
#include "run_repository_priv.hpp"
#include "scheduler_repository_priv.hpp"
#include "storage_failure_priv.hpp"
#include "time_source.hpp"
#include "transaction.hpp"

#include <algorithm>
#include <array>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace jb::jobu {
namespace {

using namespace detail;
template <typename T = void>
using RecoveryResult = jb::core::Result<T, jb::core::Error>;

auto invariant(std::string_view reason) -> jb::core::Error
{
    return {.category = jb::core::ErrorCategory::Internal,
            .code     = "jobu.recovery.invariant",
            .message  = "Startup recovery could not establish valid durable state",
            .detail   = "reason=" + std::string{reason}};
}

// One stack-owned invocation: pages never retain queries across repair transactions, and
// only commit_unit() publishes report changes. No runtime or external-execution state lives here.
class Recovery final {
public:
    Recovery(jb::db::Database&            database,
             AttributeRegistry const&     attributes,
             CronEngine const&            cron,
             jb::core::UuidGenerator&     generator,
             jb::core::TimeSource&        time,
             RecoveryOptions              options,
             std::function<bool()> const& should_stop)
        : _database{database}
        , _repository{database, attributes}
        , _runs{database, attributes}
        , _scheduler{database, attributes}
        , _cron{cron}
        , _generator{generator}
        , _time{time}
        , _batch_size{options.scan_batch_size}
        , _should_stop{should_stop}
        , _recovery_time{time.utc_now()}
    {}

    auto run() -> RecoveryResult<RecoveryReport>
    {
        // Validate all persisted families before the first write, including owners with no runs.
        auto validated = validate(false);
        if (!validated) {
            return RecoveryResult<RecoveryReport>::failure(std::move(validated).error());
        }
        auto interrupted = visit_pages<jb::core::Uuid>(
            [this](auto after) { return _repository.list_runs(_batch_size, after, RunState::Running); },
            [](auto const& row) { return row.id; },
            [this](JobRun const& row) { return recover_run(row); });
        if (!interrupted) {
            return RecoveryResult<RecoveryReport>::failure(std::move(interrupted).error());
        }

        // Missing work is independent of interruption. Repair recurrence before draining job
        // suspension so successor snapshots keep the same revision ordering as normal completion.
        auto jobs =
            visit_pages<jb::core::Uuid>([this](auto after) { return _repository.list_jobs(_batch_size, after); },
                                        [](auto const& row) { return row.id; },
                                        [this](JobDefinition const& job) { return recover_job(job); });
        if (!jobs) {
            return RecoveryResult<RecoveryReport>::failure(std::move(jobs).error());
        }
        auto queues = visit_pages<jb::core::Uuid>(
            [this](auto after) { return _repository.list_queues(_batch_size, after); },
            [](auto const& row) { return row.id; },
            [this](Queue const& queue) {
                if (queue.state != QueueState::Suspending) {
                    return RecoveryResult<>::success();
                }
                return commit_unit([&]() -> RecoveryResult<RecoveryReport> {
                    auto drained = _repository.complete_drained_queue_suspension(queue.id, _recovery_time);
                    if (!drained) {
                        return RecoveryResult<RecoveryReport>::failure(std::move(drained).error());
                    }
                    return RecoveryResult<RecoveryReport>::success({.suspended_queues = *drained ? 1U : 0U});
                });
            });
        if (!queues) {
            return RecoveryResult<RecoveryReport>::failure(std::move(queues).error());
        }

        // Scans accept missing work and suspensions as repair inputs. Final validation must
        // additionally reject those shapes, not merely repeat the initial structural checks.
        auto final = validate(true);
        if (!final) {
            return RecoveryResult<RecoveryReport>::failure(std::move(final).error());
        }
        return RecoveryResult<RecoveryReport>::success(_report);
    }

private:
    auto check_stop() const -> RecoveryResult<>
    {
        if (_should_stop && _should_stop()) {
            return RecoveryResult<>::failure({.category = jb::core::ErrorCategory::Cancelled,
                                              .code     = "jobu.recovery.cancelled",
                                              .message  = "Startup recovery was cancelled"});
        }
        return RecoveryResult<>::success();
    }

    template <typename Key, typename ReadPage, typename KeyOf, typename Visit>
    auto visit_pages(ReadPage read, KeyOf key_of, Visit visit) -> RecoveryResult<>
    {
        auto after = std::optional<Key>{};
        for (;;) {
            auto stop = check_stop();
            if (!stop) {
                return stop;
            }
            auto page = read(after);
            if (!page) {
                return RecoveryResult<>::failure(std::move(page).error());
            }
            if (page->empty()) {
                return check_stop();
            }
            // Save the stable cursor before mutations change the scan predicate. All rows
            // own their values, and the repository has already released its read queries.
            after = key_of(page->back());
            for (auto const& row : *page) {
                auto result = visit(row);
                if (!result) {
                    return result;
                }
            }
        }
    }

    auto validate(bool final) -> RecoveryResult<>
    {
        auto runs = visit_pages<jb::core::Uuid>(
            [this](auto after) { return _repository.list_runs(_batch_size, after); },
            [](auto const& row) { return row.id; },
            [final](JobRun const& row) {
                return final && row.state == RunState::Running
                         ? RecoveryResult<>::failure(invariant("running_run_after_recovery"))
                         : RecoveryResult<>::success();
            });
        if (!runs) {
            return runs;
        }
        auto attempts = visit_pages<RecoveryAttemptKey>(
            [this](auto after) { return _repository.list_attempts(_batch_size, after); },
            [](auto const& row) {
                return RecoveryAttemptKey{.run_id = row.run_id, .attempt_number = row.attempt_number};
            },
            [final](JobAttempt const& row) {
                return final && row.state == AttemptState::Running
                         ? RecoveryResult<>::failure(invariant("running_attempt_after_recovery"))
                         : RecoveryResult<>::success();
            });
        if (!attempts) {
            return attempts;
        }
        auto outputs =
            visit_pages<RecoveryAttemptKey>([this](auto after) { return _repository.list_outputs(_batch_size, after); },
                                            [](auto const& key) { return key; },
                                            [](auto const&) { return RecoveryResult<>::success(); });
        if (!outputs) {
            return outputs;
        }
        auto jobs = visit_pages<jb::core::Uuid>(
            [this](auto after) { return _repository.list_jobs(_batch_size, after); },
            [](auto const& row) { return row.id; },
            [this, final](JobDefinition const& job) -> RecoveryResult<> {
                if (!final) {
                    return RecoveryResult<>::success();
                }
                // No Running work remains at this point, so every Suspending owner is drained.
                if (job.state == JobState::Suspending) {
                    return RecoveryResult<>::failure(invariant("job_still_suspending"));
                }
                if (job.state != JobState::Deleted && std::holds_alternative<CronSchedule>(job.schedule)) {
                    auto work = _runs.find_schedule_owned(job.id);
                    if (!work) {
                        return RecoveryResult<>::failure(std::move(work).error());
                    }
                    if (!*work) {
                        return RecoveryResult<>::failure(invariant("missing_recurring_work"));
                    }
                }
                return RecoveryResult<>::success();
            });
        if (!jobs) {
            return jobs;
        }
        return visit_pages<jb::core::Uuid>([this](auto after) { return _repository.list_queues(_batch_size, after); },
                                           [](auto const& row) { return row.id; },
                                           [final](Queue const& queue) {
                                               return final && queue.state == QueueState::Suspending
                                                        ? RecoveryResult<>::failure(invariant("queue_still_suspending"))
                                                        : RecoveryResult<>::success();
                                           });
    }

    template <typename Repair>
    auto commit_unit(Repair repair) -> RecoveryResult<>
    {
        auto stop = check_stop();
        if (!stop) {
            return stop;
        }
        auto transaction = jb::db::Transaction::begin(_database);
        if (!transaction) {
            return RecoveryResult<>::failure(std::move(transaction).error());
        }
        auto delta = repair();
        auto next  = _report;
        auto ready = delta ? accumulate(next, *delta) : RecoveryResult<>::failure(std::move(delta).error());
        if (ready) {
            ready = check_stop();
        }
        if (ready) {
            ready = transaction->commit();
        }
        if (!ready) {
            // Unwind before returning a result. Preserve the first failure except when a
            // failed rollback would otherwise disguise a poisoned connection as cancellation.
            auto error = std::move(ready).error();
            if (transaction->is_active()) {
                auto rolled_back = transaction->rollback();
                if (!rolled_back && error.code == "jobu.recovery.cancelled") {
                    error = std::move(rolled_back).error();
                }
            }
            return RecoveryResult<>::failure(std::move(error));
        }
        _report = next;
        return RecoveryResult<>::success();
    }

    static auto accumulate(RecoveryReport& report, RecoveryReport const& delta) -> RecoveryResult<>
    {
        constexpr auto counters = std::array{&RecoveryReport::interrupted_attempts,
                                             &RecoveryReport::retrying_runs,
                                             &RecoveryReport::terminal_runs,
                                             &RecoveryReport::inserted_successors,
                                             &RecoveryReport::suspended_jobs,
                                             &RecoveryReport::suspended_queues};
        for (auto member : counters) {
            if (delta.*member > std::numeric_limits<std::uint64_t>::max() - report.*member) {
                return RecoveryResult<>::failure(invariant("report_counter_overflow"));
            }
            report.*member += delta.*member;
        }
        return RecoveryResult<>::success();
    }

    auto recover_run(JobRun const& run) -> RecoveryResult<>
    {
        auto const lower_bound = std::max(_recovery_time, _time.utc_now());
        return commit_unit([&]() -> RecoveryResult<RecoveryReport> {
            auto active = _scheduler.find_active_attempt(run.id);
            if (!active) {
                return RecoveryResult<RecoveryReport>::failure(std::move(active).error());
            }
            if (!*active) {
                return RecoveryResult<RecoveryReport>::failure(invariant("missing_running_attempt"));
            }
            auto key      = RecoveryAttemptKey{.run_id = run.id, .attempt_number = **active};
            auto decision = _repository.find_retry_decision(key, _recovery_time);
            if (!decision) {
                return RecoveryResult<RecoveryReport>::failure(std::move(decision).error());
            }
            auto interrupted = _repository.interrupt_attempt(key, _recovery_time);
            if (!interrupted) {
                return RecoveryResult<RecoveryReport>::failure(std::move(interrupted).error());
            }
            auto transitioned = decision->retry
                                  ? _repository.set_run_retry_wait(key, _recovery_time, decision->retry->due_at)
                                  : _repository.set_run_interrupted(key, _recovery_time);
            if (!transitioned) {
                return RecoveryResult<RecoveryReport>::failure(std::move(transitioned).error());
            }

            auto delta = RecoveryReport{.interrupted_attempts = 1,
                                        .retrying_runs        = decision->retry ? 1U : 0U,
                                        .terminal_runs        = decision->retry ? 0U : 1U};
            if (!decision->retry) {
                auto successor = _repository.insert_interrupted_successor(run.id, lower_bound, _cron, _generator);
                if (!successor) {
                    return RecoveryResult<RecoveryReport>::failure(std::move(successor).error());
                }
                delta.inserted_successors = *successor ? 1U : 0U;
            }
            auto queue = _repository.complete_drained_queue_suspension(run.queue_id, _recovery_time);
            if (!queue) {
                return RecoveryResult<RecoveryReport>::failure(std::move(queue).error());
            }
            auto job = _repository.complete_drained_job_suspension(run.job_id, _recovery_time);
            if (!job) {
                return RecoveryResult<RecoveryReport>::failure(std::move(job).error());
            }
            delta.suspended_queues = *queue ? 1U : 0U;
            delta.suspended_jobs   = *job ? 1U : 0U;
            return RecoveryResult<RecoveryReport>::success(delta);
        });
    }

    auto recover_job(JobDefinition const& job) -> RecoveryResult<>
    {
        if (job.state == JobState::Deleted ||
            (std::holds_alternative<OnceSchedule>(job.schedule) && job.state != JobState::Suspending)) {
            return RecoveryResult<>::success();
        }
        auto const lower_bound = std::max(_recovery_time, _time.utc_now());
        return commit_unit([&]() -> RecoveryResult<RecoveryReport> {
            auto successor = _repository.repair_missing_successor(job.id, lower_bound, _cron, _generator);
            if (!successor) {
                return RecoveryResult<RecoveryReport>::failure(std::move(successor).error());
            }
            auto drained = _repository.complete_drained_job_suspension(job.id, _recovery_time);
            if (!drained) {
                return RecoveryResult<RecoveryReport>::failure(std::move(drained).error());
            }
            return RecoveryResult<RecoveryReport>::success(
                {.inserted_successors = *successor ? 1U : 0U, .suspended_jobs = *drained ? 1U : 0U});
        });
    }

    jb::db::Database&            _database;
    RecoveryRepository           _repository;
    RunRepository                _runs;
    SchedulerRepository          _scheduler;
    CronEngine const&            _cron;
    jb::core::UuidGenerator&     _generator;
    jb::core::TimeSource&        _time;
    std::size_t                  _batch_size;
    std::function<bool()> const& _should_stop;
    jb::core::UtcTimePoint       _recovery_time;
    RecoveryReport               _report;
};

} // namespace

auto recover_startup(jb::db::Database&        database,
                     AttributeRegistry const& attributes,
                     CronEngine const&        cron,
                     jb::core::UuidGenerator& uuid_generator,
                     jb::core::TimeSource&    time_source,
                     RecoveryOptions          options) -> RecoveryResult<RecoveryReport>
{
    return detail::recover_startup(database, attributes, cron, uuid_generator, time_source, options, {});
}

auto detail::recover_startup(jb::db::Database&            database,
                             AttributeRegistry const&     attributes,
                             CronEngine const&            cron,
                             jb::core::UuidGenerator&     uuid_generator,
                             jb::core::TimeSource&        time_source,
                             RecoveryOptions              options,
                             std::function<bool()> const& should_stop) -> RecoveryResult<RecoveryReport>
{
    if (options.scan_batch_size == 0 || options.scan_batch_size > 4096) {
        return RecoveryResult<RecoveryReport>::failure({.category = jb::core::ErrorCategory::InvalidArgument,
                                                        .code     = "jobu.recovery.invalid_options",
                                                        .message  = "Recovery scan batch size must be in 1..4096"});
    }
    auto recovery = Recovery{database, attributes, cron, uuid_generator, time_source, options, should_stop};
    auto result   = recovery.run();
    if (!result) {
        return RecoveryResult<RecoveryReport>::failure(
            sanitized_storage_error(result.error(), StorageOperation::Recovery));
    }
    return result;
}

} // namespace jb::jobu
