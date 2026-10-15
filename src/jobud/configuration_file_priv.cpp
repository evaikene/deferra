#include "configuration_file_priv.hpp"

#include "runtime_paths_priv.hpp"

#include <array>
#include <cerrno>
#include <cstddef>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace jb::jobud::detail {
namespace {

constexpr std::size_t kMaximumConfigurationBytes{65'536};

auto read_failed(PathFailure failure) -> StartupError
{
    // Preserve the configuration reader's public-to-startup code/category while retaining the
    // path helper's fixed reason and owning snapshot for the pre-logger formatter.
    failure.error.code = "jobud.config.read_failed";
    return {.code         = "jobud.config.read_failed",
            .message      = "Daemon configuration file could not be read",
            .category     = failure.error.category,
            .path_failure = std::move(failure)};
}

auto file_failure(char const* reason, jb::core::ErrorCategory category, PathDiagnostic diagnostic) -> StartupError
{
    return read_failed({
        .error      = {.category = category,
                       .code     = "jobud.config.read_failed",
                       .message  = "Daemon configuration file could not be read",
                       .detail   = reason},
        .diagnostic = std::move(diagnostic)
    });
}

} // anonymous namespace

auto read_configuration_file(std::filesystem::path const& path, bool allow_missing)
    -> jb::core::Result<std::optional<std::string>, StartupError>
{
    using ReadResult = jb::core::Result<std::optional<std::string>, StartupError>;

    // Configuration trust follows the invoking identity, independently of any later run-as target.
    auto const user   = ::geteuid();
    auto       parent = open_trusted_parent(path, user, allow_missing, PathRole::Configuration);
    if (!parent) {
        return ReadResult::failure(read_failed(std::move(parent).error()));
    }
    if (!*parent) {
        return ReadResult::success(std::nullopt);
    }

    // Nonblocking open lets us reject FIFOs without waiting for a writer. Regular-file reads are unaffected.
    // The descriptor, rather than a later pathname lookup, owns the file we inspect and read.
    auto       diagnostic = PathDiagnostic{.role            = PathRole::Configuration,
                                           .configured_path = path,
                                           .failing_path    = parent->value().path / parent->value().leaf,
                                           .required_uid    = user};
    auto const raw        = ::openat(parent->value().directory.get(),
                                     parent->value().leaf.c_str(),
                                     O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (raw < 0) {
        auto const saved_errno = errno;
        if (allow_missing && saved_errno == ENOENT) {
            return ReadResult::success(std::nullopt);
        }
        auto const category     = saved_errno == EACCES || saved_errno == EPERM || saved_errno == ELOOP
                                    ? jb::core::ErrorCategory::PermissionDenied
                                    : jb::core::ErrorCategory::Io;
        diagnostic.system_errno = saved_errno;
        if (saved_errno == ELOOP) {
            diagnostic.actual_type = std::filesystem::file_type::symlink;
        }
        return ReadResult::failure(file_failure("config_open", category, std::move(diagnostic)));
    }
    PathDescriptor file{raw};

    struct stat metadata{};
    if (::fstat(file.get(), &metadata) != 0) {
        diagnostic.system_errno = errno;
        return ReadResult::failure(file_failure("config_stat", jb::core::ErrorCategory::Io, std::move(diagnostic)));
    }
    set_path_metadata(diagnostic, metadata.st_mode, metadata.st_uid, metadata.st_gid);
    if (!S_ISREG(metadata.st_mode) || (metadata.st_uid != 0 && metadata.st_uid != user) ||
        (metadata.st_mode & 0022) != 0) {
        return ReadResult::failure(file_failure("config_owner_type_or_mode",
                                                jb::core::ErrorCategory::PermissionDenied,
                                                std::move(diagnostic)));
    }

    auto text = std::string{};
    auto data = std::array<char, 4096>{};
    while (true) {
        auto const count = ::read(file.get(), data.data(), data.size());
        if (count < 0) {
            auto const saved_errno = errno;
            if (saved_errno == EINTR) {
                continue;
            }
            diagnostic.system_errno = saved_errno;
            return ReadResult::failure(file_failure("config_read", jb::core::ErrorCategory::Io, std::move(diagnostic)));
        }
        if (count == 0) {
            break;
        }
        if (static_cast<std::size_t>(count) > kMaximumConfigurationBytes - text.size()) {
            return ReadResult::failure(
                {.code = "jobud.config.invalid", .message = "Daemon configuration file exceeds the size limit"});
        }
        text.append(data.data(), static_cast<std::size_t>(count));
    }
    return ReadResult::success(std::move(text));
}

} // namespace jb::jobud::detail
