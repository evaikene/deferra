#pragma once

#include <filesystem>
#include <system_error>

namespace jb::test {

/// Secures only fixture-created SQLite artifacts before handing preseeded state to the real daemon.
/// Call after closing the fixture database. Production deliberately refuses to repair existing files.
[[nodiscard]] inline auto protect_daemon_state(std::filesystem::path const& database) -> std::error_code
{
    std::error_code error;
    std::filesystem::permissions(database.parent_path(), std::filesystem::perms::owner_all, error);
    if (error) {
        return error;
    }

    for (auto const* suffix : {"", ".lock", "-wal", "-shm", "-journal"}) {
        auto const path = std::filesystem::path{database.string() + suffix};
        if (!std::filesystem::exists(path, error)) {
            if (error) {
                return error;
            }
            continue;
        }
        std::filesystem::permissions(path,
                                     std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                     error);
        if (error) {
            return error;
        }
    }
    return {};
}

} // namespace jb::test
