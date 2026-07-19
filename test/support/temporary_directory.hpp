/** @file temporary_directory.hpp
 * @brief Defines a move-only RAII directory helper for tests.
 */
#pragma once

#include <filesystem>
#include <system_error>

namespace jb::test {

/// Creates a unique directory below the system temporary directory.
/// It removes only its own tree at destruction; use cleanup() to assert cleanup or release() to retain it.
class TemporaryDirectory {
public:
    /// Creates a unique `jobu-test-` directory or throws when setup cannot continue.
    TemporaryDirectory();
    /// Performs best-effort cleanup and never throws.
    ~TemporaryDirectory();

    /// Prevents accidental shared ownership.
    TemporaryDirectory(TemporaryDirectory const&)                    = delete;
    /// Prevents accidental shared ownership.
    auto operator=(TemporaryDirectory const&) -> TemporaryDirectory& = delete;
    /// Transfers cleanup ownership from another helper.
    TemporaryDirectory(TemporaryDirectory&& other) noexcept;
    /// Cleans up this helper, then transfers cleanup ownership from another helper.
    auto operator=(TemporaryDirectory&& other) noexcept -> TemporaryDirectory&;

    /// Returns the directory path without transferring cleanup ownership.
    [[nodiscard]] auto path() const noexcept -> std::filesystem::path const& { return _path; }

    /// Transfers the path to the caller and disables this helper's cleanup.
    [[nodiscard]] auto release() noexcept -> std::filesystem::path;
    /// Removes this exact directory tree and returns any filesystem error.
    [[nodiscard]] auto cleanup() noexcept -> std::error_code;

private:
    std::filesystem::path _path;
};

} // namespace jb::test
