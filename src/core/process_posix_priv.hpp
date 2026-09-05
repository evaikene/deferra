#pragma once

#include "error.hpp"
#include "result.hpp"

#include <array>
#include <csignal> // IWYU pragma: keep Provides POSIX signal sets and dispositions.
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <sys/types.h>
#include <unistd.h>

namespace jb::core {
class Process;
}

namespace jb::core::priv {
class PreparedProcessRequest;

/// Fixed wire stages; neither the child record nor its parent translation contains request data.
enum class ProcessChildStage : std::uint8_t {
    None,
    Gate,
    Group,
    Descriptors,
    Signals,
    Directory,
    Identity,
    Exec
};

struct ProcessChildError {
    ProcessChildStage           stage{ProcessChildStage::None};
    std::array<std::uint8_t, 3> reserved{};
    int                         native_error{0};
};

/// Child syscall seam and fixed fault observations, copied before creation. No allocating callable enters the child.
struct ProcessChildOptions {
    uid_t (*effective_uid)(){::geteuid};
    ProcessChildStage fail_stage{ProcessChildStage::None};
    int               signal_before_reset{0};
    int               signal_before_exec{0};
};

/// Parent-only native seam. Overrides preserve errno and failure-without-mutation semantics.
class ProcessOperations {
public:
    virtual ~ProcessOperations() = default;
    virtual auto open_null(int flags) noexcept -> int;
    virtual auto make_pipe(int* descriptors) noexcept -> int;
    virtual auto make_gate(int* descriptors) noexcept -> int;
    virtual auto create_child() noexcept -> pid_t;
    virtual auto establish_group(pid_t pid) noexcept -> int;
    /// @throws std::exception from an injected test adapter before sending; throwing after release is forbidden.
    virtual auto release_gate(int fd, pid_t pid) -> ssize_t;

    virtual auto child_options() noexcept -> ProcessChildOptions { return {}; }
};

/// All owned descriptors are above 3 before child creation. Destruction never invokes user code.
struct ProcessDescriptors {
    int input{-1};
    int output{-1};
    int status_read{-1};
    int status_write{-1};
    int gate_parent{-1};
    int gate_child{-1};
    ProcessDescriptors() = default;
    ~ProcessDescriptors();
    ProcessDescriptors(ProcessDescriptors const&)                    = delete;
    auto operator=(ProcessDescriptors const&) -> ProcessDescriptors& = delete;
    void close_all() noexcept;
};

/// Owns parent-prepared pointer and signal arrays. The child reads these without mutation or allocation.
struct ProcessChildPlan {
    std::vector<char const*> candidates;
    char* const*             argv{nullptr};
    char* const*             envp{nullptr};
    char const*              directory{nullptr};
    std::array<int, NSIG>    signals{};
    std::size_t              signal_count{0};
    sigset_t                 blocked{};
    sigset_t                 target_mask{};
    struct sigaction         default_action{};
    bool                     require_non_root{false};
    ProcessChildOptions      options;
};

void close_process_fd(int& fd) noexcept;
auto process_error(char const* code, ErrorCategory category, char const* stage, int native_error = 0) -> Error;
auto process_child_error(ProcessChildError record) -> Error;
/// The descriptor bundle retains ownership of partially completed setup.
auto prepare_process_descriptors(ProcessDescriptors& descriptors, ProcessOperations& operations) -> Result<void, Error>;
auto prepare_process_child(PreparedProcessRequest const& request, ProcessChildOptions options)
    -> Result<ProcessChildPlan, Error>;
[[noreturn]] void execute_process_child(ProcessDescriptors const& descriptors, ProcessChildPlan const& plan) noexcept;

/// Injection is legal only while idle, on the owner thread. Production uses the default operations.
struct ProcessTestAccess {
    static void set_operations(Process& process, std::shared_ptr<ProcessOperations> operations) noexcept;
};
} // namespace jb::core::priv
