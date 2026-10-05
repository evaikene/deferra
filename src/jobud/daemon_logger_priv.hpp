#pragma once

#include "configuration_priv.hpp"
#include "logging.hpp"

#include <memory>

namespace jb::jobud::detail {

/// Synchronous stderr sink. Both formats escape line breaks, retain the first duplicate field,
/// replace invalid UTF-8 and render nonfinite doubles as null. Output failures discard the record.
/// Borrowed record storage is consumed before return; concurrent writes are serialized.
[[nodiscard]] auto make_daemon_logger(LoggingFormat format, jb::core::LogLevel level)
    -> std::shared_ptr<jb::core::Logger>;

/// Process-scope installation. Restores the previous logger after all workers, database and
/// signal-relay cleanup have completed. Construct/destroy on the composition thread.
class DaemonLogScope final {
public:
    DaemonLogScope(LoggingFormat format, jb::core::LogLevel level);
    ~DaemonLogScope();

    DaemonLogScope(DaemonLogScope const&)                    = delete;
    DaemonLogScope(DaemonLogScope&&)                         = delete;
    auto operator=(DaemonLogScope const&) -> DaemonLogScope& = delete;
    auto operator=(DaemonLogScope&&) -> DaemonLogScope&      = delete;

private:
    std::shared_ptr<jb::core::Logger> _previous;
};

} // namespace jb::jobud::detail
