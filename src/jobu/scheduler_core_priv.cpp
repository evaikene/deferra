#include "scheduler_core_priv.hpp"

#include "attempt_executor.hpp"
#include "scheduler_dispatch_priv.hpp"
#include "scheduler_repository_priv.hpp"
#include "time_source.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace jb::jobu::detail {

namespace {

template <typename T>
using CoreResult = jb::core::Result<T, jb::core::Error>;

constexpr std::size_t kMaximumRepositoryPageRows = 1000U;

struct CapacityState {
    std::uint64_t cli_running{0};
    std::uint64_t http_running{0};
    std::uint64_t queue_slots{0};
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

auto load_capacity(SchedulerRepository& repository, jb::core::Uuid const& selected_queue, std::size_t limit)
    -> CoreResult<CapacityState>
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
            if (row.queue_id == selected_queue) {
                auto queue = checked_add(state.queue_slots, row.usage.queue_slots);
                if (!queue) {
                    return CoreResult<CapacityState>::failure(std::move(queue).error());
                }
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

void record_start(CapacityState& state, JobType type)
{
    if (type == JobType::Cli) {
        ++state.cli_running;
    }
    else {
        ++state.http_running;
    }
    ++state.queue_slots;
}

auto dispatch_type(jb::db::Database&           database,
                   AttributeRegistry const&    attributes,
                   AttemptExecutor&            executor,
                   SchedulerRepository&        repository,
                   QueueRuntime const&         queue,
                   JobType                     type,
                   jb::core::UtcTimePoint      now,
                   SchedulerCoreOptions const& options,
                   CapacityState&              capacity) -> CoreResult<void>
{
    auto const global_limit = global_limit_for(options, type);
    auto const limit        = page_size(options);

    while (running_for(capacity, type) < global_limit && capacity.queue_slots < queue.concurrency_limit) {
        if (!executor.is_available(type)) {
            break;
        }
        auto candidates = repository.list_runnable(queue.id, type, now, limit);
        if (!candidates) {
            return CoreResult<void>::failure(std::move(candidates).error());
        }
        if (candidates->empty()) {
            break;
        }

        auto started = std::size_t{0};
        for (auto const& candidate : *candidates) {
            if (running_for(capacity, type) >= global_limit || capacity.queue_slots >= queue.concurrency_limit) {
                break;
            }
            auto dispatched =
                dispatch_selected(database, attributes, executor, candidate.run.id, now, [](AttemptCompletion const&) {
                });
            if (!dispatched) {
                return CoreResult<void>::failure(std::move(dispatched).error());
            }
            if (!dispatched->has_value()) {
                continue;
            }
            record_start(capacity, type);
            ++started;
        }

        if (candidates->size() < limit || started == 0) {
            break;
        }
    }
    return CoreResult<void>::success();
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
    if (queues->empty()) {
        return CoreResult<void>::success();
    }

    // Stage 4.11 intentionally drives one queue. Stage 4.12 replaces this selection with weighted arbitration.
    auto const& queue    = queues->front();
    auto        capacity = load_capacity(repository, queue.id, limit);
    if (!capacity) {
        return CoreResult<void>::failure(std::move(capacity).error());
    }

    auto cli =
        dispatch_type(_database, _attributes, _executor, repository, queue, JobType::Cli, now, _options, *capacity);
    if (!cli) {
        return CoreResult<void>::failure(std::move(cli).error());
    }
    return dispatch_type(_database, _attributes, _executor, repository, queue, JobType::Http, now, _options, *capacity);
}

} // namespace jb::jobu::detail
