#include "io_device.hpp"

#include <utility>

namespace jb::core {

IODevice::IODevice(Object* parent)
    : Object(parent)
{}

IODevice::~IODevice() = default;

auto IODevice::error() const noexcept -> IOError
{
    return _error;
}

auto IODevice::error_string() const noexcept -> std::string const&
{
    return _error_string;
}

void IODevice::clear_error()
{
    _error = IOError::NoError;
    _error_string.clear();
}

void IODevice::set_error(IOError error, std::string message)
{
    _error        = error;
    _error_string = std::move(message);

    if (_error != IOError::NoError) {
        emit(errorOccurred, _error, _error_string);
    }
}

void IODevice::emit_ready_read()
{
    emit(readyRead);
}

void IODevice::emit_bytes_written(std::size_t bytes)
{
    emit(bytesWritten, bytes);
}

} // namespace jb::core
