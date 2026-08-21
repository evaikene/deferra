#include "scheduler_core_priv.hpp"

#include "attempt_executor.hpp"
#include "scheduler_dispatch_priv.hpp"
#include "scheduler_repository_priv.hpp"
#include "time_source.hpp"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace jb::jobu::detail {

namespace {

template <typename T>
using CoreResult = jb::core::Result<T, jb::core::Error>;

constexpr std::size_t kMaximumRepositoryPageRows = 1000U;

struct CapacityState {
    std::uint64_t                           cli_running{0};
    std::uint64_t                           http_running{0};
    std::map<jb::core::Uuid, std::uint64_t> queue_slots;
};

struct EligibleQueue {
    QueueRuntime const*            runtime{nullptr};
    std::vector<DispatchCandidate> candidates;
};

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
        }
        after_id = page->back().run_id;
        if (page->size() < limit) {
            break;
        }
    }
    return CoreResult<CapacityState>::success(state);
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

auto record_start(CapacityState& state, JobType type, jb::core::Uuid const& queue_id) -> CoreResult<void>
{
    auto* running = &state.http_running;
    if (type == JobType::Cli) {
        running = &state.cli_running;
    }
    auto& queue_slots = state.queue_slots[queue_id];
    if (*running == std::numeric_limits<std::uint64_t>::max() ||
        queue_slots == std::numeric_limits<std::uint64_t>::max()) {
        return CoreResult<void>::failure(capacity_overflow());
    }
    ++*running;
    ++queue_slots;
    return CoreResult<void>::success();
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

auto dispatch_visit(jb::db::Database&                       database,
                    AttributeRegistry const&                attributes,
                    AttemptExecutor&                        executor,
                    SchedulerRepository&                    repository,
                    std::vector<QueueRuntime> const&        queues,
                    JobType                                 type,
                    jb::core::UtcTimePoint                  now,
                    SchedulerCoreOptions const&             options,
                    CapacityState&                          capacity,
                    std::map<jb::core::Uuid, std::int64_t>& credits) -> CoreResult<bool>
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
            credits[queue.id] = 0;
            continue;
        }
        auto candidates = repository.list_runnable(queue.id, type, now, limit);
        if (!candidates) {
            return CoreResult<bool>::failure(std::move(candidates).error());
        }
        if (candidates->empty()) {
            credits[queue.id] = 0;
            continue;
        }
        eligible.push_back({.runtime = &queue, .candidates = std::move(candidates).value()});
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

    auto* selected = &eligible.front();
    for (auto& queue : eligible) {
        auto const queue_credit    = credits[queue.runtime->id];
        auto const selected_credit = credits[selected->runtime->id];
        if (queue_credit > selected_credit ||
            (queue_credit == selected_credit && queue.runtime->id < selected->runtime->id)) {
            selected = &queue;
        }
    }
    auto subtracted = subtract_total(credits[selected->runtime->id], total);
    if (!subtracted) {
        return CoreResult<bool>::failure(std::move(subtracted).error());
    }

    for (auto const& candidate : selected->candidates) {
        auto dispatched =
            dispatch_selected(database, attributes, executor, candidate.run.id, now, [](AttemptCompletion const&) {});
        if (!dispatched) {
            return CoreResult<bool>::failure(std::move(dispatched).error());
        }
        if (!dispatched->has_value()) {
            continue;
        }
        auto recorded = record_start(capacity, type, selected->runtime->id);
        if (!recorded) {
            return CoreResult<bool>::failure(std::move(recorded).error());
        }
        return CoreResult<bool>::success(true);
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

    while (true) {
        auto const first_type     = _cli_first ? JobType::Cli : JobType::Http;
        auto const second_type    = _cli_first ? JobType::Http : JobType::Cli;
        auto&      first_credits  = _cli_first ? _cli_credits : _http_credits;
        auto&      second_credits = _cli_first ? _http_credits : _cli_credits;

        auto first = dispatch_visit(_database,
                                    _attributes,
                                    _executor,
                                    repository,
                                    *queues,
                                    first_type,
                                    now,
                                    _options,
                                    *capacity,
                                    first_credits);
        if (!first) {
            return CoreResult<void>::failure(std::move(first).error());
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
                                     second_credits);
        if (!second) {
            return CoreResult<void>::failure(std::move(second).error());
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
