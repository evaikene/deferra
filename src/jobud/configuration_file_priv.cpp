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

auto read_failed(jb::core::ErrorCategory category) -> StartupError
{
    return {.code     = "jobud.config.read_failed",
            .message  = "Daemon configuration file could not be read",
            .category = category};
}

} // anonymous namespace

auto read_configuration_file(std::filesystem::path const& path, bool allow_missing)
    -> jb::core::Result<std::optional<std::string>, StartupError>
{
    using ReadResult = jb::core::Result<std::optional<std::string>, StartupError>;

    // Configuration trust follows the invoking identity, independently of any later run-as target.
    auto const user   = ::geteuid();
    auto       parent = open_trusted_parent(path, user, allow_missing);
    if (!parent) {
        return ReadResult::failure(read_failed(parent.error().category));
    }
    if (!*parent) {
        return ReadResult::success(std::nullopt);
    }

    // Nonblocking open lets us reject FIFOs without waiting for a writer. Regular-file reads are unaffected.
    // The descriptor, rather than a later pathname lookup, owns the file we inspect and read.
    auto const raw = ::openat(parent->value().directory.get(),
                              parent->value().leaf.c_str(),
                              O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (raw < 0) {
        if (allow_missing && errno == ENOENT) {
            return ReadResult::success(std::nullopt);
        }
        auto const category = errno == EACCES || errno == EPERM || errno == ELOOP
                                ? jb::core::ErrorCategory::PermissionDenied
                                : jb::core::ErrorCategory::Io;
        return ReadResult::failure(read_failed(category));
    }
    PathDescriptor file{raw};

    struct stat metadata{};
    if (::fstat(file.get(), &metadata) != 0) {
        return ReadResult::failure(read_failed(jb::core::ErrorCategory::Io));
    }
    if (!S_ISREG(metadata.st_mode) || (metadata.st_uid != 0 && metadata.st_uid != user) ||
        (metadata.st_mode & 0022) != 0) {
        return ReadResult::failure(read_failed(jb::core::ErrorCategory::PermissionDenied));
    }

    auto text = std::string{};
    auto data = std::array<char, 4096>{};
    while (true) {
        auto const count = ::read(file.get(), data.data(), data.size());
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return ReadResult::failure(read_failed(jb::core::ErrorCategory::Io));
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
