/** @file result.hpp
 * @brief Defines an explicit value-or-error result type for operational failures.
 */
#pragma once

#include <memory>
#include <stdexcept>
#include <utility>
#include <variant>

namespace jb::core {

/// Marker used internally for a default-constructed result with no outcome.
struct UninitializedResult {
    /// Compares two marker values.
    auto operator==(UninitializedResult const&) const -> bool = default;
};

/// Stores a successful value, represented error, or no outcome yet.
/// Construct with success() or failure(); accessing an absent alternative throws std::logic_error.
/// @tparam T Value type. @tparam E Error type.
template <typename T, typename E>
class [[nodiscard]] Result {
public:
    /// Creates an explicitly uninitialized result.
    Result() noexcept = default;

    /// Creates a successful result by taking ownership of value.
    static auto success(T value) -> Result { return Result{std::in_place_index<1>, std::move(value)}; }

    /// Creates a failed result by taking ownership of error.
    static auto failure(E error) -> Result { return Result{std::in_place_index<2>, std::move(error)}; }

    /// Returns true when the result has a value or an error.
    [[nodiscard]] auto is_initialized() const noexcept -> bool { return _data.index() != 0; }

    /// Returns true only when the result contains a value.
    [[nodiscard]] auto has_value() const noexcept -> bool { return _data.index() == 1; }

    /// Returns true only when the result contains a value.
    explicit operator bool() const noexcept { return has_value(); }

    /// Returns the stored value.
    /// @throws std::logic_error when the result is not successful.
    auto value() & -> T&
    {
        ensure_value();
        return std::get<1>(_data);
    }

    /// Returns the stored value without permitting mutation.
    /// @throws std::logic_error when the result is not successful.
    auto value() const& -> T const&
    {
        ensure_value();
        return std::get<1>(_data);
    }

    /// Moves the stored value out of this result.
    /// @throws std::logic_error when the result is not successful.
    auto value() && -> T&&
    {
        ensure_value();
        return std::move(std::get<1>(_data));
    }

    /// Returns the represented error.
    /// @throws std::logic_error when no error is present.
    auto error() & -> E&
    {
        ensure_error();
        return std::get<2>(_data);
    }

    /// Returns the represented error without permitting mutation.
    /// @throws std::logic_error when no error is present.
    auto error() const& -> E const&
    {
        ensure_error();
        return std::get<2>(_data);
    }

    /// Moves the represented error out of this result.
    /// @throws std::logic_error when no error is present.
    auto error() && -> E&&
    {
        ensure_error();
        return std::move(std::get<2>(_data));
    }

    /// Returns the stored value with dereference syntax.
    auto operator*() & -> T& { return value(); }

    /// Returns the stored value without permitting mutation.
    auto operator*() const& -> T const& { return value(); }

    /// Returns a pointer to the stored value.
    auto operator->() -> T* { return std::addressof(value()); }

    /// Returns a pointer to the stored value without permitting mutation.
    auto operator->() const -> T const* { return std::addressof(value()); }

    /// Returns the stored value, or a converted fallback when no value is present.
    template <typename U>
    auto value_or(U&& fallback) const& -> T
    {
        if (has_value()) {
            return value();
        }
        return static_cast<T>(std::forward<U>(fallback));
    }

    /// Compares result state and contained alternatives when equality is available.
    auto operator==(Result const&) const -> bool = default;

private:
    /// Constructs a result whose variant is initialized with a value.
    explicit Result(std::in_place_index_t<1>, T value)
        : _data{std::in_place_index<1>, std::move(value)}
    {}

    /// Constructs a result whose variant is initialized with an error.
    explicit Result(std::in_place_index_t<2>, E error)
        : _data{std::in_place_index<2>, std::move(error)}
    {}

    /// Throws when this result does not hold a value.
    void ensure_value() const
    {
        if (!has_value()) {
            throw std::logic_error{"Result does not contain a value"};
        }
    }

    /// Throws when this result does not hold an error.
    void ensure_error() const
    {
        if (_data.index() != 2) {
            throw std::logic_error{"Result does not contain an error"};
        }
    }

    std::variant<UninitializedResult, T, E> _data;
};

/// Result specialization for operations that succeed without producing a value.
/// Use value() only to validate success; dereference, arrow, and value_or() are intentionally unavailable.
/// @tparam E Error type.
template <typename E>
class [[nodiscard]] Result<void, E> {
public:
    /// Creates an explicitly uninitialized result.
    Result() noexcept = default;

    /// Creates a successful completion result.
    static auto success() -> Result { return Result{std::in_place_index<1>}; }

    /// Creates a failed completion result by taking ownership of error.
    static auto failure(E error) -> Result { return Result{std::in_place_index<2>, std::move(error)}; }

    /// Returns true when the result has completed or failed.
    [[nodiscard]] auto is_initialized() const noexcept -> bool { return _data.index() != 0; }

    /// Returns true only when the result completed successfully.
    [[nodiscard]] auto has_value() const noexcept -> bool { return _data.index() == 1; }

    /// Returns true only when the result completed successfully.
    explicit operator bool() const noexcept { return has_value(); }

    /// Verifies successful completion.
    /// @throws std::logic_error when the result is not successful.
    void value() const
    {
        if (!has_value()) {
            throw std::logic_error{"Result does not contain a value"};
        }
    }

    /// Returns the represented error.
    /// @throws std::logic_error when no error is present.
    auto error() & -> E&
    {
        ensure_error();
        return std::get<2>(_data);
    }

    /// Returns the represented error without permitting mutation.
    /// @throws std::logic_error when no error is present.
    auto error() const& -> E const&
    {
        ensure_error();
        return std::get<2>(_data);
    }

    /// Moves the represented error out of this result.
    /// @throws std::logic_error when no error is present.
    auto error() && -> E&&
    {
        ensure_error();
        return std::move(std::get<2>(_data));
    }

    /// Compares result state and contained alternatives when equality is available.
    auto operator==(Result const&) const -> bool = default;

private:
    /// Constructs a successful completion variant.
    explicit Result(std::in_place_index_t<1>)
        : _data{std::in_place_index<1>}
    {}

    /// Constructs a failed completion variant.
    explicit Result(std::in_place_index_t<2>, E error)
        : _data{std::in_place_index<2>, std::move(error)}
    {}

    /// Throws when this result does not hold an error.
    void ensure_error() const
    {
        if (_data.index() != 2) {
            throw std::logic_error{"Result does not contain an error"};
        }
    }

    std::variant<UninitializedResult, std::monostate, E> _data;
};

} // namespace jb::core
