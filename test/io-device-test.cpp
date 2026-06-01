#include "io_device.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <string_view>

using namespace jb::core;

namespace {

class TestDevice : public IODevice {
public:

    using IODevice::clear_error;
    using IODevice::emit_bytes_written;
    using IODevice::emit_ready_read;
    using IODevice::set_error;

    auto is_open() const -> bool override { return _open; }

    void close() override { _open = false; }

    auto read(std::size_t max_size) -> std::string override
    {
        auto const size = std::min(max_size, _data.size());
        auto       out  = _data.substr(0, size);
        _data.erase(0, size);
        return out;
    }

    auto read_all() -> std::string override
    {
        auto out = _data;
        _data.clear();
        return out;
    }

    auto read_line(std::size_t max_size = static_cast<std::size_t>(-1)) -> std::string override
    {
        auto const newline = _data.find('\n');
        auto const size    = std::min(newline == std::string::npos ? _data.size() : newline, max_size);
        auto       out     = _data.substr(0, size);
        _data.erase(0, newline == std::string::npos ? size : newline + 1);
        if (!out.empty() && out.back() == '\r') {
            out.pop_back();
        }
        return out;
    }

    auto can_read_line() const -> bool override { return !_data.empty(); }

    auto write(std::string_view data) -> std::size_t override
    {
        _data.append(data);
        emit_bytes_written(data.size());
        return data.size();
    }

    auto bytes_available() const -> std::size_t override { return _data.size(); }

    void open() { _open = true; }

private:
    bool        _open{false};
    std::string _data;
};

} // anonymous namespace

TEST_CASE("IODevice exposes initial error state", "[core][io]")
{
    TestDevice device;

    CHECK(device.error() == IOError::NoError);
    CHECK(device.error_string().empty());
}

TEST_CASE("IODevice stores and clears errors", "[core][io]")
{
    TestDevice device;

    device.set_error(IOError::ReadError, "read failed");
    CHECK(device.error() == IOError::ReadError);
    CHECK(device.error_string() == "read failed");

    device.clear_error();
    CHECK(device.error() == IOError::NoError);
    CHECK(device.error_string().empty());
}

TEST_CASE("IODevice emits errorOccurred for real errors", "[core][io]")
{
    TestDevice  device;
    int         count = 0;
    IOError     last_error{IOError::NoError};
    std::string last_message;

    device.errorOccurred.connect([&](IOError error, std::string message) -> void {
        ++count;
        last_error   = error;
        last_message = std::move(message);
    });

    device.set_error(IOError::WriteError, "write failed");

    CHECK(count == 1);
    CHECK(last_error == IOError::WriteError);
    CHECK(last_message == "write failed");
}

TEST_CASE("IODevice does not emit errorOccurred for NoError", "[core][io]")
{
    TestDevice device;
    int        count = 0;

    device.errorOccurred.connect([&](IOError, std::string) -> void { ++count; });

    device.set_error(IOError::NoError, "");

    CHECK(count == 0);
    CHECK(device.error() == IOError::NoError);
    CHECK(device.error_string().empty());
}

TEST_CASE("IODevice derived devices emit readyRead and bytesWritten", "[core][io]")
{
    TestDevice  device;
    int         ready_count   = 0;
    std::size_t written_bytes = 0;

    device.readyRead.connect([&]() -> void { ++ready_count; });
    device.bytesWritten.connect([&](std::size_t bytes) -> void { written_bytes += bytes; });

    device.emit_ready_read();
    CHECK(device.write("abc") == 3);

    CHECK(ready_count == 1);
    CHECK(written_bytes == 3);
}

TEST_CASE("IODevice concrete implementation can read buffered data", "[core][io]")
{
    TestDevice device;

    CHECK_FALSE(device.is_open());
    device.open();
    CHECK(device.is_open());

    REQUIRE(device.write("abcdef") == 6);
    CHECK(device.bytes_available() == 6);
    CHECK(device.read(2) == "ab");
    CHECK(device.bytes_available() == 4);
    CHECK(device.read_all() == "cdef");
    CHECK(device.bytes_available() == 0);

    device.close();
    CHECK_FALSE(device.is_open());
}

TEST_CASE("IODevice concrete implementation can expose line reads", "[core][io]")
{
    TestDevice device;

    REQUIRE(device.write("first\r\nsecond") == 13);

    CHECK(device.can_read_line());
    CHECK(device.read_line() == "first");
    CHECK(device.can_read_line());
    CHECK(device.read_line() == "second");
}
