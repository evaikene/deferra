#pragma once

#include "attempt_executor.hpp"
#include "byte_buffer.hpp"
#include "error.hpp"
#include "result.hpp"

#include <cstddef>
#include <cstdint>

namespace jb::jobu::detail {

enum class CliCaptureMode : std::uint8_t {
    None,
    OnError,
    Always,
};

struct CliCaptureSnapshot {
    AttemptOutputChannel standard_output;
    AttemptOutputChannel standard_error;
    bool                 capture_lost{false};
};

[[nodiscard]] auto checked_cli_capture_total(std::uint64_t current_total, std::size_t appended_bytes)
    -> jb::core::Result<std::uint64_t, jb::core::Error>;

class CliCaptureBuffer final {
public:
    explicit CliCaptureBuffer(std::size_t limit);

    [[nodiscard]] auto append(jb::core::ByteView bytes) -> jb::core::Result<void, jb::core::Error>;
    [[nodiscard]] auto take() -> AttemptOutputChannel;

    /// Returns retained payload bytes without assembling a snapshot.
    [[nodiscard]] auto retained_size() const noexcept -> std::size_t { return _prefix.size() + _suffix.size(); }

private:
    void append_suffix(jb::core::ByteView bytes);

    std::size_t          _limit{0};
    jb::core::ByteBuffer _prefix;
    jb::core::ByteBuffer _suffix;
    std::size_t          _suffix_start{0};
    std::uint64_t        _total_bytes{0};
};

} // namespace jb::jobu::detail
