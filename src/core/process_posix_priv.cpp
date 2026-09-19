#include "process_posix_priv.hpp"

#include "process_request_priv.hpp"

#include <cerrno>
#include <limits>
#include <string>
#include <utility>

#include <fcntl.h>

namespace jb::core::priv {
namespace {
auto normalize_descriptor(int& fd) noexcept -> bool
{
    if (fd > 3) {
        return true;
    }
    auto const normalized = ::fcntl(fd, F_DUPFD_CLOEXEC, 4);
    if (normalized < 0) {
        return false;
    }
    close_process_fd(fd);
    fd = normalized;
    return true;
}

[[noreturn]] void fail_child(int fd, ProcessChildStage stage, int native_error) noexcept
{
    // A small fixed record is the only child diagnostic. Retry interrupted/partial writes without allocating.
    ProcessChildError const record{.stage = stage, .native_error = native_error};
    auto const*             bytes = reinterpret_cast<char const*>(&record);
    std::size_t             written{0};
    while (written < sizeof(record)) {
        auto const count = ::write(fd, bytes + written, sizeof(record) - written);
        if (count > 0) {
            written += static_cast<std::size_t>(count);
        }
        else if (count < 0 && errno == EINTR) {
            continue;
        }
        else {
            break;
        }
    }
    ::_exit(127);
}

void check_child_stage(ProcessChildOptions const& options, int fd, ProcessChildStage stage) noexcept
{
    if (options.fail_stage == stage) {
        fail_child(fd, stage, EIO);
    }
}

void close_inherited_descriptors(ProcessChildPlan const& plan) noexcept
{
    check_child_stage(plan.options, 3, ProcessChildStage::DescriptorCleanup);

    // Keep only stdin, stdout, stderr, and the close-on-exec status channel. Without a native close-range operation,
    // use the finite bound captured before child creation; no descriptor discovery or allocation enters the child path.
    if (plan.options.close_range) {
        if (plan.options.close_range(4, std::numeric_limits<unsigned int>::max()) == 0) {
            return;
        }
        if (errno != ENOSYS) {
            fail_child(3, ProcessChildStage::DescriptorCleanup, errno);
        }
    }

    for (unsigned int fd = 4; fd < plan.descriptor_limit; ++fd) {
        if (::close(static_cast<int>(fd)) == 0 || errno == EBADF || errno == EINTR) {
            continue;
        }
        fail_child(3, ProcessChildStage::DescriptorCleanup, errno);
    }
}
} // namespace

void close_process_fd(int& fd) noexcept
{
    if (fd >= 0) {
        // Linux and Darwin release the descriptor even on EINTR; retrying could close a reused descriptor.
        ::close(fd);
        fd = -1;
    }
}

ProcessDescriptors::~ProcessDescriptors()
{
    close_all();
}

void ProcessDescriptors::close_all() noexcept
{
    close_process_fd(input);
    for (auto& fd : output_read) {
        close_process_fd(fd);
    }
    for (auto& fd : output_write) {
        close_process_fd(fd);
    }
    close_process_fd(status_read);
    close_process_fd(status_write);
    close_process_fd(gate_parent);
    close_process_fd(gate_child);
}

auto process_error(char const* code, ErrorCategory category, char const* stage, int native_error) -> Error
{
    return {.category = category,
            .code     = code,
            .message  = "Process operation failed",
            .detail   = std::string{stage} + ":" + std::to_string(native_error)};
}

auto process_child_error(ProcessChildError record) -> Error
{
    auto        category = ErrorCategory::Io;
    auto const* code     = "core.process.child_setup_failed";
    auto const* stage    = "child.record";
    switch (record.stage) {
        case ProcessChildStage::Gate:
            stage = "child.gate";
            break;
        case ProcessChildStage::Group:
            stage = "child.group";
            break;
        case ProcessChildStage::Descriptors:
            stage = "child.descriptors";
            break;
        case ProcessChildStage::DescriptorCleanup:
            stage = "child.descriptor_cleanup";
            break;
        case ProcessChildStage::Signals:
            stage = "child.signals";
            break;
        case ProcessChildStage::Directory:
            code  = "core.process.chdir_failed";
            stage = "child.chdir";
            break;
        case ProcessChildStage::Hardening:
            code     = "core.process.security_failed";
            category = ErrorCategory::PermissionDenied;
            stage    = "child.hardening";
            break;
        case ProcessChildStage::Identity:
            code     = "core.process.security_failed";
            category = ErrorCategory::PermissionDenied;
            stage    = "child.identity";
            break;
        case ProcessChildStage::Exec:
            code  = "core.process.exec_failed";
            stage = "child.exec";
            if (record.native_error == ENOENT || record.native_error == ENOTDIR) {
                category = ErrorCategory::NotFound;
            }
            else if (record.native_error == EACCES || record.native_error == EPERM) {
                category = ErrorCategory::PermissionDenied;
            }
            break;
        case ProcessChildStage::None:
            break;
    }
    return process_error(code, category, stage, record.native_error);
}

auto prepare_process_descriptors(ProcessDescriptors& descriptors, ProcessOperations& operations) -> Result<void, Error>
{
    // Adopt each descriptor before the next fallible operation so rejection can close a partial setup.
    // Normalize immediately; low numbers can be reused by subsequent opens without aliasing the child plan.
    auto failed = [] {
        auto const error    = errno;
        auto const category = error == EMFILE || error == ENFILE || error == ENOMEM ? ErrorCategory::ResourceExhausted
                                                                                    : ErrorCategory::Io;
        return Result<void, Error>::failure(
            process_error("core.process.resource_setup_failed", category, "parent.descriptors", error));
    };
    descriptors.input = operations.open_null(O_RDONLY);
    if (descriptors.input < 0 || !normalize_descriptor(descriptors.input)) {
        return failed();
    }
    int pair[2];
    if (operations.make_pipe(pair) != 0) {
        return failed();
    }
    descriptors.status_read  = pair[0];
    descriptors.status_write = pair[1];
    if (!normalize_descriptor(descriptors.status_read) || !normalize_descriptor(descriptors.status_write)) {
        return failed();
    }
    for (std::size_t i = 0; i < descriptors.output_read.size(); ++i) {
        if (operations.make_pipe(pair) != 0) {
            return failed();
        }
        descriptors.output_read[i]  = pair[0];
        descriptors.output_write[i] = pair[1];
        if (!normalize_descriptor(descriptors.output_read[i]) || !normalize_descriptor(descriptors.output_write[i])) {
            return failed();
        }
        auto const flags = ::fcntl(descriptors.output_read[i], F_GETFL);
        if (flags < 0 || ::fcntl(descriptors.output_read[i], F_SETFL, flags | O_NONBLOCK) != 0) {
            return failed();
        }
    }
    if (operations.make_gate(pair) != 0) {
        return failed();
    }
    descriptors.gate_parent = pair[0];
    descriptors.gate_child  = pair[1];
    if (!normalize_descriptor(descriptors.gate_parent) || !normalize_descriptor(descriptors.gate_child)) {
        return failed();
    }
    auto const flags = ::fcntl(descriptors.status_read, F_GETFL);
    if (flags < 0 || ::fcntl(descriptors.status_read, F_SETFL, flags | O_NONBLOCK) != 0) {
        return failed();
    }
    return Result<void, Error>::success();
}

auto prepare_process_child(PreparedProcessRequest const& request, ProcessOperations& operations)
    -> Result<ProcessChildPlan, Error>
{
    auto const options = operations.child_options();

    ProcessChildPlan plan;
    plan.options                = options;
    plan.argv                   = request.argv().data();
    plan.envp                   = request.envp().data();
    plan.directory              = request.working_directory().c_str();
    plan.require_non_root       = request.require_non_root();
    plan.prevent_privilege_gain = request.prevent_privilege_gain();
    if (plan.prevent_privilege_gain && !options.enable_privilege_hardening) {
        return Result<ProcessChildPlan, Error>::failure(
            process_error("core.process.security_unsupported", ErrorCategory::Unsupported, "hardening.unsupported"));
    }

    // The bounded close loop cannot discover a safe descriptor limit after child creation, so freeze the live parent
    // view with the other plan data while ordinary library operations remain available.
    if (operations.descriptor_close_limit(plan.descriptor_limit) != 0) {
        auto const error = errno;
        return Result<ProcessChildPlan, Error>::failure(process_error("core.process.resource_setup_failed",
                                                                      ErrorCategory::ResourceExhausted,
                                                                      "parent.descriptor_limit",
                                                                      error));
    }
    for (auto const& candidate : request.candidates()) {
        plan.candidates.push_back(candidate.c_str());
    }
    plan.default_action.sa_handler = SIG_DFL;
    sigemptyset(&plan.default_action.sa_mask);
    sigemptyset(&plan.target_mask);
    sigfillset(&plan.blocked);
    // libc may reserve signal numbers that cannot be queried/reset. Discover those before creation.
    for (int signal = 1; signal < NSIG; ++signal) {
        if (signal == SIGKILL || signal == SIGSTOP) {
            continue;
        }
        struct sigaction current{};
        if (::sigaction(signal, nullptr, &current) == 0) {
            plan.signals[plan.signal_count++] = signal;
        }
        else if (errno != EINVAL) {
            return Result<ProcessChildPlan, Error>::failure(
                process_error("core.process.child_setup_failed", ErrorCategory::Io, "parent.signals", errno));
        }
    }
    return Result<ProcessChildPlan, Error>::success(std::move(plan));
}

[[noreturn]] void execute_process_child(ProcessDescriptors const& descriptors, ProcessChildPlan const& plan) noexcept
{
    // Everything referenced here was frozen in the parent. No destructor, allocation, logger, mutex,
    // application handler, or exception path runs in this Process-controlled interval before execve()/_exit().
    // On macOS, host/runtime at-fork handlers have already run inside public fork() before this function begins.
    ::close(descriptors.status_read);
    for (auto const fd : descriptors.output_read) {
        ::close(fd);
    }
    ::close(descriptors.gate_parent);
    char    permission{};
    ssize_t received;
    do {
        received = ::read(descriptors.gate_child, &permission, 1);
    } while (received < 0 && errno == EINTR);
    if (received == 0) {
        ::_exit(127);
    }
    if (received != 1 || permission != 'x') {
        fail_child(descriptors.status_write, ProcessChildStage::Gate, received < 0 ? errno : EIO);
    }
    ::close(descriptors.gate_child);
    check_child_stage(plan.options, descriptors.status_write, ProcessChildStage::Group);
    // Parent establishes the group before release; repeating in the child closes both sides of the setup race.
    if (::setpgid(0, 0) != 0) {
        fail_child(descriptors.status_write, ProcessChildStage::Group, errno);
    }
    check_child_stage(plan.options, descriptors.status_write, ProcessChildStage::Descriptors);
    // Every source is >3, so mapping 0/1/2/3 cannot overwrite another required source.
    if (::dup2(descriptors.input, STDIN_FILENO) < 0 || ::dup2(descriptors.output_write[0], STDOUT_FILENO) < 0 ||
        ::dup2(descriptors.output_write[1], STDERR_FILENO) < 0 || ::dup2(descriptors.status_write, 3) < 0 ||
        ::fcntl(3, F_SETFD, FD_CLOEXEC) != 0) {
        fail_child(descriptors.status_write, ProcessChildStage::Descriptors, errno);
    }
    ::close(descriptors.input);
    for (auto const fd : descriptors.output_write) {
        ::close(fd);
    }
    ::close(descriptors.status_write);
    close_inherited_descriptors(plan);

    if (plan.options.signal_before_reset != 0) {
        ::kill(::getpid(), plan.options.signal_before_reset);
    }
    check_child_stage(plan.options, 3, ProcessChildStage::Signals);
    for (std::size_t i = 0; i < plan.signal_count; ++i) {
        if (::sigaction(plan.signals[i], &plan.default_action, nullptr) != 0) {
            fail_child(3, ProcessChildStage::Signals, errno);
        }
    }
    // Only unblock after every inherited disposition has been replaced, including handlers unrelated to SIGCHLD.
    if (::sigprocmask(SIG_SETMASK, &plan.target_mask, nullptr) != 0) {
        fail_child(3, ProcessChildStage::Signals, errno);
    }
    if (::chdir(plan.directory) != 0) {
        fail_child(3, ProcessChildStage::Directory, errno);
    }
    if (plan.prevent_privilege_gain) {
        // The request is strict: prove that the platform latched its protection before identity validation and exec.
        check_child_stage(plan.options, 3, ProcessChildStage::Hardening);
        if (plan.options.enable_privilege_hardening() != 0) {
            fail_child(3, ProcessChildStage::Hardening, errno);
        }
    }
    if (plan.options.signal_before_exec != 0) {
        ::kill(::getpid(), plan.options.signal_before_exec);
    }
    // The fixed child policy is authoritative even if the parent's identity changed before creation.
    if (plan.require_non_root && plan.options.effective_uid() == 0) {
        fail_child(3, ProcessChildStage::Identity, EPERM);
    }
    int remembered{ENOENT};
    for (auto const* candidate : plan.candidates) {
        ::execve(candidate, plan.argv, plan.envp);
        auto const error = errno;
        if (error == EACCES) {
            remembered = EACCES;
        }
        else if (error != ENOENT && error != ENOTDIR) {
            fail_child(3, ProcessChildStage::Exec, error);
        }
    }
    fail_child(3, ProcessChildStage::Exec, remembered);
}
} // namespace jb::core::priv
