/**
 * @file enum_bitmask.hpp
 * @brief Provides a type-safe bitmask wrapper for strongly typed enum flags.
 *
 * Define an enum whose values are individual powers of two, then create an
 * `enum_bitmask` specialization for the enum:
 *
 * \code{.cpp}
 * enum class Permission : unsigned {
 *     Read  = 1U << 0U,
 *     Write = 1U << 1U,
 *     Exec  = 1U << 2U,
 * };
 * using Permissions = jb::core::enum_bitmask<Permission>;
 * \endcode
 *
 * Construct a mask from one or more flags, inspect it with `test()` or
 * `test_any()`, and modify it with `set()` and `clear()`:
 *
 * \code{.cpp}
 * Permissions permissions{Permission::Read, Permission::Write};
 * if (permissions.test(Permission::Read)) {
 *     // The read flag is set.
 * }
 *
 * permissions.set(Permission::Exec);
 * permissions.clear(Permission::Write);
 * const auto executable = permissions.test_any(Permission::Exec);
 * \endcode
 *
 * Masks can also be combined with the bitwise operators and inspected as
 * their underlying unsigned integer type:
 *
 * \code{.cpp}
 * constexpr auto readable = Permissions{Permission::Read};
 * constexpr auto writable = Permissions{Permission::Write};
 * constexpr auto read_write = readable | writable;
 * static_assert(read_write.test(Permissions{Permission::Read, Permission::Write}));
 *
 * const auto raw_bits = read_write.bits();
 * \endcode
 */
#pragma once

#include <initializer_list>
#include <type_traits>

namespace jb::core {

/// Type-safe bitmask wrapper for strongly-typed enums.
template <typename E> requires std::is_enum_v<E>
class enum_bitmask {
    using U = std::make_unsigned_t<std::underlying_type_t<E>>;

public:

    using enum_type  = E; ///< The enum type wrapped by this bitmask
    using value_type = U; ///< The underlying integral type used to store the bitmask

    /// @name Constructors
    /// @{

    constexpr enum_bitmask() noexcept = default;

    constexpr enum_bitmask(E f) noexcept
        : _value{static_cast<U>(f)}
    {}

    constexpr explicit enum_bitmask(U bits) noexcept
        : _value{bits}
    {}

    constexpr enum_bitmask(std::initializer_list<E> init) noexcept
    {
        for (auto f : init) {
            _value |= static_cast<U>(f);
        }
    }

    /// @}

    /// @name Modifiers
    /// @{

    /// Reset the bitmask to an empty state (i.e., no flags set)
    constexpr void reset() noexcept { _value = 0; }

    /// Set individual flags
    constexpr void set(E f) noexcept { _value |= static_cast<U>(f); }

    constexpr void set(enum_bitmask flags) noexcept { _value |= flags._value; }

    /// Clear individual flags
    constexpr void clear(E f) noexcept { _value &= ~static_cast<U>(f); }

    constexpr void clear(enum_bitmask flags) noexcept { _value &= ~flags._value; }

    /// @}

    /// @name Observers
    /// @{

    /// Test if all the specified flags are set
    /// @return true if all the specified flags are set, false otherwise
    [[nodiscard]] constexpr auto test(E f) const noexcept -> bool
    {
        return (_value & static_cast<U>(f)) == static_cast<U>(f);
    }

    [[nodiscard]] constexpr auto test(enum_bitmask f) const noexcept -> bool { return (_value & f._value) == f._value; }

    /// Test if any of the specified flags are set
    /// @return true if any of the specified flags are set, false otherwise
    [[nodiscard]] constexpr auto test_any(E f) const noexcept -> bool { return (_value & static_cast<U>(f)) != 0; }

    [[nodiscard]] constexpr auto test_any(enum_bitmask f) const noexcept -> bool { return (_value & f._value) != 0; }

    /// Test if no flags are set
    /// @return true if no flags are set, false otherwise
    [[nodiscard]] constexpr auto none() const noexcept -> bool { return _value == 0; }

    /// Test if any flags are set
    /// @return true if any flags are set, false otherwise
    [[nodiscard]] constexpr auto any() const noexcept -> bool { return _value != 0; }

    [[nodiscard]] constexpr explicit operator bool() const noexcept { return any(); }

    /// Returns the underlying bitmask value as an unsigned integer
    [[nodiscard]] constexpr auto bits() const noexcept -> U { return _value; }

    [[nodiscard]] constexpr explicit operator U() const noexcept { return bits(); }

    /// @}

    /// @name Bitwise operators
    /// @{

    constexpr auto operator|(enum_bitmask f) const noexcept -> enum_bitmask
    {
        return enum_bitmask{_value | f._value};
    }

    constexpr auto operator&(enum_bitmask f) const noexcept -> enum_bitmask
    {
        return enum_bitmask{_value & f._value};
    }

    constexpr auto operator^(enum_bitmask f) const noexcept -> enum_bitmask
    {
        return enum_bitmask{_value ^ f._value};
    }

    constexpr auto operator~() const noexcept -> enum_bitmask
    {
        return enum_bitmask{~_value};
    }

    constexpr auto operator|=(enum_bitmask f) noexcept -> enum_bitmask&
    {
        _value |= f._value;
        return *this;
    }

    constexpr auto operator&=(enum_bitmask f) noexcept -> enum_bitmask&
    {
        _value &= f._value;
        return *this;
    }

    constexpr auto operator^=(enum_bitmask f) noexcept -> enum_bitmask&
    {
        _value ^= f._value;
        return *this;
    }

    /// @}

private:
    U _value{0};
};

} // namespace jb::core
