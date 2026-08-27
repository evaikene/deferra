#pragma once

#include "byte_buffer.hpp"
#include "error.hpp"
#include "http_client.hpp"
#include "result.hpp"

#include <cstddef>
#include <cstdint>

namespace jb::net::detail {

/// Checks a C-style element count and the resulting cumulative capture count without reading data.
[[nodiscard]] auto
checked_capture_append_size(std::uint64_t current_total, std::size_t element_size, std::size_t element_count)
    -> jb::core::Result<std::size_t, jb::core::Error>;

/// Retains a bounded first/last view while counting every appended byte.
class CaptureBuffer {
public:
    /// Creates an empty capture with an inclusive retained-byte limit.
    explicit CaptureBuffer(std::size_t limit);

    /// Counts @p element_size * @p element_count bytes and retains the configured first/last portions.
    /// @return The complete appended byte count, or a safe internal error without changing capture state.
    [[nodiscard]] auto append(void const* data, std::size_t element_size, std::size_t element_count)
        -> jb::core::Result<std::size_t, jb::core::Error>;

    /// Moves the current capture into an owning value and resets this buffer for reuse with the same limit.
    [[nodiscard]] auto take() -> HttpCapturedData;

private:
    void append_suffix(jb::core::ByteView bytes);

    std::size_t          _limit{0};
    jb::core::ByteBuffer _prefix;
    jb::core::ByteBuffer _suffix;
    std::size_t          _suffix_start{0};
    std::uint64_t        _total_bytes{0};
};

} // namespace jb::net::detail
