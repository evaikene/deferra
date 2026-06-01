#pragma once

#include "enum_bitmask.hpp"
#include "io_device.hpp"

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

namespace jb::core {

/// File open mode flags.
enum class OpenMode {
    ReadOnly  = 1u << 0u,                ///< Open for reading
    WriteOnly = 1u << 1u,                ///< Open for writing
    ReadWrite = (1u << 0u) | (1u << 1u), ///< Open for reading and writing
    Append    = 1u << 2u,                ///< Writes append to the end of the file
    Truncate  = 1u << 3u,                ///< Truncate the file while opening
    Create    = 1u << 4u,                ///< Create the file if it does not exist
    Text      = 1u << 5u,                ///< Open in text mode instead of binary mode
};

/// Type-safe set of file open mode flags.
using OpenModes = enum_bitmask<OpenMode>;

/// Synchronous byte-oriented file device.
///
/// File performs blocking filesystem I/O and does not emit readyRead for
/// ordinary file data. It emits bytesWritten after successful writes and
/// errorOccurred when operations fail.
class File : public IODevice {
public:

    /// Constructs a closed file device.
    /// @param[in] parent Optional parent that owns this file
    explicit File(Object* parent = nullptr);

    /// Closes the file if it is open.
    ~File() override;

    /// Opens @p path with @p modes.
    ///
    /// At least one of ReadOnly, WriteOnly, or ReadWrite must be set. Existing
    /// files are preserved unless Truncate is set. Missing files are created
    /// only when Create is set.
    auto open(std::filesystem::path path, OpenModes modes) -> bool;

    /// Returns true when the file is open.
    [[nodiscard]] auto is_open() const -> bool override;

    /// Closes the file. Errors during close are reported through error().
    void close() override;

    /// Reads up to @p max_size bytes from the current position.
    [[nodiscard]] auto read(std::size_t max_size) -> std::string override;

    /// Reads all bytes from the current position to the end of the file.
    [[nodiscard]] auto read_all() -> std::string override;

    /// Reads one text line without a trailing LF or CRLF delimiter.
    [[nodiscard]] auto read_line(std::size_t max_size = static_cast<std::size_t>(-1)) -> std::string override;

    /// Returns true when a line or final partial line remains to be read.
    [[nodiscard]] auto can_read_line() const -> bool override;

    /// Writes bytes at the current position, or at the end when Append is set.
    /// @return Number of bytes written
    auto write(std::string_view data) -> std::size_t override;

    /// Returns the number of bytes remaining from the current read position.
    [[nodiscard]] auto bytes_available() const -> std::size_t override;

    /// Returns the open file path, or an empty path when no path has been opened.
    [[nodiscard]] auto path() const -> std::filesystem::path const&;

    /// Returns the file size in bytes.
    [[nodiscard]] auto size() const -> std::size_t;

    /// Returns the current file position.
    [[nodiscard]] auto position() const -> std::size_t;

    /// Moves the current file position.
    auto seek(std::size_t offset) -> bool;

    /// Returns true when the current read position is at or beyond the end.
    [[nodiscard]] auto at_end() const -> bool;

private:
    struct Private;

    [[nodiscard]] auto can_read() const -> bool;
    [[nodiscard]] auto can_write() const -> bool;
    [[nodiscard]] auto has_mode(OpenMode mode) const -> bool;
    auto               fail(IOError error, std::string message) -> bool;

    Private* _d_file;
};

} // namespace jb::core
