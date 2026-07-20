#pragma once

#include "io_device.hpp"

#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace jb::test {

class MemoryIODevice final : public jb::core::IODevice {
public:
    explicit MemoryIODevice(jb::core::Object* parent = nullptr);
    ~MemoryIODevice() override;

    void open();
    void inject_input(std::string_view bytes);
    void fail(jb::core::IOError error, std::string message);

    [[nodiscard]] auto written_data() const noexcept -> std::string const&;
    [[nodiscard]] auto take_written_data() -> std::string;

    void               set_write_limit(std::optional<std::size_t> bytes);
    void               set_auto_acknowledge_writes(bool enabled) noexcept;
    void               acknowledge_writes(std::size_t bytes = std::numeric_limits<std::size_t>::max());
    [[nodiscard]] auto unacknowledged_bytes() const noexcept -> std::size_t;

    [[nodiscard]] auto is_open() const -> bool override;
    void               close() override;
    [[nodiscard]] auto read(std::size_t max_size) -> std::string override;
    [[nodiscard]] auto read_all() -> std::string override;
    [[nodiscard]] auto read_line(std::size_t max_size = std::numeric_limits<std::size_t>::max())
        -> std::string override;
    [[nodiscard]] auto can_read_line() const -> bool override;
    auto               write(std::string_view data) -> std::size_t override;
    [[nodiscard]] auto bytes_available() const -> std::size_t override;

private:
    bool                       _open{false};
    bool                       _auto_acknowledge_writes{true};
    std::optional<std::size_t> _write_limit;
    std::size_t                _unacknowledged_bytes{0};
    std::string                _input;
    std::string                _written;
};

} // namespace jb::test
