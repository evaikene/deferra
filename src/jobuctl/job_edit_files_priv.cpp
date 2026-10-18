#include "job_edit_files_priv.hpp"

#include "time_source.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <fcntl.h>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace jb::jobuctl::detail {
namespace {

using namespace jb::core;

auto file_error(std::string code, std::string message) -> Error
{
    return {.category = ErrorCategory::Io, .code = std::move(code), .message = std::move(message)};
}

auto cleanup_error() -> Error
{
    return file_error("jobuctl.edit.cleanup_failed",
                      "Unable to remove all known edit files and the empty private directory");
}

class Descriptor final {
public:
    explicit Descriptor(int value)
        : _value{value}
    {}

    ~Descriptor()
    {
        if (_value >= 0) {
            static_cast<void>(::close(_value));
        }
    }

    Descriptor(Descriptor const&)                    = delete;
    auto operator=(Descriptor const&) -> Descriptor& = delete;

    auto get() const -> int { return _value; }

    auto release() -> int { return std::exchange(_value, -1); }

private:
    int _value;
};

auto safe_draft(struct stat const& metadata) -> bool
{
    return S_ISREG(metadata.st_mode) && metadata.st_uid == ::geteuid() && (metadata.st_mode & 0077) == 0;
}

} // namespace

JobEditFiles::JobEditFiles(std::filesystem::path directory, int parent, int directory_fd, JobEditFileIo io)
    : _directory{std::move(directory)}
    , _leaf{_directory.filename().string()}
    , _parent{parent}
    , _directory_fd{directory_fd}
    , _io{std::move(io)}
{
    if (!_io.read) {
        _io.read = ::read;
    }
    if (!_io.write) {
        _io.write = ::write;
    }
    if (!_io.finish) {
        _io.finish = ::close;
    }
    if (!_io.inspect) {
        _io.inspect = ::fstat;
    }
}

JobEditFiles::~JobEditFiles()
{
    // Destruction releases handles only. The coordinator must explicitly choose destructive cleanup.
    if (_directory_fd >= 0) {
        static_cast<void>(::close(_directory_fd));
    }
    if (_parent >= 0) {
        static_cast<void>(::close(_parent));
    }
}

JobEditFiles::JobEditFiles(JobEditFiles&& other) noexcept
    : _directory{std::move(other._directory)}
    , _leaf{std::move(other._leaf)}
    , _parent{std::exchange(other._parent, -1)}
    , _directory_fd{std::exchange(other._directory_fd, -1)}
    , _io{std::move(other._io)}
    , _draft_present{std::exchange(other._draft_present, false)}
    , _request_present{std::exchange(other._request_present, false)}
    , _request_ready{std::exchange(other._request_ready, false)}
    , _request_device{other._request_device}
    , _request_inode{other._request_inode}
{}

