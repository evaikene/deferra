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
    ~TemporaryDirectory();

    TemporaryDirectory(TemporaryDirectory const&)                    = delete;
    auto operator=(TemporaryDirectory const&) -> TemporaryDirectory& = delete;
    TemporaryDirectory(TemporaryDirectory&& other) noexcept;
    auto operator=(TemporaryDirectory&& other) noexcept -> TemporaryDirectory&;

    [[nodiscard]] auto path() const noexcept -> std::filesystem::path const& { return _path; }

    [[nodiscard]] auto release() noexcept -> std::filesystem::path;
    [[nodiscard]] auto cleanup() noexcept -> std::error_code;

private:
    std::filesystem::path _path;
};

} // namespace jb::test
  /// Performs best-effort cleanup and never throws.
  /// Prevents accidental shared ownership.
  /// Prevents accidental shared ownership.
  /// Transfers cleanup ownership from another helper.
  /// Cleans up this helper, then transfers cleanup ownership from another helper.
  /// Returns the directory path without transferring cleanup ownership.
  /// Transfers the path to the caller and disables this helper's cleanup.
  /// Removes this exact directory tree and returns any filesystem error.
