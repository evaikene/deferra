#include <jb/core/application.hpp>
#include <jb/jobu/cli/cli_attempt_executor.hpp>

int main(int argc, char const* argv[])
{
    jb::core::Application             app{argc, argv};
    jb::jobu::cli::CliAttemptExecutor executor;

    // Construction links the Process adapter without launching work or opting
    // into root execution. HTTP is never available through the CLI executor.
    return executor.is_available(jb::jobu::JobType::Http) ? 1 : 0;
}
