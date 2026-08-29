#include "attempt_executor.hpp"

#include <type_traits>

using ExecutorResult = jb::core::Result<void, jb::core::Error>;

static_assert(std::is_same_v<std::underlying_type_t<jb::jobu::FailureDisposition>, std::uint8_t>);
static_assert(std::is_copy_constructible_v<jb::jobu::AttemptKey>);
static_assert(std::is_move_constructible_v<jb::jobu::AttemptStartRequest>);
static_assert(std::is_move_constructible_v<jb::jobu::AttemptOutputChannel>);
static_assert(std::is_move_constructible_v<jb::jobu::AttemptOutput>);
static_assert(std::is_move_constructible_v<jb::jobu::AttemptCompletion>);
static_assert(std::is_same_v<decltype(jb::jobu::AttemptOutputChannel::bytes), jb::core::ByteBuffer>);
static_assert(
    std::is_same_v<decltype(jb::jobu::AttemptOutput::primary), std::optional<jb::jobu::AttemptOutputChannel>>);
static_assert(std::is_same_v<decltype(jb::jobu::AttemptCompletion::output), std::optional<jb::jobu::AttemptOutput>>);
static_assert(std::is_invocable_r_v<void, jb::jobu::AttemptCompletionHandler, jb::jobu::AttemptCompletion>);

static_assert(std::is_abstract_v<jb::jobu::AttemptExecutor>);
static_assert(std::has_virtual_destructor_v<jb::jobu::AttemptExecutor>);
static_assert(std::is_same_v<decltype(&jb::jobu::AttemptExecutor::is_available),
                             bool (jb::jobu::AttemptExecutor::*)(jb::jobu::JobType) const noexcept>);
static_assert(std::is_same_v<decltype(&jb::jobu::AttemptExecutor::start),
                             ExecutorResult (jb::jobu::AttemptExecutor::*)(jb::jobu::AttemptStartRequest,
                                                                           jb::jobu::AttemptCompletionHandler)>);
static_assert(std::is_same_v<decltype(&jb::jobu::AttemptExecutor::cancel),
                             ExecutorResult (jb::jobu::AttemptExecutor::*)(jb::jobu::AttemptKey const&)>);

auto main() -> int
{
    return 0;
}
