#include "attempt_executor_group.hpp"

#include "object.hpp"

#include <cstddef>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>

namespace jb::jobu {

namespace {

using GroupResult = jb::core::Result<void, jb::core::Error>;

auto group_error(jb::core::ErrorCategory category, std::string code, std::string message) -> jb::core::Error
{
    return {
        .category = category,
        .code     = std::move(code),
        .message  = std::move(message),
    };
}

auto supported_type(JobType type) noexcept -> bool
{
    switch (type) {
        case JobType::Cli:
        case JobType::Http:
            return true;
    }
    return false;
}

struct AttemptKeyHash {
    auto operator()(AttemptKey const& key) const noexcept -> std::size_t
    {
        auto seed = std::hash<jb::core::Uuid>{}(key.run_id);
        seed ^= std::hash<AttemptNumber>{}(key.attempt_number) + std::size_t{0x9e3779b9U} + (seed << 6U) + (seed >> 2U);
        return seed;
    }
};

} // anonymous namespace

struct AttemptExecutorGroup::Private {
    std::map<JobType, std::unique_ptr<AttemptExecutor>>              executors;
    std::unordered_map<AttemptKey, AttemptExecutor*, AttemptKeyHash> routes;

    [[nodiscard]] auto add(JobType type, std::unique_ptr<AttemptExecutor> executor) -> GroupResult
    {
        if (!supported_type(type)) {
            return GroupResult::failure(group_error(jb::core::ErrorCategory::Unsupported,
                                                    "jobu.executor.unsupported_type",
                                                    "The executor group does not recognize the runner type"));
        }
        if (!executor) {
            return GroupResult::failure(group_error(jb::core::ErrorCategory::InvalidArgument,
                                                    "jobu.executor.invalid_registration",
                                                    "The executor group requires an owned executor"));
        }
        if (executors.contains(type)) {
            return GroupResult::failure(group_error(jb::core::ErrorCategory::Conflict,
                                                    "jobu.executor.duplicate_type",
                                                    "The executor group already owns this runner type"));
        }

        // An Object parent and this unique_ptr would be competing lifetime owners. Reject before storage; destroying
        // the by-value pointer then lets Object unlink itself from its existing parent exactly once.
        auto* object = dynamic_cast<jb::core::Object*>(executor.get());
        if (object != nullptr && object->parent() != nullptr) {
            return GroupResult::failure(group_error(jb::core::ErrorCategory::InvalidArgument,
                                                    "jobu.executor.invalid_registration",
                                                    "An Object-derived executor must be unparented"));
        }

        executors.emplace(type, std::move(executor));
        return GroupResult::success();
    }

    [[nodiscard]] auto is_available(JobType type) const noexcept -> bool
    {
        auto const executor = executors.find(type);
        return executor != executors.end() && executor->second->is_available(type);
    }

    [[nodiscard]] auto start(AttemptStartRequest request, AttemptCompletionHandler completion) -> GroupResult
    {
        auto const executor_entry = executors.find(request.type);
        if (executor_entry == executors.end()) {
            return GroupResult::failure(group_error(jb::core::ErrorCategory::Unsupported,
                                                    "jobu.executor.unsupported_type",
                                                    "No executor is registered for the runner type"));
        }

        auto const key = request.key;
        if (routes.contains(key)) {
            return GroupResult::failure(group_error(jb::core::ErrorCategory::Conflict,
                                                    "jobu.executor.duplicate_attempt",
                                                    "The attempt key is already active"));
        }

        auto* executor          = executor_entry->second.get();
        auto  routed_completion = AttemptCompletionHandler{};
        if (completion) {
            routed_completion =
                [this, key, completion = std::move(completion)](AttemptCompletion child_completion) mutable {
                    // Retire the accepted key, not the child-reported key. A malformed completion remains visible to
                    // the Scheduler while callback reentrancy sees the route as inactive.
                    routes.erase(key);
                    completion(std::move(child_completion));
                };
        }

        auto started = executor->start(std::move(request), std::move(routed_completion));
        if (!started) {
            return GroupResult::failure(std::move(started).error());
        }

        routes.emplace(key, executor);
        return GroupResult::success();
    }

    [[nodiscard]] auto cancel(AttemptKey const& key) -> GroupResult
    {
        auto const route = routes.find(key);
        if (route == routes.end()) {
            return GroupResult::failure(group_error(jb::core::ErrorCategory::NotFound,
                                                    "jobu.executor.attempt_not_found",
                                                    "The attempt key is not active"));
        }
        return route->second->cancel(key);
    }

    void shutdown()
    {
        // Child executors own the wrappers that reference this routing state. Destroy them while the route map is
        // still alive, relying on the AttemptExecutor contract to suppress callbacks during child destruction.
        executors.clear();
        routes.clear();
    }
};

AttemptExecutorGroup::AttemptExecutorGroup()
    : _data{std::make_unique<Private>()}
{}

AttemptExecutorGroup::~AttemptExecutorGroup()
{
    _data->shutdown();
}

auto AttemptExecutorGroup::add(JobType type, std::unique_ptr<AttemptExecutor> executor) -> GroupResult
{
    return _data->add(type, std::move(executor));
}

auto AttemptExecutorGroup::is_available(JobType type) const noexcept -> bool
{
    return _data->is_available(type);
}

auto AttemptExecutorGroup::start(AttemptStartRequest request, AttemptCompletionHandler completion) -> GroupResult
{
    return _data->start(std::move(request), std::move(completion));
}

auto AttemptExecutorGroup::cancel(AttemptKey const& key) -> GroupResult
{
    return _data->cancel(key);
}

} // namespace jb::jobu
