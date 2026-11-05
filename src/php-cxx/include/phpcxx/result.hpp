#pragma once

#include <phpcxx/error.hpp> // IWYU pragma: export

#include <cassert>
#include <optional>
#include <utility>
#include <variant>

namespace php {

/// A value or recoverable Error. Supports move-only values without implicit Zend retains.
/// Accessors require the matching alternative; misuse is a programming error.
/// Copyability follows T. This container itself requires no initialized PHP request.
template <typename T>
class [[nodiscard]] Result {
public:
    /// Construct success, moving the supplied value into the result.
    Result(T value)
        : _state(std::in_place_index<0>, std::move(value))
    {}

    /// Construct failure with an owned diagnostic.
    Result(Error error)
        : _state(std::in_place_index<1>, std::move(error))
    {}

    /// Whether this result contains a value.
    explicit operator bool() const noexcept { return _state.index() == 0; }

    /// Borrow the successful value; requires success.
    auto value() & noexcept -> T&
    {
        assert(*this);
        return *std::get_if<0>(&_state);
    }

    /// Borrow the successful value; requires success.
    auto value() const& noexcept -> T const&
    {
        assert(*this);
        return *std::get_if<0>(&_state);
    }

    /// Move the successful value out; requires success.
    auto value() && noexcept -> T&&
    {
        assert(*this);
        return std::move(*std::get_if<0>(&_state));
    }

    /// Borrow the failure; requires failure. Valid only while this result lives.
    auto error() const& noexcept -> Error const&
    {
        assert(!*this);
        return *std::get_if<1>(&_state);
    }

    /// Move the failure out; requires failure.
    auto error() && noexcept -> Error
    {
        assert(!*this);
        return std::move(*std::get_if<1>(&_state));
    }

private:
    std::variant<T, Error> _state;
};

/// Success without a payload, or an owned recoverable Error.
template <>
class [[nodiscard]] Result<void> {
public:
    /// Construct success.
    Result() noexcept = default;

    /// Construct failure.
    Result(Error error)
        : _error(std::move(error))
    {}

    /// Whether the operation succeeded.
    explicit operator bool() const noexcept { return !_error.has_value(); }

    /// Borrow the failure; requires failure and a live result.
    auto error() const& noexcept -> Error const&
    {
        assert(_error);
        return *_error;
    }

    /// Move the failure out; requires failure.
    auto error() && noexcept -> Error
    {
        assert(_error);
        return std::move(*_error);
    }

private:
    std::optional<Error> _error;
};

} // namespace php
