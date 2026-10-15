#include "execution_events.hpp"

#include <chrono>
#include <optional>
#include <type_traits>

static_assert(std::is_copy_constructible_v<jb::jobu::ExecutionContext>);
static_assert(std::is_copy_constructible_v<jb::jobu::AttemptStarted>);
static_assert(std::is_copy_constructible_v<jb::jobu::AttemptFinished>);
static_assert(std::is_copy_constructible_v<jb::jobu::RetryScheduled>);
static_assert(std::is_copy_constructible_v<jb::jobu::RunFinished>);
static_assert(std::is_same_v<decltype(jb::jobu::AttemptFinished::elapsed), std::optional<std::chrono::milliseconds>>);
static_assert(std::is_same_v<decltype(jb::jobu::AttemptFinished::exit_code), std::optional<int>>);
static_assert(std::is_same_v<decltype(jb::jobu::AttemptFinished::signal_number), std::optional<int>>);
static_assert(std::is_same_v<decltype(jb::jobu::AttemptFinished::http_status), std::optional<unsigned>>);
static_assert(
    std::is_same_v<decltype(jb::jobu::RunFinished::completing_attempt), std::optional<jb::jobu::AttemptNumber>>);

auto main() -> int
{
    return 0;
}
