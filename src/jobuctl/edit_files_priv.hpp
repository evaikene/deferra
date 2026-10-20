#pragma once

#include "edit_resource_priv.hpp"
#include "error.hpp"
#include "json.hpp"
#include "result.hpp"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/types.h>

namespace jb::jobuctl::detail {

/// Synchronous file-only test seams; empty operations use POSIX. Captures must outlive the owner.
/// finish must close its descriptor even when reporting failure; close is never retried after EINTR.
struct EditFileIo {
    std::function<ssize_t(int, void*, std::size_t)>       read;
    std::function<ssize_t(int, void const*, std::size_t)> write;
    std::function<int(int)>                               finish;
    std::function<int(int, struct stat*)>                 inspect;
};

/// Owning locations for recovery. request is present only after a complete, closed minimal request was written.
/// Paths are data: callers must escape them for human display. No file contents are included.
struct EditArtifacts {
    std::filesystem::path                directory;
    std::filesystem::path                draft;
    std::optional<std::filesystem::path> request;
};

/// A creation failure may leave artifacts only when safe, known-file cleanup itself failed.
struct EditFileFailure {
    jb::core::Error       error;
    std::filesystem::path retained_directory;
};

/// Owns private directory handles, without an Application or Object lifetime.
/// Completed creation retains files on destruction, so an early return cannot discard an operator's edits.
/// Call cleanup() explicitly after success, no-op, or cancellation; failures can use artifacts() for recovery.
class EditFiles final {
public:
    /// Writes already formatted draft.jsonc bytes unchanged; rejects more than 16 MiB before any file I/O.
    /// Domain code supplies instructions/validation; an empty parent selects system temp.
    /// Creates a 0700 directory and exclusive 0600 draft, closing the draft before returning.
    [[nodiscard]] static auto
    create(EditResource resource, std::string_view draft, std::filesystem::path const& parent = {}, EditFileIo io = {})
        -> jb::core::Result<EditFiles, EditFileFailure>;

    ~EditFiles();
    EditFiles(EditFiles const&)                    = delete;
    auto operator=(EditFiles const&) -> EditFiles& = delete;
    EditFiles(EditFiles&& other) noexcept;
    // Assignment could lose the only reportable path to retained work.
    auto operator=(EditFiles&&) -> EditFiles& = delete;

    [[nodiscard]] auto artifacts() const -> EditArtifacts;

    /// Accepts safe atomic-save replacement inodes. Reads at most 16 MiB, without following links or blocking on FIFOs.
    /// Metadata and I/O errors never modify the draft. JSON/domain validation remains the coordinator's responsibility.
    [[nodiscard]] auto read_draft() const -> jb::core::Result<std::string, jb::core::Error>;

    /// Writes caller-validated minimal params as compact JSON within the request-file byte limit, exclusively and once.
    /// Never overwrites the edited draft. Serialization/size failures create no request file.
    /// Failed/partial request files are not advertised as submit-ready. Existing RPC/payload limits are unchanged.
    [[nodiscard]] auto write_request(jb::core::JsonValue const& params) -> jb::core::Result<void, jb::core::Error>;

    /// Removes known user-owned regular files and then the empty directory, using retained directory descriptors.
    /// Unexpected entries or replaced directory/request identities are retained and reported as cleanup failure.
    /// No recursive removal occurs. On failure artifacts() still supplies the directory location.
    [[nodiscard]] auto cleanup() -> jb::core::Result<void, jb::core::Error>;

private:
    EditFiles(EditResource resource, std::filesystem::path directory, int parent, int directory_fd, EditFileIo io);
    auto write_file(char const* name, std::string_view text, bool request) -> jb::core::Result<void, jb::core::Error>;
    auto remove_file(char const* name, bool request) const -> bool;

    EditResource          _resource;
    std::filesystem::path _directory;
    std::string           _leaf;
    int                   _parent{-1};
    int                   _directory_fd{-1};
    EditFileIo            _io;
    bool                  _draft_present{false};
    bool                  _request_present{false};
    bool                  _request_ready{false};
    dev_t                 _request_device{};
    ino_t                 _request_inode{};
};

} // namespace jb::jobuctl::detail
