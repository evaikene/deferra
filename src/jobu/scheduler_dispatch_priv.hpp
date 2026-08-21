#pragma once

#include "attempt_executor.hpp"
#include "result.hpp"

#include <optional>

namespace jb::db {
class Database;
}

namespace jb::jobu {

class AttributeRegistry;

namespace detail {

struct DispatchStart {
    AttemptKey                       key;
    std::optional<AttemptCompletion> immediate_completion;
};

[[nodiscard]] auto dispatch_selected(jb::db::Database&        database,
                                     AttributeRegistry const& attributes,
                                     AttemptExecutor&         executor,
                                     jb::core::Uuid const&    run_id,
                                     jb::core::UtcTimePoint   started_at,
                                     AttemptCompletionHandler completion)
    -> jb::core::Result<std::optional<DispatchStart>, jb::core::Error>;

} // namespace detail

} // namespace jb::jobu
