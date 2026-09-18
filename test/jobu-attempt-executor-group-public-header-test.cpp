#include "attempt_executor_group.hpp"

#include "object.hpp"

#include <memory>
#include <type_traits>

using Group          = jb::jobu::AttemptExecutorGroup;
using ExecutorResult = jb::core::Result<void, jb::core::Error>;

static_assert(std::is_base_of_v<jb::jobu::AttemptExecutor, Group>);
static_assert(!std::is_base_of_v<jb::core::Object, Group>);
static_assert(!std::is_copy_constructible_v<Group>);
static_assert(!std::is_move_constructible_v<Group>);
static_assert(std::is_same_v<decltype(&Group::add),
                             ExecutorResult (Group::*)(jb::jobu::JobType, std::unique_ptr<jb::jobu::AttemptExecutor>)>);
static_assert(std::is_same_v<decltype(&Group::is_available), bool (Group::*)(jb::jobu::JobType) const noexcept>);
static_assert(
    std::is_same_v<decltype(&Group::start),
                   ExecutorResult (Group::*)(jb::jobu::AttemptStartRequest, jb::jobu::AttemptCompletionHandler)>);
static_assert(std::is_same_v<decltype(&Group::cancel), ExecutorResult (Group::*)(jb::jobu::AttemptKey const&)>);

auto main() -> int
{
    Group group;
    return group.is_available(jb::jobu::JobType::Cli) ? 1 : 0;
}
