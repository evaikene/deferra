#include "process_request_priv.hpp"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace jb::core::priv {
namespace {

using PreparationResult = Result<std::unique_ptr<PreparedProcessRequest>, Error>;

auto invalid_request(std::string_view reason) -> Error
{
    return {.category = ErrorCategory::InvalidArgument,
            .code     = "core.process.invalid_request",
            .message  = "Process request is invalid",
            .detail   = std::string{reason}};
}

auto contains_nul(std::string_view text) -> bool
{
    return text.find('\0') != std::string_view::npos;
}

auto valid_environment_name(std::string_view name) -> bool
{
    auto const letter = [](char ch) { return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '_'; };
    if (name.empty() || !letter(name.front())) {
        return false;
    }
    for (char ch : name) {
        if (!letter(ch) && (ch < '0' || ch > '9')) {
            return false;
        }
    }
    return true;
}

/// Bound every increment before addition, including independently untrusted string lengths.
auto add_bytes(std::size_t& total, std::size_t amount, std::size_t limit) -> bool
{
    if (amount > limit - total) {
        return false;
    }
    total += amount;
    return true;
}

auto prepare_candidates(ProcessStartInfo const& info) -> Result<std::vector<std::string>, Error>
{
    using CandidatesResult = Result<std::vector<std::string>, Error>;
    if (info.executable.front() == '/') {
        return CandidatesResult::success({info.executable});
    }
    auto const path = info.environment.find("PATH");
    if (path == info.environment.end()) {
        return CandidatesResult::failure(invalid_request("path.missing"));
    }

    // Check each expansion before allocation, independently of exec's argv/environment limit.
    std::vector<std::string> candidates;
    std::string_view         remaining{path->second};
    std::size_t              bytes{0};
    while (true) {
        auto const colon     = remaining.find(':');
        auto const directory = remaining.substr(0, colon);
        if (directory.empty() || directory.front() != '/') {
            return CandidatesResult::failure(invalid_request("path.invalid_entry"));
        }
        if (candidates.size() == kMaxPathEntries) {
            return CandidatesResult::failure(invalid_request("path.too_many_entries"));
        }
        // Preserve supplied directory spelling; avoid adding another separator when it already ends in '/'.
        auto const separator_bytes = directory.back() == '/' ? std::size_t{0} : std::size_t{1};
        if (!add_bytes(bytes, directory.size(), kMaxPathCandidateBytes) ||
            !add_bytes(bytes, separator_bytes, kMaxPathCandidateBytes) ||
            !add_bytes(bytes, info.executable.size(), kMaxPathCandidateBytes) ||
            !add_bytes(bytes, 1, kMaxPathCandidateBytes)) {
            return CandidatesResult::failure(invalid_request("path.candidates_too_large"));
        }
        auto candidate = std::string{directory};
        if (separator_bytes != 0) {
            candidate += '/';
        }
        candidate += info.executable;
        candidates.push_back(std::move(candidate));
        if (colon == std::string_view::npos) {
            break;
        }
        remaining.remove_prefix(colon + 1);
    }
    return CandidatesResult::success(std::move(candidates));
}

} // namespace

auto checked_process_deadline(TimePoint launch_time, Duration timeout) -> Result<TimePoint, Error>
{
    if (timeout <= Duration::zero()) {
        return Result<TimePoint, Error>::failure(invalid_request("timeout.not_positive"));
    }
    // Subtract the positive duration from max first; never evaluate overflowing time-point addition.
    if (launch_time > TimePoint::max() - timeout) {
        return Result<TimePoint, Error>::failure(invalid_request("timeout.deadline_out_of_range"));
    }
    return Result<TimePoint, Error>::success(launch_time + timeout);
}

auto validate_process_signal_configuration() -> Result<void, Error>
{
    struct sigaction disposition{};
    if (sigaction(SIGCHLD, nullptr, &disposition) != 0) {
        auto const native_error = errno;
        return Result<void, Error>::failure({.category = ErrorCategory::Conflict,
                                             .code     = "core.process.signal_configuration",
                                             .message  = "Process requires waitable child status",
                                             .detail   = "sigchld.query_failed:" + std::to_string(native_error)});
    }
    // Do not repair process-global policy: another component may deliberately own this disposition.
    if (disposition.sa_handler == SIG_IGN || (disposition.sa_flags & SA_NOCLDWAIT) != 0) {
        return Result<void, Error>::failure({.category = ErrorCategory::Conflict,
                                             .code     = "core.process.signal_configuration",
                                             .message  = "Process requires waitable child status",
                                             .detail   = "sigchld.not_waitable"});
    }
    return Result<void, Error>::success();
}

