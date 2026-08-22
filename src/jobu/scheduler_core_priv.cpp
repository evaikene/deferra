#include "scheduler_core_priv.hpp"

#include "attempt_executor.hpp"
#include "json.hpp"
#include "retry_policy_priv.hpp"
#include "scheduler_dispatch_priv.hpp"
#include "scheduler_repository_priv.hpp"
#include "time_source.hpp"
#include "transaction.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace jb::jobu::detail {

namespace {

template <typename T>
using CoreResult = jb::core::Result<T, jb::core::Error>;

constexpr std::size_t kMaximumRepositoryPageRows    = 1000U;
constexpr std::size_t kMaximumCompletionResultBytes = std::size_t{256} * 1024U;

struct CapacityState {
    std::uint64_t                                                 cli_running{0};
    std::uint64_t                                                 http_running{0};
    std::map<jb::core::Uuid, std::uint64_t>                       queue_slots;
    std::map<jb::core::Uuid, std::vector<BlockingRetryCandidate>> blocking_retries;
    std::set<jb::core::Uuid>                                      blocking_retry_ids;
};

struct CandidateBatch {
    std::vector<DispatchCandidate> candidates;
    std::size_t                    next{0};
    bool                           queried{false};
    bool                           may_have_more{false};
};

struct EligibleQueue {
    QueueRuntime const* runtime{nullptr};
    CandidateBatch*     batch{nullptr};
    bool                blocking_only{false};
};

using CandidateCache = std::map<jb::core::Uuid, CandidateBatch>;

struct CompletionEffect {
    bool queue_slot_retained{false};
};

using CompletionProcessor =
    std::function<CoreResult<CompletionEffect>(jb::core::Uuid const&, AttemptCompletion const&)>;

auto core_error(jb::core::ErrorCategory category, std::string code, std::string message) -> jb::core::Error
{
    return {
        .category = category,
        .code     = std::move(code),
        .message  = std::move(message),
    };
}

auto invalid_options() -> jb::core::Error
{
    return core_error(jb::core::ErrorCategory::InvalidArgument,
                      "jobu.scheduler.invalid_options",
                      "Scheduler concurrency limits and candidate batch size must be positive");
}

auto capacity_overflow() -> jb::core::Error
{
    return core_error(jb::core::ErrorCategory::Internal,
                      "jobu.storage.invariant",
                      "Persisted scheduler capacity exceeds its supported range");
}

auto fairness_overflow() -> jb::core::Error
{
    return core_error(jb::core::ErrorCategory::Internal,
                      "jobu.storage.invariant",
                      "Scheduler fairness arithmetic exceeds its supported range");
}

auto invalid_completion(std::string reason) -> jb::core::Error
{
    auto error   = core_error(jb::core::ErrorCategory::Internal,
                              "jobu.executor.invalid_completion",
                              "Attempt executor completion violates its contract");
    error.detail = "reason=" + std::move(reason);
    return error;
}

auto checked_add(std::uint64_t& value, std::uint32_t increment) -> CoreResult<void>
{
    if (value > std::numeric_limits<std::uint64_t>::max() - increment) {
        return CoreResult<void>::failure(capacity_overflow());
    }
    value += increment;
    return CoreResult<void>::success();
}

auto page_size(SchedulerCoreOptions const& options) noexcept -> std::size_t
{
    return std::min(options.candidate_batch_size, kMaximumRepositoryPageRows);
}

auto load_runtime_queues(SchedulerRepository& repository, std::size_t limit) -> CoreResult<std::vector<QueueRuntime>>
{
    auto queues   = std::vector<QueueRuntime>{};
    auto after_id = std::optional<jb::core::Uuid>{};
    while (true) {
        auto page = repository.list_runtime_queues(limit, after_id);
        if (!page) {
            return CoreResult<std::vector<QueueRuntime>>::failure(std::move(page).error());
        }
        if (page->empty()) {
            break;
        }
        after_id = page->back().id;
        queues.insert(queues.end(), std::make_move_iterator(page->begin()), std::make_move_iterator(page->end()));
        if (page->size() < limit) {
            break;
        }
    }
    return CoreResult<std::vector<QueueRuntime>>::success(std::move(queues));
}

auto load_capacity(SchedulerRepository& repository, std::size_t limit) -> CoreResult<CapacityState>
{
    auto state    = CapacityState{};
    auto after_id = std::optional<jb::core::Uuid>{};
    while (true) {
        auto page = repository.list_capacity_rows(limit, after_id);
        if (!page) {
            return CoreResult<CapacityState>::failure(std::move(page).error());
        }
        if (page->empty()) {
            break;
        }
        for (auto const& row : *page) {
            auto cli = checked_add(state.cli_running, row.usage.cli_running);
            if (!cli) {
                return CoreResult<CapacityState>::failure(std::move(cli).error());
            }
            auto http = checked_add(state.http_running, row.usage.http_running);
            if (!http) {
                return CoreResult<CapacityState>::failure(std::move(http).error());
            }
            auto& queue_slots = state.queue_slots[row.queue_id];
            auto  queue       = checked_add(queue_slots, row.usage.queue_slots);
            if (!queue) {
                return CoreResult<CapacityState>::failure(std::move(queue).error());
            }
            if (row.blocking_retry) {
                state.blocking_retries[row.queue_id].push_back(*row.blocking_retry);
                state.blocking_retry_ids.insert(row.run_id);
            }
        }
        after_id = page->back().run_id;
        if (page->size() < limit) {
            break;
        }
    }
    auto const ordered_before = [](BlockingRetryCandidate const& left, BlockingRetryCandidate const& right) {
        if (left.priority != right.priority) {
            return left.priority > right.priority;
        }
        if (left.runnable_at != right.runnable_at) {
            return left.runnable_at < right.runnable_at;
        }
        if (left.planned_at != right.planned_at) {
            return left.planned_at < right.planned_at;
        }
        return left.run_id < right.run_id;
    };
    for (auto& [queue_id, retries] : state.blocking_retries) {
        (void)queue_id;
        std::ranges::sort(retries, ordered_before);
    }
    return CoreResult<CapacityState>::success(std::move(state));
}

auto running_for(CapacityState const& state, JobType type) noexcept -> std::uint64_t
{
    return type == JobType::Cli ? state.cli_running : state.http_running;
}

auto global_limit_for(SchedulerCoreOptions const& options, JobType type) noexcept -> std::uint32_t
{
    return type == JobType::Cli ? options.cli_concurrency : options.http_concurrency;
}

auto queue_slots_for(CapacityState const& state, jb::core::Uuid const& queue_id) noexcept -> std::uint64_t
{
    auto const found = state.queue_slots.find(queue_id);
    return found == state.queue_slots.end() ? 0 : found->second;
}

auto record_start(CapacityState& state, JobType type, jb::core::Uuid const& queue_id, bool queue_slot_already_occupied)
    -> CoreResult<void>
{
    auto* running = &state.http_running;
    if (type == JobType::Cli) {
        running = &state.cli_running;
    }
    auto& queue_slots = state.queue_slots[queue_id];
    if (*running == std::numeric_limits<std::uint64_t>::max() ||
        (!queue_slot_already_occupied && queue_slots == std::numeric_limits<std::uint64_t>::max())) {
        return CoreResult<void>::failure(capacity_overflow());
    }
    ++*running;
    if (!queue_slot_already_occupied) {
        ++queue_slots;
    }
    return CoreResult<void>::success();
}

auto record_completion(CapacityState& state, JobType type, jb::core::Uuid const& queue_id, CompletionEffect effect)
    -> CoreResult<void>
{
    auto* running = &state.http_running;
    if (type == JobType::Cli) {
        running = &state.cli_running;
    }
    auto& queue_slots = state.queue_slots[queue_id];
    if (*running == 0 || (!effect.queue_slot_retained && queue_slots == 0)) {
        return CoreResult<void>::failure(capacity_overflow());
    }
    --*running;
    if (!effect.queue_slot_retained) {
        --queue_slots;
    }
    return CoreResult<void>::success();
}

auto next_blocking_retry(CapacityState const&            capacity,
                         jb::core::Uuid const&           queue_id,
                         JobType                         type,
                         jb::core::UtcTimePoint          now,
                         std::set<jb::core::Uuid> const& attempted) -> BlockingRetryCandidate const*
{
    auto const found = capacity.blocking_retries.find(queue_id);
    if (found == capacity.blocking_retries.end()) {
        return nullptr;
    }
    auto const candidate = std::ranges::find_if(found->second, [&](BlockingRetryCandidate const& retry) {
        return retry.type == type && retry.runnable_at <= now && !attempted.contains(retry.run_id);
    });
    return candidate == found->second.end() ? nullptr : &*candidate;
}

auto validate_completion(AttemptCompletion const& completion) -> CoreResult<std::string>
{
    switch (completion.outcome) {
        case AttemptOutcome::Succeeded:
        case AttemptOutcome::Cancelled:
            if (completion.failure_disposition || completion.retry_not_before) {
                return CoreResult<std::string>::failure(invalid_completion("terminal_fields"));
            }
            break;
        case AttemptOutcome::Failed:
            if (!completion.failure_disposition) {
                return CoreResult<std::string>::failure(invalid_completion("missing_failure_disposition"));
            }
            if (*completion.failure_disposition != FailureDisposition::Terminal &&
                *completion.failure_disposition != FailureDisposition::Retryable) {
                return CoreResult<std::string>::failure(invalid_completion("unknown_failure_disposition"));
            }
            if (completion.retry_not_before && *completion.failure_disposition != FailureDisposition::Retryable) {
                return CoreResult<std::string>::failure(invalid_completion("terminal_retry_deadline"));
            }
            break;
        case AttemptOutcome::Interrupted:
            return CoreResult<std::string>::failure(invalid_completion("interrupted_reserved"));
        default:
            return CoreResult<std::string>::failure(invalid_completion("unknown_outcome"));
    }

    if (!completion.result.is_object()) {
        return CoreResult<std::string>::failure(invalid_completion("result_not_object"));
    }
    auto serialized = jb::rpc::serialize_json(completion.result);
    if (!serialized) {
        return CoreResult<std::string>::failure(invalid_completion("result_not_serializable"));
    }
    if (serialized->size() > kMaximumCompletionResultBytes) {
        return CoreResult<std::string>::failure(invalid_completion("result_too_large"));
    }
    return CoreResult<std::string>::success(std::move(serialized).value());
}

auto terminal_run_state(AttemptOutcome outcome) -> CoreResult<RunState>
{
    switch (outcome) {
        case AttemptOutcome::Succeeded:
            return CoreResult<RunState>::success(RunState::Succeeded);
        case AttemptOutcome::Failed:
            return CoreResult<RunState>::success(RunState::Failed);
        case AttemptOutcome::Cancelled:
            return CoreResult<RunState>::success(RunState::Cancelled);
        case AttemptOutcome::Interrupted:
            break;
    }
    return CoreResult<RunState>::failure(invalid_completion("invalid_terminal_outcome"));
}

auto process_completion(jb::db::Database&                        database,
                        AttributeRegistry const&                 attributes,
                        jb::core::TimeSource&                    time_source,
                        std::map<jb::core::Uuid, std::uint64_t>& active_attempts,
                        jb::core::Uuid const&                    expected_run_id,
                        AttemptCompletion const&                 completion) -> CoreResult<CompletionEffect>
{
    auto const active = active_attempts.find(expected_run_id);
    if (active == active_attempts.end()) {
        return CoreResult<CompletionEffect>::failure(invalid_completion("unexpected_callback"));
    }
    if (completion.key.run_id != expected_run_id || completion.key.attempt_number != active->second) {
        return CoreResult<CompletionEffect>::failure(invalid_completion("key_mismatch"));
    }
    auto serialized = validate_completion(completion);
    if (!serialized) {
        return CoreResult<CompletionEffect>::failure(std::move(serialized).error());
    }

    auto const completed_at = time_source.utc_now();
    auto       transaction  = jb::db::Transaction::begin(database);
    if (!transaction) {
        return CoreResult<CompletionEffect>::failure(std::move(transaction).error());
    }
    auto guard = std::move(transaction).value();

    SchedulerRepository repository{database, attributes};
    auto context = repository.find_completion_context(completion.key.run_id, completion.key.attempt_number);
    if (!context) {
        return CoreResult<CompletionEffect>::failure(std::move(context).error());
    }
    auto decision = retry_decision(
        context->run.attributes,
        {
            .run_id           = completion.key.run_id,
            .attempt_number   = completion.key.attempt_number,
            .outcome          = completion.outcome,
            .retryable        = completion.failure_disposition == FailureDisposition::Retryable,
            .retry_allowed    = context->job_state != JobState::Deleted && context->queue_state != QueueState::Deleted,
            .completed_at     = completed_at,
            .retry_not_before = completion.retry_not_before,
        });
    if (!decision) {
        return CoreResult<CompletionEffect>::failure(std::move(decision).error());
    }
    auto attempt_completed = repository.complete_attempt(completion.key.run_id,
                                                         completion.key.attempt_number,
                                                         completed_at,
                                                         completion.outcome,
                                                         *serialized);
    if (!attempt_completed) {
        return CoreResult<CompletionEffect>::failure(std::move(attempt_completed).error());
    }

    auto effect = CompletionEffect{};
    if (decision->retry) {
        auto retry_wait = repository.set_run_retry_wait(completion.key.run_id, decision->retry->due_at);
        if (!retry_wait) {
            return CoreResult<CompletionEffect>::failure(std::move(retry_wait).error());
        }
        auto const job_permits_execution =
            context->job_state == JobState::Active || context->run.origin == RunOrigin::Manual;
        effect.queue_slot_retained = decision->retry->mode == RetryMode::Blocking && job_permits_execution &&
                                     context->queue_state == QueueState::Active;
    }
    else {
        auto terminal = terminal_run_state(completion.outcome);
        if (!terminal) {
            return CoreResult<CompletionEffect>::failure(std::move(terminal).error());
        }
        auto run_completed = repository.set_run_terminal(completion.key.run_id, *terminal, completed_at, *serialized);
        if (!run_completed) {
            return CoreResult<CompletionEffect>::failure(std::move(run_completed).error());
        }
    }

    auto committed = guard.commit();
    if (!committed) {
        return CoreResult<CompletionEffect>::failure(std::move(committed).error());
    }
    active_attempts.erase(active);
    return CoreResult<CompletionEffect>::success(effect);
}

void reset_credits(std::map<jb::core::Uuid, std::int64_t>& credits, std::vector<QueueRuntime> const& queues)
{
    for (auto const& queue : queues) {
        auto const found = credits.find(queue.id);
        if (found != credits.end()) {
            found->second = 0;
        }
    }
}

void reconcile_fairness(std::vector<QueueRuntime> const&         queues,
                        std::map<jb::core::Uuid, std::uint32_t>& queue_weights,
                        std::map<jb::core::Uuid, std::int64_t>&  cli_credits,
                        std::map<jb::core::Uuid, std::int64_t>&  http_credits)
{
    auto current_weights = std::map<jb::core::Uuid, std::uint32_t>{};
    for (auto const& queue : queues) {
        current_weights.emplace(queue.id, queue.weight);
        auto const previous = queue_weights.find(queue.id);
        // A new or reweighted queue begins a new fairness history for both independent resources.
        if (previous == queue_weights.end() || previous->second != queue.weight) {
            cli_credits[queue.id]  = 0;
            http_credits[queue.id] = 0;
        }
    }

    for (auto iterator = cli_credits.begin(); iterator != cli_credits.end();) {
        if (!current_weights.contains(iterator->first)) {
            iterator = cli_credits.erase(iterator);
        }
        else {
            ++iterator;
        }
    }
    for (auto iterator = http_credits.begin(); iterator != http_credits.end();) {
        if (!current_weights.contains(iterator->first)) {
            iterator = http_credits.erase(iterator);
        }
        else {
            ++iterator;
        }
    }
    queue_weights = std::move(current_weights);
}

auto add_weight(std::int64_t& value, std::uint32_t weight) -> CoreResult<void>
{
    if (value > std::numeric_limits<std::int64_t>::max() - static_cast<std::int64_t>(weight)) {
        return CoreResult<void>::failure(fairness_overflow());
    }
    value += static_cast<std::int64_t>(weight);
    return CoreResult<void>::success();
}

auto subtract_total(std::int64_t& value, std::int64_t total) -> CoreResult<void>
{
    if (value < std::numeric_limits<std::int64_t>::min() + total) {
        return CoreResult<void>::failure(fairness_overflow());
    }
    value -= total;
    return CoreResult<void>::success();
}

auto load_candidate_batch(SchedulerRepository&   repository,
                          jb::core::Uuid const&  queue_id,
                          JobType                type,
                          jb::core::UtcTimePoint now,
                          std::size_t            limit,
                          CandidateBatch&        batch) -> CoreResult<void>
{
    if (batch.next < batch.candidates.size() || (batch.queried && !batch.may_have_more)) {
        return CoreResult<void>::success();
    }

    auto candidates = repository.list_runnable(queue_id, type, now, limit);
    if (!candidates) {
        return CoreResult<void>::failure(std::move(candidates).error());
    }
    batch.candidates    = std::move(candidates).value();
    batch.next          = 0;
    batch.queried       = true;
    batch.may_have_more = batch.candidates.size() == limit;
    return CoreResult<void>::success();
}

auto dispatch_visit(jb::db::Database&                        database,
                    AttributeRegistry const&                 attributes,
                    AttemptExecutor&                         executor,
                    SchedulerRepository&                     repository,
                    std::vector<QueueRuntime> const&         queues,
                    JobType                                  type,
                    jb::core::UtcTimePoint                   now,
                    SchedulerCoreOptions const&              options,
                    CapacityState&                           capacity,
                    std::map<jb::core::Uuid, std::int64_t>&  credits,
                    CandidateCache&                          candidate_cache,
                    std::map<jb::core::Uuid, std::uint64_t>& active_attempts,
                    std::set<jb::core::Uuid>&                attempted_blocking_retries,
                    CompletionProcessor const&               completion_processor) -> CoreResult<bool>
{
    if (running_for(capacity, type) >= global_limit_for(options, type)) {
        return CoreResult<bool>::success(false);
    }
    if (!executor.is_available(type)) {
        reset_credits(credits, queues);
        return CoreResult<bool>::success(false);
    }

    auto const limit    = page_size(options);
    auto       eligible = std::vector<EligibleQueue>{};
    eligible.reserve(queues.size());
    for (auto const& queue : queues) {
        if (queue_slots_for(capacity, queue.id) >= queue.concurrency_limit) {
            if (next_blocking_retry(capacity, queue.id, type, now, attempted_blocking_retries) == nullptr) {
                credits[queue.id] = 0;
                continue;
            }
            eligible.push_back({.runtime = &queue, .blocking_only = true});
            continue;
        }
        auto& batch  = candidate_cache[queue.id];
        auto  loaded = load_candidate_batch(repository, queue.id, type, now, limit, batch);
        if (!loaded) {
            return CoreResult<bool>::failure(std::move(loaded).error());
        }
        if (batch.next == batch.candidates.size()) {
            credits[queue.id] = 0;
            continue;
        }
        eligible.push_back({.runtime = &queue, .batch = &batch, .blocking_only = false});
    }
    if (eligible.empty()) {
        return CoreResult<bool>::success(false);
    }

    // Smooth weighted round-robin adds every eligible weight before selecting the highest current credit.
    auto total = std::int64_t{0};
    for (auto const& queue : eligible) {
        auto total_added = add_weight(total, queue.runtime->weight);
        if (!total_added) {
            return CoreResult<bool>::failure(std::move(total_added).error());
        }
        auto const credit = credits[queue.runtime->id];
        if (credit > std::numeric_limits<std::int64_t>::max() - static_cast<std::int64_t>(queue.runtime->weight)) {
            return CoreResult<bool>::failure(fairness_overflow());
        }
    }
    for (auto const& queue : eligible) {
        credits[queue.runtime->id] += static_cast<std::int64_t>(queue.runtime->weight);
    }

    while (!eligible.empty()) {
        auto selected_index = std::size_t{0};
        for (auto index = std::size_t{1}; index < eligible.size(); ++index) {
            auto const queue_credit    = credits[eligible[index].runtime->id];
            auto const selected_credit = credits[eligible[selected_index].runtime->id];
            if (queue_credit > selected_credit ||
                (queue_credit == selected_credit &&
                 eligible[index].runtime->id < eligible[selected_index].runtime->id)) {
                selected_index = index;
            }
        }
        auto& selected   = eligible[selected_index];
        auto  subtracted = subtract_total(credits[selected.runtime->id], total);
        if (!subtracted) {
            return CoreResult<bool>::failure(std::move(subtracted).error());
        }

        auto try_dispatch = [&](jb::core::Uuid const& run_id, bool queue_slot_already_occupied) -> CoreResult<bool> {
            auto completion = [expected_run_id = run_id,
                               processor       = completion_processor](AttemptCompletion const& value) mutable {
                auto handled = processor(expected_run_id, value);
                (void)handled;
            };
            auto dispatched = dispatch_selected(database, attributes, executor, run_id, now, std::move(completion));
            if (!dispatched) {
                return CoreResult<bool>::failure(std::move(dispatched).error());
            }
            if (!dispatched->has_value()) {
                return CoreResult<bool>::success(false);
            }
            if (dispatched->value().key.run_id != run_id || dispatched->value().key.attempt_number == 0) {
                return CoreResult<bool>::failure(invalid_completion("dispatch_key_mismatch"));
            }
            auto const [active, inserted] = active_attempts.emplace(run_id, dispatched->value().key.attempt_number);
            (void)active;
            if (!inserted) {
                return CoreResult<bool>::failure(invalid_completion("duplicate_active_attempt"));
            }
            auto recorded = record_start(capacity, type, selected.runtime->id, queue_slot_already_occupied);
            if (!recorded) {
                return CoreResult<bool>::failure(std::move(recorded).error());
            }
            if (dispatched->value().immediate_completion) {
                auto completed = completion_processor(run_id, *dispatched->value().immediate_completion);
                if (!completed) {
                    return CoreResult<bool>::failure(std::move(completed).error());
                }
                auto reconciled = record_completion(capacity, type, selected.runtime->id, *completed);
                if (!reconciled) {
                    return CoreResult<bool>::failure(std::move(reconciled).error());
                }
            }
            return CoreResult<bool>::success(true);
        };

        if (selected.blocking_only) {
            while (auto const* retry =
                       next_blocking_retry(capacity, selected.runtime->id, type, now, attempted_blocking_retries)) {
                attempted_blocking_retries.insert(retry->run_id);
                auto dispatched = try_dispatch(retry->run_id, true);
                if (!dispatched) {
                    return CoreResult<bool>::failure(std::move(dispatched).error());
                }
                if (*dispatched) {
                    return CoreResult<bool>::success(true);
                }
            }
        }
        else {
            while (selected.batch->next < selected.batch->candidates.size()) {
                auto const& candidate = selected.batch->candidates[selected.batch->next];
                ++selected.batch->next;
                auto dispatched =
                    try_dispatch(candidate.run.id, capacity.blocking_retry_ids.contains(candidate.run.id));
                if (!dispatched) {
                    return CoreResult<bool>::failure(std::move(dispatched).error());
                }
                if (*dispatched) {
                    return CoreResult<bool>::success(true);
                }
            }
        }

        // The selected snapshot went stale. It no longer participates in this opportunity, but another queue may.
        credits[selected.runtime->id]  = 0;
        total                         -= static_cast<std::int64_t>(selected.runtime->weight);
        eligible.erase(eligible.begin() + static_cast<std::ptrdiff_t>(selected_index));
    }
    return CoreResult<bool>::success(false);
}

} // anonymous namespace

