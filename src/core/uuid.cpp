#include "uuid.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>

#if defined(__linux__)
#  include <sys/random.h>
#elif defined(__APPLE__)
#  include <stdlib.h>
#endif

namespace jb::core {

namespace {

constexpr std::uint64_t kMaxTimestampMs{0xffffffffffffULL};

auto invalid_uuid_error() -> Error
{
    return {
        .category = ErrorCategory::InvalidArgument,
        .code     = "core.uuid.invalid_format",
        .message  = "UUID must use canonical 8-4-4-4-12 hexadecimal format",
    };
}

auto hex_value(char character) noexcept -> int
{
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F') {
        return character - 'A' + 10;
    }
    return -1;
}

auto fill_random(std::span<std::byte> bytes) -> Result<void, Error>
{
#if defined(__linux__)
    auto* data      = reinterpret_cast<unsigned char*>(bytes.data());
    auto  remaining = bytes.size();
    while (remaining != 0) {
        auto const count = ::getrandom(data, remaining, 0);
        if (count > 0) {
            data      += count;
            remaining -= static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        return Result<void, Error>::failure({
            .category = ErrorCategory::Unavailable,
            .code     = "core.uuid.random_unavailable",
            .message  = "Operating-system random source is unavailable",
            .detail   = std::strerror(errno),
        });
    }
    return Result<void, Error>::success();
#elif defined(__APPLE__)
    ::arc4random_buf(bytes.data(), bytes.size());
    return Result<void, Error>::success();
#else
    static_cast<void>(bytes);
    return Result<void, Error>::failure({
        .category = ErrorCategory::Unsupported,
        .code     = "core.uuid.random_unsupported",
        .message  = "No operating-system random source is available on this platform",
    });
#endif
}

auto timestamp_ms(UtcTimePoint value) -> Result<std::uint64_t, Error>
{
    auto const milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(value.time_since_epoch()).count();
    if (milliseconds < 0 || static_cast<std::uint64_t>(milliseconds) > kMaxTimestampMs) {
        return Result<std::uint64_t, Error>::failure({
            .category = ErrorCategory::InvalidArgument,
            .code     = "core.uuid.timestamp_out_of_range",
            .message  = "UTC time cannot be represented by UUIDv7",
        });
    }
    return Result<std::uint64_t, Error>::success(static_cast<std::uint64_t>(milliseconds));
}

void set_timestamp(Uuid::Storage& bytes, std::uint64_t value) noexcept
{
    for (auto index = 0U; index < 6; ++index) {
        auto const shift = (5U - index) * 8U;
        bytes[index]     = static_cast<std::byte>((value >> shift) & 0xffU);
    }
}

} // anonymous namespace

auto Uuid::parse(std::string_view text) -> Result<Uuid, Error>
{
    if (text.size() != 36 || text[8] != '-' || text[13] != '-' || text[18] != '-' || text[23] != '-') {
        return Result<Uuid, Error>::failure(invalid_uuid_error());
    }

    Storage     bytes{};
    std::size_t input_index{0};
    for (std::size_t byte_index = 0; byte_index < bytes.size(); ++byte_index) {
        if (input_index == 8 || input_index == 13 || input_index == 18 || input_index == 23) {
            ++input_index;
        }
        auto const high = hex_value(text[input_index++]);
        auto const low  = hex_value(text[input_index++]);
        if (high < 0 || low < 0) {
            return Result<Uuid, Error>::failure(invalid_uuid_error());
        }
        bytes[byte_index] = static_cast<std::byte>((high << 4) | low);
    }
    return Result<Uuid, Error>::success(Uuid{bytes});
}

auto Uuid::to_string() const -> std::string
{
    constexpr std::string_view hex{"0123456789abcdef"};

    std::string result;
    result.reserve(36);
    for (std::size_t index = 0; index < _bytes.size(); ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10) {
            result.push_back('-');
        }
        auto const value = std::to_integer<unsigned char>(_bytes[index]);
        result.push_back(hex[value >> 4U]);
        result.push_back(hex[value & 0x0fU]);
    }
    return result;
}

UuidV7Generator::UuidV7Generator(TimeSource& time_source)
    : _time_source{time_source}
{}

auto UuidV7Generator::generate() -> Result<Uuid, Error>
{
    std::scoped_lock const lock{_mutex};
    auto const             current_timestamp = timestamp_ms(_time_source.utc_now());
    if (!current_timestamp) {
        return Result<Uuid, Error>::failure(current_timestamp.error());
    }

    if (!_has_last || *current_timestamp > _last_timestamp_ms) {
        auto       next          = _last_uuid;
        auto const random_result = fill_random(next);
        if (!random_result) {
            return Result<Uuid, Error>::failure(random_result.error());
        }

        set_timestamp(next, *current_timestamp);
        next[6]            = (next[6] & std::byte{0x0f}) | std::byte{0x70};
        next[8]            = (next[8] & std::byte{0x3f}) | std::byte{0x80};
        _last_uuid         = next;
        _last_timestamp_ms = *current_timestamp;
        _has_last          = true;
        return Result<Uuid, Error>::success(Uuid{next});
    }

    auto next = _last_uuid;
    if (!increment_random_tail(next)) {
        return Result<Uuid, Error>::failure({
            .category = ErrorCategory::ResourceExhausted,
            .code     = "core.uuid.sequence_exhausted",
            .message  = "UUIDv7 sequence space is exhausted for the current timestamp",
        });
    }
    _last_uuid = next;
    return Result<Uuid, Error>::success(Uuid{next});
}

auto UuidV7Generator::increment_random_tail(Uuid::Storage& bytes) noexcept -> bool
{
    for (std::size_t index = bytes.size(); index-- > 9;) {
        auto value = std::to_integer<unsigned char>(bytes[index]);
        if (value != std::numeric_limits<unsigned char>::max()) {
            bytes[index] = static_cast<std::byte>(value + 1U);
            return true;
        }
        bytes[index] = std::byte{0};
    }

    auto value = std::to_integer<unsigned char>(bytes[8]) & 0x3fU;
    if (value != 0x3fU) {
        bytes[8] = static_cast<std::byte>(static_cast<unsigned char>(0x80U | (value + 1U)));
        return true;
    }
    bytes[8] = std::byte{0x80};

    value = std::to_integer<unsigned char>(bytes[7]);
    if (value != std::numeric_limits<unsigned char>::max()) {
        bytes[7] = static_cast<std::byte>(value + 1U);
        return true;
    }
    bytes[7] = std::byte{0};

    value = std::to_integer<unsigned char>(bytes[6]) & 0x0fU;
    if (value != 0x0fU) {
        bytes[6] = static_cast<std::byte>(static_cast<unsigned char>(0x70U | (value + 1U)));
        return true;
    }
    bytes[6] = std::byte{0x70};
    return false;
}

} // namespace jb::core

auto std::hash<jb::core::Uuid>::operator()(jb::core::Uuid const& value) const noexcept -> std::size_t
{
    std::size_t hash{static_cast<std::size_t>(1469598103934665603ULL)};
    for (auto const byte : value.bytes()) {
        hash ^= std::to_integer<unsigned char>(byte);
        hash *= 1099511628211ULL;
    }
    return hash;
}