auto prepare_process_request(ProcessStartInfo info, TimePoint launch_time, long runtime_arg_max) -> PreparationResult
{
    if (info.executable.empty() || info.executable.size() > kMaxProcessPathBytes || contains_nul(info.executable) ||
        (info.executable.front() != '/' && info.executable.find('/') != std::string::npos)) {
        return PreparationResult::failure(invalid_request("executable.invalid"));
    }
    auto const& directory = info.working_directory.native();
    if (directory.empty() || directory.front() != '/' || directory.size() > kMaxProcessPathBytes ||
        contains_nul(directory)) {
        return PreparationResult::failure(invalid_request("working_directory.invalid"));
    }
    if (info.arguments.size() > kMaxProcessArguments) {
        return PreparationResult::failure(invalid_request("arguments.too_many"));
    }
    if (info.termination_grace < Duration::zero() || info.termination_grace > std::chrono::minutes{5}) {
        return PreparationResult::failure(invalid_request("termination_grace.out_of_range"));
    }
    std::optional<TimePoint> deadline;
    if (info.timeout) {
        auto checked = checked_process_deadline(launch_time, *info.timeout);
        if (!checked) {
            return PreparationResult::failure(checked.error());
        }
        deadline = checked.value();
    }

    // Account incrementally rather than multiplying unbounded counts. Both pointer-array terminators count.
    std::size_t bytes{2 * sizeof(char*)};
    auto const  add_string = [&bytes](std::size_t size) {
        return add_bytes(bytes, size, kMaxProcessArgumentBytes) &&
               add_bytes(bytes, 1 + sizeof(char*), kMaxProcessArgumentBytes);
    };
    if (!add_string(info.executable.size())) {
        return PreparationResult::failure(invalid_request("aggregate.too_large"));
    }
    for (auto const& argument : info.arguments) {
        if (contains_nul(argument)) {
            return PreparationResult::failure(invalid_request("argument.contains_nul"));
        }
        if (!add_string(argument.size())) {
            return PreparationResult::failure(invalid_request("aggregate.too_large"));
        }
    }
    for (auto const& [name, value] : info.environment) {
        if (!valid_environment_name(name)) {
            return PreparationResult::failure(invalid_request("environment.invalid_name"));
        }
        if (contains_nul(value)) {
            return PreparationResult::failure(invalid_request("environment.value_contains_nul"));
        }
        if (!add_bytes(bytes, name.size(), kMaxProcessArgumentBytes) ||
            !add_bytes(bytes, 1, kMaxProcessArgumentBytes) || !add_string(value.size())) {
            return PreparationResult::failure(invalid_request("aggregate.too_large"));
        }
    }
    if (runtime_arg_max <= static_cast<long>(kProcessArgumentSafetyMargin)) {
        return PreparationResult::failure(invalid_request("aggregate.runtime_limit_unavailable"));
    }
    if (bytes > static_cast<unsigned long>(runtime_arg_max) - kProcessArgumentSafetyMargin) {
        return PreparationResult::failure(invalid_request("aggregate.runtime_limit"));
    }
    auto candidates = prepare_candidates(info);
    if (!candidates) {
        return PreparationResult::failure(candidates.error());
    }

    // Allocate the immutable owner once; publish pointers only after every owning string reaches its final location.
    auto prepared                     = std::unique_ptr<PreparedProcessRequest>{new PreparedProcessRequest};
    prepared->_candidates             = std::move(candidates).value();
    prepared->_working_directory      = directory;
    prepared->_deadline               = deadline;
    prepared->_termination_grace      = info.termination_grace;
    prepared->_require_non_root       = info.require_non_root;
    prepared->_prevent_privilege_gain = info.prevent_privilege_gain;
    prepared->_argument_bytes         = bytes;
    prepared->_arguments.reserve(info.arguments.size() + 1);
    prepared->_arguments.push_back(std::move(info.executable));
    for (auto& argument : info.arguments) {
        prepared->_arguments.push_back(std::move(argument));
    }
    prepared->_environment.reserve(info.environment.size());
    for (auto const& [name, value] : info.environment) {
        auto entry  = name;
        entry      += '=';
        entry      += value;
        prepared->_environment.push_back(std::move(entry));
    }
    prepared->_argv.reserve(prepared->_arguments.size() + 1);
    for (auto& argument : prepared->_arguments) {
        prepared->_argv.push_back(argument.data());
    }
    prepared->_argv.push_back(nullptr);
    prepared->_envp.reserve(prepared->_environment.size() + 1);
    for (auto& entry : prepared->_environment) {
        prepared->_envp.push_back(entry.data());
    }
    prepared->_envp.push_back(nullptr);
    return PreparationResult::success(std::move(prepared));
}

} // namespace jb::core::priv