auto JobEditFiles::create(JsonValue const& draft, std::filesystem::path const& parent, JobEditFileIo io)
    -> Result<JobEditFiles, JobEditFileFailure>
{
    using Creation = Result<JobEditFiles, JobEditFileFailure>;
    auto text      = format_job_edit_draft(draft);
    if (!text) {
        return Creation::failure({.error = std::move(text).error()});
    }

    std::error_code error;
    auto            root = parent.empty() ? std::filesystem::temp_directory_path(error) : parent;
    if (!error) {
        root = std::filesystem::canonical(root, error);
    }
    Descriptor parent_fd{error ? -1 : ::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
    if (parent_fd.get() < 0) {
        return Creation::failure(
            {.error = file_error("jobuctl.edit.create_failed", "Unable to open the edit temporary directory")});
    }

    // Random names and exclusive mkdirat avoid races through a mutable parent pathname.
    SystemTimeSource clock;
    UuidV7Generator  generator{clock};
    for (std::size_t attempt = 0; attempt < 100; ++attempt) {
        auto id = generator.generate();
        if (!id) {
            return Creation::failure(
                {.error = file_error("jobuctl.edit.create_failed", "Unable to name a private edit directory")});
        }
        auto leaf = "jobuctl-edit-" + id->to_string();
        if (::mkdirat(parent_fd.get(), leaf.c_str(), 0700) != 0) {
            if (errno == EEXIST) {
                continue;
            }
            break;
        }

        auto const raw = ::openat(parent_fd.get(), leaf.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (raw < 0) {
            auto const removed = ::unlinkat(parent_fd.get(), leaf.c_str(), AT_REMOVEDIR) == 0;
            return Creation::failure(
                {.error = file_error("jobuctl.edit.create_failed", "Unable to open the private edit directory"),
                 .retained_directory = removed ? std::filesystem::path{} : root / leaf});
        }
        JobEditFiles files{root / leaf, parent_fd.release(), raw, std::move(io)};
        // Set the exact owner permissions even under a restrictive umask; no shared bits were ever granted.
        auto written = ::fchmod(raw, 0700) == 0
                         ? files.write_file("draft.jsonc", *text, false)
                         : Result<void, Error>::failure(
                               file_error("jobuctl.edit.create_failed", "Unable to protect the edit directory"));
        if (!written) {
            auto cleaned = files.cleanup();
            return Creation::failure({.error              = std::move(written).error(),
                                      .retained_directory = cleaned ? std::filesystem::path{} : files._directory});
        }
        return Creation::success(std::move(files));
    }
    return Creation::failure(
        {.error = file_error("jobuctl.edit.create_failed", "Unable to create a private edit directory")});
}

auto JobEditFiles::artifacts() const -> JobEditArtifacts
{
    return {.directory = _directory,
            .draft     = _directory / "draft.jsonc",
            .request   = _request_ready ? std::optional{_directory / "update-request.json"} : std::nullopt};
}

auto JobEditFiles::read_draft() const -> Result<std::string, Error>
{
    Descriptor file{::openat(_directory_fd, "draft.jsonc", O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK)};
    if (file.get() < 0) {
        if (errno == ENOENT) {
            return Result<std::string, Error>::failure({.category = ErrorCategory::InvalidArgument,
                                                        .code     = "jobuctl.edit.missing_draft",
                                                        .message  = "Edit draft is missing after editing"});
        }
        if (errno == ELOOP) {
            return Result<std::string, Error>::failure({.category = ErrorCategory::InvalidArgument,
                                                        .code     = "jobuctl.edit.unsafe_draft",
                                                        .message  = "Edit draft must not be a symbolic link"});
        }
        return Result<std::string, Error>::failure(
            file_error("jobuctl.edit.read_failed", "Unable to open a safe edit draft"));
    }
    struct stat metadata{};
    if (_io.inspect(file.get(), &metadata) != 0) {
        return Result<std::string, Error>::failure(
            file_error("jobuctl.edit.read_failed", "Unable to inspect the edit draft"));
    }
    if (!safe_draft(metadata)) {
        return Result<std::string, Error>::failure(
            {.category = ErrorCategory::InvalidArgument,
             .code     = "jobuctl.edit.unsafe_draft",
             .message  = "Edit draft must be a private regular file owned by the current user"});
    }

    // Enforce the bound while accumulating, including files that grow after fstat. One extra byte detects overflow.
    auto text  = std::string{};
    auto block = std::array<char, 8192>{};
    while (true) {
        auto const wanted = std::min(block.size(), max_job_edit_draft_bytes - text.size() + 1);
        auto const count  = _io.read(file.get(), block.data(), wanted);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0) {
            return Result<std::string, Error>::failure(
                file_error("jobuctl.edit.read_failed", "Unable to read the edit draft"));
        }
        if (count == 0) {
            return Result<std::string, Error>::success(std::move(text));
        }
        if (static_cast<std::size_t>(count) > max_job_edit_draft_bytes - text.size()) {
            return Result<std::string, Error>::failure({.category = ErrorCategory::InvalidArgument,
                                                        .code     = "jobuctl.edit.too_large",
                                                        .message  = "Job edit draft exceeds 16 MiB"});
        }
        text.append(block.data(), static_cast<std::size_t>(count));
    }
}

auto JobEditFiles::write_file(char const* name, std::string const& text, bool request) -> Result<void, Error>
{
    Descriptor file{::openat(_directory_fd, name, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600)};
    if (file.get() < 0) {
        return Result<void, Error>::failure(
            file_error("jobuctl.edit.write_failed", "Unable to create an edit file exclusively"));
    }
    struct stat metadata{};
    if (::fstat(file.get(), &metadata) != 0) {
        return Result<void, Error>::failure(
            file_error("jobuctl.edit.write_failed", "Unable to inspect the new edit file"));
    }
    if (request) {
        _request_present = true;
        _request_device  = metadata.st_dev;
        _request_inode   = metadata.st_ino;
    }
    else {
        _draft_present = true;
    }

    if (::fchmod(file.get(), 0600) != 0) {
        return Result<void, Error>::failure(file_error("jobuctl.edit.write_failed", "Unable to protect the edit file"));
    }
    auto written = std::size_t{0};
    while (written < text.size()) {
        auto const count = _io.write(file.get(), text.data() + written, text.size() - written);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            return Result<void, Error>::failure(
                file_error("jobuctl.edit.write_failed", "Unable to write the edit file"));
        }
        written += static_cast<std::size_t>(count);
    }
    // Closing is part of publication: a reported close error must not produce a ready-to-submit artifact.
    if (_io.finish(file.release()) != 0) {
        return Result<void, Error>::failure(file_error("jobuctl.edit.write_failed", "Unable to finish the edit file"));
    }
    return Result<void, Error>::success();
}

