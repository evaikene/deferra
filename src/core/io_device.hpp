/**
 * @file io_device.hpp
 * @brief Defines the common lifecycle, error, and signal contract for byte-oriented devices.
 *
 * Concrete devices implement the byte operations and report activity through the shared
 * signals. In particular, `closed` lets transport-independent users observe the end of an
 * open device lifecycle without knowing its concrete type.
 */
#pragma once

#include "object.hpp"
#include "signal.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace jb::core {

/// Common I/O error category used by IODevice implementations.
enum class IOError : std::uint8_t {
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

namespace priv {
struct IODevicePrivate; // defined in io_device_priv.hpp
} // namespace priv

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
    ///
    /// Closing an open device emits closed after the concrete device has completed
    /// its shutdown. Closing an already closed device has no effect.
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
    Signal<> ready_read;

    /// Emitted after bytes have been accepted for writing.
    Signal<std::size_t> bytes_written;

    /// Emitted when an operation fails.
    Signal<IOError, std::string> error_occurred;

    /// Emitted once after an open device completes its transition to closed.
    ///
    /// Concrete devices make any final readable bytes available and emit ready_read
    /// before this signal. Destroying a device does not emit closed.
    Signal<> closed;

protected:

    /// Constructor for subclasses that supply their own private data.
    /// @param[in] dd  Reference to a heap-allocated struct that inherits (directly
    ///                or transitively) from priv::IODevicePrivate. IODevice takes ownership;
    ///                do NOT delete @p dd elsewhere.
    /// @param[in] parent Optional parent
    explicit IODevice(priv::IODevicePrivate& dd, Object* parent = nullptr);

    /// Clears the stored error state.
    void clear_error();

    /// Stores an error and emits errorOccurred unless @p error is NoError.
    void set_error(IOError error, std::string message);

    /// Emits ready_read.
    void emit_ready_read();

    /// Emits bytes_written.
    void emit_bytes_written(std::size_t bytes);

    /// Emits closed after a concrete device completes an open-to-closed transition.
    ///
    /// Derived classes are responsible for calling this exactly once per open lifecycle
    /// and must not call it from their destructors.
    void emit_closed();
};

} // namespace jb::core
