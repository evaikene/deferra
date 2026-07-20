#include "memory_io_device.hpp"

#include <algorithm>
#include <utility>

namespace jb::test {

MemoryIODevice::MemoryIODevice(jb::core::Object* parent)
    : IODevice(parent)
{}

MemoryIODevice::~MemoryIODevice() = default;

void MemoryIODevice::open()
{
    if (_open) {
        return;
    }
    _open = true;
    clear_error();
}

void MemoryIODevice::inject_input(std::string_view bytes)
{
    _input.append(bytes);
    if (_open) {
        emit_ready_read();
    }
}

void MemoryIODevice::fail(jb::core::IOError error, std::string message)
{
    set_error(error, std::move(message));
}

auto MemoryIODevice::written_data() const noexcept -> std::string const&
{
    return _written;
}

auto MemoryIODevice::take_written_data() -> std::string
{
    return std::exchange(_written, {});
}

void MemoryIODevice::set_write_limit(std::optional<std::size_t> bytes)
{
    _write_limit = bytes;
}

void MemoryIODevice::set_auto_acknowledge_writes(bool enabled) noexcept
{
    _auto_acknowledge_writes = enabled;
}

void MemoryIODevice::acknowledge_writes(std::size_t bytes)
{
    auto const acknowledged = std::min(bytes, _unacknowledged_bytes);
    if (acknowledged == 0U) {
        return;
    }

    _unacknowledged_bytes -= acknowledged;
    emit_bytes_written(acknowledged);
}

auto MemoryIODevice::unacknowledged_bytes() const noexcept -> std::size_t
{
    return _unacknowledged_bytes;
}

auto MemoryIODevice::is_open() const -> bool
{
    return _open;
}

void MemoryIODevice::close()
{
    if (!_open) {
        return;
    }
    _open = false;
    emit_closed();
}

auto MemoryIODevice::read(std::size_t max_size) -> std::string
{
    auto const size  = std::min(max_size, _input.size());
    auto       bytes = _input.substr(0U, size);
    _input.erase(0U, size);
    return bytes;
}

auto MemoryIODevice::read_all() -> std::string
{
    return std::exchange(_input, {});
}

auto MemoryIODevice::read_line(std::size_t max_size) -> std::string
{
    auto const newline      = _input.find('\n');
    auto const content_size = newline == std::string::npos ? _input.size() : newline;
    auto const size         = std::min(max_size, content_size);
    auto       line         = _input.substr(0U, size);

    _input.erase(0U, size);
    if (size == content_size && newline != std::string::npos) {
        _input.erase(0U, 1U);
    }
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    return line;
}

auto MemoryIODevice::can_read_line() const -> bool
{
    return !_input.empty();
}

auto MemoryIODevice::write(std::string_view data) -> std::size_t
{
    if (!_open) {
        set_error(jb::core::IOError::NotOpen, "Memory device is not open");
        return 0U;
    }

    auto const accepted = std::min(data.size(), _write_limit.value_or(data.size()));
    _written.append(data.substr(0U, accepted));
    _unacknowledged_bytes += accepted;
    if (_auto_acknowledge_writes) {
        acknowledge_writes(accepted);
    }
    return accepted;
}

auto MemoryIODevice::bytes_available() const -> std::size_t
{
    return _input.size();
}

} // namespace jb::test