auto JobEditFiles::write_request(PreparedJobUpdate const& update) -> Result<void, Error>
{
    if (_request_ready || _request_present) {
        return Result<void, Error>::failure(
            file_error("jobuctl.edit.write_failed", "A minimal update request already exists"));
    }
    auto text = serialize_job_edit_request(update.params);
    if (!text) {
        return Result<void, Error>::failure(std::move(text).error());
    }
    auto written = write_file("update-request.json", *text, true);
    if (!written) {
        // Remove only our partial inode. A pre-existing or substituted entry is never claimed as ours.
        if (_request_present && remove_file("update-request.json", true)) {
            _request_present = false;
        }
        return written;
    }
    _request_ready = true;
    return Result<void, Error>::success();
}

auto JobEditFiles::remove_file(char const* name, bool request) const -> bool
{
    struct stat metadata{};
    if (::fstatat(_directory_fd, name, &metadata, AT_SYMLINK_NOFOLLOW) != 0) {
        return errno == ENOENT;
    }
    if (!S_ISREG(metadata.st_mode) || metadata.st_uid != ::geteuid()) {
        return false;
    }
    if (request && (metadata.st_dev != _request_device || metadata.st_ino != _request_inode)) {
        return false;
    }
    return ::unlinkat(_directory_fd, name, 0) == 0;
}

auto JobEditFiles::cleanup() -> Result<void, Error>
{
    if (_directory_fd < 0) {
        return Result<void, Error>::success();
    }
    // Verify directory identity before any deletion, so a renamed/replaced path cannot redirect cleanup.
    struct stat held{};
    struct stat named{};
    if (::fstat(_directory_fd, &held) != 0 || ::fstatat(_parent, _leaf.c_str(), &named, AT_SYMLINK_NOFOLLOW) != 0 ||
        !S_ISDIR(named.st_mode) || held.st_dev != named.st_dev || held.st_ino != named.st_ino) {
        return Result<void, Error>::failure(cleanup_error());
    }
    if (_draft_present) {
        if (!remove_file("draft.jsonc", false)) {
            return Result<void, Error>::failure(cleanup_error());
        }
        _draft_present = false;
    }
    if (_request_present) {
        if (!remove_file("update-request.json", true)) {
            return Result<void, Error>::failure(cleanup_error());
        }
        _request_present = false;
        _request_ready   = false;
    }
    if (::unlinkat(_parent, _leaf.c_str(), AT_REMOVEDIR) != 0) {
        return Result<void, Error>::failure(cleanup_error());
    }
    static_cast<void>(::close(std::exchange(_directory_fd, -1)));
    static_cast<void>(::close(std::exchange(_parent, -1)));
    return Result<void, Error>::success();
}

} // namespace jb::jobuctl::detail
