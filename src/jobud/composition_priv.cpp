#include "composition_priv.hpp"

#include "attempt_executor_group.hpp"
#include "cli/cli_attempt_executor.hpp"
#include "http/http_attempt_executor.hpp"
#include "job.hpp"
#include "logging.hpp"

#include <utility>

namespace jb::jobud::detail {

auto register_attempt_executors(jb::jobu::AttemptExecutorGroup&                                group,
                                jb::net::HttpClient&                                           http_client,
                                jb::core::TimeSource&                                          time_source,
                                bool                                                           allow_root_cli,
                                std::unique_ptr<jb::jobu::cli::detail::EffectiveIdentityProbe> identity)
    -> jb::core::Result<void, jb::core::Error>
{
    using namespace jb::jobu;

    // Only the group owns these Objects; assigning an Object parent would create a second owner.
    auto registered = group.add(JobType::Http, std::make_unique<http::HttpAttemptExecutor>(http_client, time_source));
    if (!registered) {
        return registered;
    }

    if (!identity) {
        identity = cli::detail::make_system_identity_probe();
    }
    auto const warn_root_override = allow_root_cli && identity->effective_user_id() == 0U;
    auto       cli_executor =
        cli::detail::CliAttemptExecutorFactory::create({.allow_root = allow_root_cli}, std::move(identity));
    registered = group.add(JobType::Cli, std::move(cli_executor));
    if (!registered) {
        return registered;
    }

    // Warn once at startup, not on each availability query or attempt. Never include job-supplied data.
    if (warn_root_override) {
        jb::core::log_warning("UNSAFE: --allow-root-cli enables command execution as root");
    }
    return registered;
}

} // namespace jb::jobud::detail