SchedulerCore::SchedulerCore(jb::db::Database&        database,
                             AttributeRegistry const& attributes,
                             jb::core::TimeSource&    time_source,
                             AttemptExecutor&         executor,
                             SchedulerCoreOptions     options) noexcept
    : _database{database}
    , _attributes{attributes}
    , _time_source{time_source}
    , _executor{executor}
    , _options{options}
{}

auto SchedulerCore::process_cycle() -> jb::core::Result<void, jb::core::Error>
{
    if (_failure) {
        return CoreResult<void>::failure(*_failure);
    }
    if (_options.cli_concurrency == 0 || _options.http_concurrency == 0 || _options.candidate_batch_size == 0) {
        return CoreResult<void>::failure(invalid_options());
    }

    auto const          now   = _time_source.utc_now();
    auto const          limit = page_size(_options);
    SchedulerRepository repository{_database, _attributes};
    auto                queues = load_runtime_queues(repository, limit);
    if (!queues) {
        return CoreResult<void>::failure(std::move(queues).error());
    }
    reconcile_fairness(*queues, _queue_weights, _cli_credits, _http_credits);
    auto capacity = load_capacity(repository, limit);
    if (!capacity) {
        return CoreResult<void>::failure(std::move(capacity).error());
    }
    auto cli_candidates          = CandidateCache{};
    auto http_candidates         = CandidateCache{};
    auto attempted_cli_blocking  = std::set<jb::core::Uuid>{};
    auto attempted_http_blocking = std::set<jb::core::Uuid>{};
    auto completion_processor =
        CompletionProcessor{[this](jb::core::Uuid const&    expected_run_id,
                                   AttemptCompletion const& completion) -> CoreResult<CompletionEffect> {
            auto completed =
                process_completion(_database, _attributes, _time_source, _active_attempts, expected_run_id, completion);
            if (!completed && !_failure) {
                _failure = completed.error();
            }
            return completed;
        }};

    while (true) {
        auto const first_type        = _cli_first ? JobType::Cli : JobType::Http;
        auto const second_type       = _cli_first ? JobType::Http : JobType::Cli;
        auto&      first_credits     = _cli_first ? _cli_credits : _http_credits;
        auto&      second_credits    = _cli_first ? _http_credits : _cli_credits;
        auto&      first_candidates  = _cli_first ? cli_candidates : http_candidates;
        auto&      second_candidates = _cli_first ? http_candidates : cli_candidates;
        auto&      first_blocking    = _cli_first ? attempted_cli_blocking : attempted_http_blocking;
        auto&      second_blocking   = _cli_first ? attempted_http_blocking : attempted_cli_blocking;

        auto first = dispatch_visit(_database,
                                    _attributes,
                                    _executor,
                                    repository,
                                    *queues,
                                    first_type,
                                    now,
                                    _options,
                                    *capacity,
                                    first_credits,
                                    first_candidates,
                                    _active_attempts,
                                    first_blocking,
                                    completion_processor);
        if (!first) {
            return CoreResult<void>::failure(std::move(first).error());
        }
        if (_failure) {
            return CoreResult<void>::failure(*_failure);
        }
        auto second = dispatch_visit(_database,
                                     _attributes,
                                     _executor,
                                     repository,
                                     *queues,
                                     second_type,
                                     now,
                                     _options,
                                     *capacity,
                                     second_credits,
                                     second_candidates,
                                     _active_attempts,
                                     second_blocking,
                                     completion_processor);
        if (!second) {
            return CoreResult<void>::failure(std::move(second).error());
        }
        if (_failure) {
            return CoreResult<void>::failure(*_failure);
        }
        if (!*first && !*second) {
            break;
        }
        // Empty cycles do not consume the token; the next productive round starts with the other type.
        _cli_first = !_cli_first;
    }
    return CoreResult<void>::success();
}

} // namespace jb::jobu::detail
