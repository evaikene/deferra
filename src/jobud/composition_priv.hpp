#pragma once

#include "cli/process_adapter_priv.hpp"
#include "error.hpp"
#include "result.hpp"

#include <memory>

namespace jb::core {
class TimeSource;
}

namespace jb::net {
class HttpClient;
}

namespace jb::jobu {
class AttemptExecutorGroup;
}

namespace jb::jobud::detail {

/// Registers unparented runners on the owner EventLoop thread. The client and clock must outlive the group.
/// A failed registration leaves earlier registrations owned by the group for ordinary scope cleanup.
/// The optional private identity probe controls both the startup warning and the CLI executor's live checks.
[[nodiscard]] auto
register_attempt_executors(jb::jobu::AttemptExecutorGroup&                                group,
                           jb::net::HttpClient&                                           http_client,
                           jb::core::TimeSource&                                          time_source,
                           bool                                                           allow_root_cli,
                           std::unique_ptr<jb::jobu::cli::detail::EffectiveIdentityProbe> identity = {})
    -> jb::core::Result<void, jb::core::Error>;

} // namespace jb::jobud::detail
