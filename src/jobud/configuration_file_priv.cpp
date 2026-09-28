#include "configuration_file_priv.hpp"

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

class FileDescriptor {
public:
    explicit FileDescriptor(int value)
        : _value{value}
    {}

    ~FileDescriptor() { ::close(_value); }

    FileDescriptor(FileDescriptor const&)            = delete;
    FileDescriptor& operator=(FileDescriptor const&) = delete;

    [[nodiscard]] auto get() const noexcept -> int { return _value; }

private:
    int _value;
};

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

    // The descriptor, rather than a later pathname lookup, owns the file we inspect and read.
    auto const raw = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (raw < 0) {
        if (allow_missing && errno == ENOENT) {
            return ReadResult::success(std::nullopt);
        }
        auto const category = errno == EACCES || errno == EPERM || errno == ELOOP
                                ? jb::core::ErrorCategory::PermissionDenied
                                : jb::core::ErrorCategory::Io;
        return ReadResult::failure(read_failed(category));
    }
    FileDescriptor file{raw};

    struct stat metadata{};
    if (::fstat(file.get(), &metadata) != 0) {
        return ReadResult::failure(read_failed(jb::core::ErrorCategory::Io));
    }
    if (!S_ISREG(metadata.st_mode)) {
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
