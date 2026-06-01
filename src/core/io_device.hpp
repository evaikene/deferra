#pragma once

#include "object.hpp"
#include "signal.hpp"

#include <cstddef>
#include <string>
#include <string_view>

namespace jb::core {

/// Common I/O error category used by IODevice implementations.
enum class IOError {
    NoError,         ///< No error has occurred
    NotOpen,         ///< Operation requires an open device
    OpenError,       ///< Opening the device failed
    ReadError,       ///< Reading from the device failed
    WriteError,      ///< Writing to the device failed
    CloseError,      ///< Closing the device failed
    SeekError,       ///< Seeking within the device failed
    InvalidArgument, ///< The operation received an invalid argument
    Unsupported,     ///< The operation is not supported by this device
    ResourceError,   ///< The operating system reported a resource error
};

/// Base class for byte-oriented I/O devices.
///
/// IODevice provides the shared signal and error contract for concrete
/// devices such as File and TcpSocket. The class is object-thread-only like
/// other Object-derived event-loop types; callers should use EventLoop::post()
/// or queued signal connections for cross-thread interaction.
class IODevice : public Object {
public:

    /// Constructs an I/O device.
    /// @param[in] parent Optional parent that owns this device
    explicit IODevice(Object* parent = nullptr);

    /// Destructor.
    ~IODevice() override;

    /// Returns true when the concrete device is open.
    [[nodiscard]] virtual auto is_open() const -> bool = 0;

    /// Closes the device.
    virtual void close() = 0;

    /// Reads up to @p max_size bytes from the device.
    [[nodiscard]] virtual auto read(std::size_t max_size) -> std::string = 0;

    /// Reads all currently available bytes from the device.
    [[nodiscard]] virtual auto read_all() -> std::string = 0;

    /// Reads one text line without the trailing newline.
    ///
    /// A trailing `\n` is not included in the returned string. For CRLF input,
    /// the preceding `\r` is also omitted. Empty lines are returned as an empty
    /// string; use can_read_line() to distinguish an available empty line from
    /// no available line.
    [[nodiscard]] virtual auto read_line(std::size_t max_size = static_cast<std::size_t>(-1)) -> std::string = 0;

    /// Returns true when read_line() can return a line without waiting.
    [[nodiscard]] virtual auto can_read_line() const -> bool = 0;

    /// Writes bytes to the device.
    /// @return Number of bytes accepted for writing
    virtual auto write(std::string_view data) -> std::size_t = 0;

    /// Returns the number of bytes that can be read without waiting.
    [[nodiscard]] virtual auto bytes_available() const -> std::size_t = 0;

    /// Returns the last error category.
    [[nodiscard]] auto error() const noexcept -> IOError;

    /// Returns the last error message.
    [[nodiscard]] auto error_string() const noexcept -> std::string const&;

    /// Emitted when new bytes are available to read.
    Signal<> readyRead;

    /// Emitted after bytes have been accepted for writing.
    Signal<std::size_t> bytesWritten;

    /// Emitted when an operation fails.
    Signal<IOError, std::string> errorOccurred;

protected:

    /// Clears the stored error state.
    void clear_error();

    /// Stores an error and emits errorOccurred unless @p error is NoError.
    void set_error(IOError error, std::string message);

    /// Emits readyRead.
    void emit_ready_read();

    /// Emits bytesWritten.
    void emit_bytes_written(std::size_t bytes);

private:
    IOError     _error{IOError::NoError};
    std::string _error_string;
};

} // namespace jb::core
