#include "http/http_attempt_executor.hpp"

#include <concepts>
#include <type_traits>

using ExecutorResult = jb::core::Result<void, jb::core::Error>;
using HttpExecutor   = jb::jobu::http::HttpAttemptExecutor;

static_assert(std::is_final_v<HttpExecutor>);
static_assert(std::is_base_of_v<jb::jobu::AttemptExecutor, HttpExecutor>);
static_assert(std::has_virtual_destructor_v<HttpExecutor>);
static_assert(!std::is_copy_constructible_v<HttpExecutor>);
static_assert(!std::is_move_constructible_v<HttpExecutor>);
static_assert(std::constructible_from<HttpExecutor, jb::net::HttpClient&, jb::core::TimeSource&>);
static_assert(
    std::is_same_v<decltype(&HttpExecutor::is_available), bool (HttpExecutor::*)(jb::jobu::JobType) const noexcept>);
static_assert(std::is_same_v<decltype(&HttpExecutor::start),
                             ExecutorResult (HttpExecutor::*)(jb::jobu::AttemptStartRequest,
                                                              jb::jobu::AttemptCompletionHandler)>);
static_assert(
    std::is_same_v<decltype(&HttpExecutor::cancel), ExecutorResult (HttpExecutor::*)(jb::jobu::AttemptKey const&)>);

auto main() -> int
{
    return 0;
}
