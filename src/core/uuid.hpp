/** @file uuid.hpp
 * @brief Defines UUID values and an injectable, monotonic UUIDv7 generator.
 */
#pragma once

#include "error.hpp"
#include "result.hpp"
#include "time_source.hpp"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>

namespace jb::core {

/// Immutable 16-byte UUID value with parsing, formatting, ordering, and hashing support.
/// Use parse() for external text, to_string() for canonical output, and bytes() for binary adapters.
class Uuid {
public:
    /// Compact storage used by database and wire adapters.
    using Storage = std::array<std::byte, 16>;

    /// Creates the nil UUID whose bytes are all zero.
    constexpr Uuid() noexcept = default;

    /// Creates a UUID from complete binary storage.
    explicit constexpr Uuid(Storage bytes) noexcept
        : _bytes{bytes}
    {}

    /// Parses canonical UUID text and returns InvalidArgument for malformed input.
    [[nodiscard]] static auto parse(std::string_view text) -> Result<Uuid, Error>;
    /// Formats this UUID as lower-case canonical `8-4-4-4-12` text.
    [[nodiscard]] auto        to_string() const -> std::string;

    /// Returns the compact binary representation without transferring ownership.
    [[nodiscard]] constexpr auto bytes() const noexcept -> Storage const& { return _bytes; }

    /// Returns true when every stored byte is zero.
    [[nodiscard]] constexpr auto is_nil() const noexcept -> bool
    {
        for (auto const byte : _bytes) {
            if (byte != std::byte{0}) {
                return false;
            }
        }
        return true;
    }

    /// Compares UUIDs lexicographically by their binary representation.
    // clang-format off
    auto operator<=>(Uuid const&) const = default;
    // clang-format on

private:
    Storage _bytes{};
};

/// Interface for components that produce UUID values.
/// Consumers can substitute deterministic generators in tests without weakening production entropy.
class UuidGenerator {
public:
    /// Destroys a generator through its interface.
    virtual ~UuidGenerator() = default;

    /// Produces one UUID or an Error when generation cannot proceed.
    [[nodiscard]] virtual auto generate() -> Result<Uuid, Error> = 0;
};

/// Thread-safe UUIDv7 generator using operating-system cryptographic randomness.
/// It preserves increasing byte order for same-millisecond calls and wall-clock regressions.
class UuidV7Generator final : public UuidGenerator {
public:
    /// Creates a generator using `time_source`, which must outlive the generator.
    explicit UuidV7Generator(TimeSource& time_source);

    /// Generates a UUIDv7 with RFC 9562 version and variant bits.
    [[nodiscard]] auto generate() -> Result<Uuid, Error> override;

private:
    /// Increments the 74-bit random tail while preserving version and variant bits.
    /// @return True when incrementing succeeds, false when the tail is exhausted.
    static auto increment_random_tail(Uuid::Storage& bytes) noexcept -> bool;

    TimeSource&   _time_source;
    std::mutex    _mutex;
    Uuid::Storage _last_uuid{};
    std::uint64_t _last_timestamp_ms{0};
    bool          _has_last{false};
};

} // namespace jb::core

template <>
struct std::hash<jb::core::Uuid> {
    /// Returns a hash suitable for standard unordered containers.
    auto operator()(jb::core::Uuid const& value) const noexcept -> std::size_t;
};
