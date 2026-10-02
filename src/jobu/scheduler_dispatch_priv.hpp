#pragma once

#include "attempt_executor.hpp"
#include "execution_telemetry.hpp"
#include "result.hpp"
#include "wait_repository_priv.hpp"

#include <optional>

namespace jb::db {
class Database;
}

namespace jb::jobu {

class AttributeRegistry;
class SecretProvider;

namespace detail {

struct MutationTiming;

struct DispatchStart {
    AttemptKey                       key;
    std::optional<AttemptCompletion> immediate_completion;
    std::optional<DelayedRun>        delayed;
};

[[nodiscard]] auto dispatch_selected(jb::db::Database&        database,
                                     AttributeRegistry const& attributes,
                                     AttemptExecutor&         executor,
                                     SecretProvider&          secrets,
                                     jb::core::Uuid const&    run_id,
                                     jb::core::UtcTimePoint   started_at,
                                     AttemptCompletionHandler completion)
    -> jb::core::Result<std::optional<DispatchStart>, jb::core::Error>;

/// Uses one caller-sampled boundary for revalidation, accounting and the durable start.
/// Returns accounting provenance without emitting; the caller reports failure after cleanup.
[[nodiscard]] auto dispatch_selected(jb::db::Database&        database,
                                     AttributeRegistry const& attributes,
                                     AttemptExecutor&         executor,
                                     SecretProvider&          secrets,
                                     jb::core::Uuid const&    run_id,
                                     MutationTiming const&    timing,
                                     AttemptCompletionHandler completion)
    -> TelemetryResult<std::optional<DispatchStart>>;

} // namespace detail

} // namespace jb::jobu
