#include "cli/cli_attempt_executor.hpp"

#include <type_traits>

static_assert(std::is_base_of_v<jb::core::Object, jb::jobu::cli::CliAttemptExecutor>);
static_assert(std::is_base_of_v<jb::jobu::AttemptExecutor, jb::jobu::cli::CliAttemptExecutor>);
static_assert(!std::is_copy_constructible_v<jb::jobu::cli::CliAttemptExecutor>);
static_assert(!std::is_move_constructible_v<jb::jobu::cli::CliAttemptExecutor>);

int main()
{
    return 0;
}
