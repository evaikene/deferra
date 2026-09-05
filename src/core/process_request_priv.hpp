#pragma once

#include "error.hpp"
#include "event_loop_types.hpp"
#include "process.hpp"
#include "result.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace jb::core::priv {

inline constexpr std::size_t kMaxProcessPathBytes{4096};
inline constexpr std::size_t kMaxProcessArguments{1024};
inline constexpr std::size_t kMaxProcessArgumentBytes{std::size_t{256} * 1024};
inline constexpr std::size_t kMaxPathEntries{256};
inline constexpr std::size_t kMaxPathCandidateBytes{std::size_t{256} * 1024};
/// Reserve space for runtime execution overhead in addition to our explicit string/pointer accounting.
inline constexpr std::size_t kProcessArgumentSafetyMargin{2048};

/// Checks before adding; accepts every positive timeout representable from the supplied launch time.
[[nodiscard]] auto checked_process_deadline(TimePoint launch_time, Duration timeout) -> Result<TimePoint, Error>;
/// Observes host SIGCHLD policy without modifying it or consuming any signal.
[[nodiscard]] auto validate_process_signal_configuration() -> Result<void, Error>;

/// Frozen parent-prepared storage. Moving its unique owner never moves strings or invalidates argv/envp pointers.
/// No further storage mutation is permitted once preparation publishes this object.
class PreparedProcessRequest final {
public:
    ~PreparedProcessRequest()                                                = default;
    PreparedProcessRequest(PreparedProcessRequest const&)                    = delete;
    PreparedProcessRequest(PreparedProcessRequest&&)                         = delete;
    auto operator=(PreparedProcessRequest const&) -> PreparedProcessRequest& = delete;
    auto operator=(PreparedProcessRequest&&) -> PreparedProcessRequest&      = delete;

    [[nodiscard]] auto candidates() const noexcept -> std::vector<std::string> const& { return _candidates; }

    [[nodiscard]] auto argv() const noexcept -> std::vector<char*> const& { return _argv; }

    [[nodiscard]] auto envp() const noexcept -> std::vector<char*> const& { return _envp; }

    [[nodiscard]] auto working_directory() const noexcept -> std::string const& { return _working_directory; }

    [[nodiscard]] auto deadline() const noexcept -> std::optional<TimePoint> { return _deadline; }

    [[nodiscard]] auto termination_grace() const noexcept -> Duration { return _termination_grace; }

    [[nodiscard]] auto require_non_root() const noexcept -> bool { return _require_non_root; }

    [[nodiscard]] auto prevent_privilege_gain() const noexcept -> bool { return _prevent_privilege_gain; }

    [[nodiscard]] auto argument_bytes() const noexcept -> std::size_t { return _argument_bytes; }

private:
    PreparedProcessRequest() = default;
    friend auto prepare_process_request(ProcessStartInfo, TimePoint, long)
        -> Result<std::unique_ptr<PreparedProcessRequest>, Error>;

    std::vector<std::string> _candidates;
    std::vector<std::string> _arguments;
    std::vector<std::string> _environment;
    std::vector<char*>       _argv;
    std::vector<char*>       _envp;
    std::string              _working_directory;
    std::optional<TimePoint> _deadline;
    Duration                 _termination_grace{};
    std::size_t              _argument_bytes{0};
    bool                     _require_non_root{false};
    bool                     _prevent_privilege_gain{false};
};

/// Pure validation/preparation with caller-supplied launch time and sysconf(_SC_ARG_MAX) observation.
/// Accounting includes argv[0], each argument NUL, NAME=VALUE NULs, and both terminated pointer arrays.
/// PATH expansion is independently bounded. No filesystem access or ambient environment lookup occurs.
[[nodiscard]] auto prepare_process_request(ProcessStartInfo info, TimePoint launch_time, long runtime_arg_max)
    -> Result<std::unique_ptr<PreparedProcessRequest>, Error>;

} // namespace jb::core::priv
