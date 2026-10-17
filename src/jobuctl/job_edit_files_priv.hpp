#pragma once

#include "error.hpp"
#include "job_edit_priv.hpp"
#include "json.hpp"
#include "result.hpp"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>

namespace jb::jobuctl::detail {

/// Synchronous file-only test seams; empty operations use POSIX. Captures must outlive the owner.
/// finish must close its descriptor even when reporting failure; close is never retried after EINTR.
struct JobEditFileIo {
    std::function<ssize_t(int, void*, std::size_t)>       read;
    std::function<ssize_t(int, void const*, std::size_t)> write;
    std::function<int(int)>                               finish;
    std::function<int(int, struct stat*)>                 inspect;
};

/// Owning locations for recovery. request is present only after a complete, closed minimal request was written.
/// Paths are data: callers must escape them for human display. No file contents are included.
struct JobEditArtifacts {
    std::filesystem::path                directory;
    std::filesystem::path                draft;
    std::optional<std::filesystem::path> request;
};

/// A creation failure may leave artifacts only when safe, known-file cleanup itself failed.
struct JobEditFileFailure {
    jb::core::Error       error;
    std::filesystem::path retained_directory;
};

/// Owns private directory handles, without an Application or Object lifetime.
/// Completed creation retains files on destruction, so an early return cannot discard an operator's edits.
/// Call cleanup() explicitly after success, no-op, or cancellation; failures can use artifacts() for recovery.
class JobEditFiles final {
public:
    /// Formats before creating files; an empty parent selects the system temporary directory.
    /// Creates a 0700 directory and exclusive 0600 draft, closing the draft before returning.
    [[nodiscard]] static auto create(jb::core::JsonValue const&   draft,
                                     std::filesystem::path const& parent = {},
                                     JobEditFileIo io = {}) -> jb::core::Result<JobEditFiles, JobEditFileFailure>;

    ~JobEditFiles();
    JobEditFiles(JobEditFiles const&)                    = delete;
    auto operator=(JobEditFiles const&) -> JobEditFiles& = delete;
    JobEditFiles(JobEditFiles&& other) noexcept;
    // Assignment could lose the only reportable path to retained work.
    auto operator=(JobEditFiles&&) -> JobEditFiles& = delete;

    [[nodiscard]] auto artifacts() const -> JobEditArtifacts;

    /// Accepts safe atomic-save replacement inodes. Reads at most 16 MiB, without following links or blocking on FIFOs.
    /// Metadata and I/O errors never modify the draft. JSON validation remains prepare_job_update()'s responsibility.
    [[nodiscard]] auto read_draft() const -> jb::core::Result<std::string, jb::core::Error>;

    /// Writes compact prepared minimal params within the request-file byte limit, exclusively and once.
    /// Never overwrites the edited draft. Serialization/size failures create no request file.
    /// Failed/partial request files are not advertised as submit-ready. Existing RPC/payload limits are unchanged.
    [[nodiscard]] auto write_request(PreparedJobUpdate const& update) -> jb::core::Result<void, jb::core::Error>;

    /// Removes known user-owned regular files and then the empty directory, using retained directory descriptors.
    /// Unexpected entries or replaced directory/request identities are retained and reported as cleanup failure.
    /// No recursive removal occurs. On failure artifacts() still supplies the directory location.
    [[nodiscard]] auto cleanup() -> jb::core::Result<void, jb::core::Error>;

private:
    JobEditFiles(std::filesystem::path directory, int parent, int directory_fd, JobEditFileIo io);
    auto write_file(char const* name, std::string const& text, bool request) -> jb::core::Result<void, jb::core::Error>;
    auto remove_file(char const* name, bool request) const -> bool;

    std::filesystem::path _directory;
    std::string           _leaf;
    int                   _parent{-1};
    int                   _directory_fd{-1};
    JobEditFileIo         _io;
    bool                  _draft_present{false};
    bool                  _request_present{false};
    bool                  _request_ready{false};
    dev_t                 _request_device{};
    ino_t                 _request_inode{};
};

} // namespace jb::jobuctl::detail
