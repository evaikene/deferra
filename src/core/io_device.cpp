#include "io_device.hpp"

#include "io_device_priv.hpp"

#include <utility>

namespace jb::core {

IODevice::IODevice(Object* parent)
    : Object(*new priv::IODevicePrivate, parent)
{}

IODevice::IODevice(priv::IODevicePrivate& dd, Object* parent)
    : Object(dd, parent)
{}

IODevice::~IODevice() = default;

auto IODevice::error() const noexcept -> IOError
{
    return d_ptr<priv::IODevicePrivate const>()->error;
}

auto IODevice::error_string() const noexcept -> std::string const&
{
    return d_ptr<priv::IODevicePrivate const>()->error_string;
}

void IODevice::clear_error()
{
    auto* d  = d_ptr<priv::IODevicePrivate>();
    d->error = IOError::NoError;
    d->error_string.clear();
}

void IODevice::set_error(IOError error, std::string message)
{
    auto* d         = d_ptr<priv::IODevicePrivate>();
    d->error        = error;
    d->error_string = std::move(message);

    if (d->error != IOError::NoError) {
        emit(error_occurred, d->error, d->error_string);
    }
}

void IODevice::emit_ready_read()
{
    emit(ready_read);
}

void IODevice::emit_bytes_written(std::size_t bytes)
{
    emit(bytes_written, bytes);
}

} // namespace jb::core
