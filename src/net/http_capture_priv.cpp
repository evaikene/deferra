#include "http_capture_priv.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

namespace jb::net::detail {

namespace {

auto capture_error(std::string detail) -> jb::core::Error
{
    return {
        .category = jb::core::ErrorCategory::Internal,
        .code     = "net.http.internal",
        .message  = "HTTP capture failed",
        .detail   = std::move(detail),
    };
}

} // anonymous namespace

auto checked_capture_append_size(std::uint64_t current_total, std::size_t element_size, std::size_t element_count)
    -> jb::core::Result<std::size_t, jb::core::Error>
{
    using Result = jb::core::Result<std::size_t, jb::core::Error>;

    if (element_size != 0U && element_count > std::numeric_limits<std::size_t>::max() / element_size) {
        return Result::failure(capture_error("capture_size_overflow"));
    }
    auto const byte_count = element_size * element_count;
    if constexpr (std::numeric_limits<std::size_t>::digits > std::numeric_limits<std::uint64_t>::digits) {
        if (byte_count > static_cast<std::size_t>(std::numeric_limits<std::uint64_t>::max())) {
            return Result::failure(capture_error("capture_total_overflow"));
        }
    }
    if (byte_count > std::numeric_limits<std::uint64_t>::max() - current_total) {
        return Result::failure(capture_error("capture_total_overflow"));
    }
    return Result::success(byte_count);
}

CaptureBuffer::CaptureBuffer(std::size_t limit)
    : _limit(limit)
{}

auto CaptureBuffer::append(void const* data, std::size_t element_size, std::size_t element_count)
    -> jb::core::Result<std::size_t, jb::core::Error>
{
    using Result = jb::core::Result<std::size_t, jb::core::Error>;

    auto byte_count = checked_capture_append_size(_total_bytes, element_size, element_count);
    if (!byte_count) {
        return Result::failure(std::move(byte_count).error());
    }
    if (*byte_count == 0U) {
        return byte_count;
    }
    if (data == nullptr) {
        return Result::failure(capture_error("capture_null_data"));
    }

    auto bytes = jb::core::ByteView{static_cast<std::byte const*>(data), *byte_count};

    // Retention remains bounded, but counting and successful consumption continue after truncation so a caller can
    // keep draining the complete stream without turning an output limit into a transport failure.
    auto const prefix_limit = (_limit / 2U) + (_limit % 2U);
    auto const prefix_space = prefix_limit - _prefix.size();
    auto const prefix_count = std::min(prefix_space, bytes.size());
    _prefix.insert(_prefix.end(), bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(prefix_count));
    bytes = bytes.subspan(prefix_count);
    append_suffix(bytes);
    _total_bytes += static_cast<std::uint64_t>(*byte_count);
    return byte_count;
}

void CaptureBuffer::append_suffix(jb::core::ByteView bytes)
{
    auto const suffix_limit = _limit / 2U;
    if (suffix_limit == 0U || bytes.empty()) {
        return;
    }

    auto const available = suffix_limit - _suffix.size();
    auto const initial   = std::min(available, bytes.size());
    _suffix.insert(_suffix.end(), bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(initial));
    bytes = bytes.subspan(initial);
    if (bytes.empty()) {
        return;
    }

    if (bytes.size() >= suffix_limit) {
        _suffix.assign(bytes.end() - static_cast<std::ptrdiff_t>(suffix_limit), bytes.end());
        _suffix_start = 0;
        return;
    }

    auto const first = std::min(bytes.size(), suffix_limit - _suffix_start);
    std::copy_n(bytes.begin(),
                static_cast<std::ptrdiff_t>(first),
                _suffix.begin() + static_cast<std::ptrdiff_t>(_suffix_start));
    std::copy(bytes.begin() + static_cast<std::ptrdiff_t>(first), bytes.end(), _suffix.begin());
    _suffix_start = (_suffix_start + bytes.size()) % suffix_limit;
}

auto CaptureBuffer::take() -> HttpCapturedData
{
    auto retained = std::move(_prefix);
    retained.reserve(retained.size() + _suffix.size());
    if (_suffix.size() < _limit / 2U || _suffix_start == 0U) {
        retained.insert(retained.end(), _suffix.begin(), _suffix.end());
    }
    else {
        retained.insert(retained.end(), _suffix.begin() + static_cast<std::ptrdiff_t>(_suffix_start), _suffix.end());
        retained.insert(retained.end(), _suffix.begin(), _suffix.begin() + static_cast<std::ptrdiff_t>(_suffix_start));
    }

    auto const retained_size = retained.size();
    auto       result        = HttpCapturedData{
        .bytes       = std::move(retained),
        .total_bytes = _total_bytes,
        .truncated   = _total_bytes > retained_size,
    };
    _prefix.clear();
    _suffix.clear();
    _suffix_start = 0;
    _total_bytes  = 0;
    return result;
}

} // namespace jb::net::detail
