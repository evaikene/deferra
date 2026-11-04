#pragma once

#include <phpcxx/views.hpp> // IWYU pragma: export

namespace php {

/// An owned zval, confined to its PHP request/execution thread.
///
/// Default and moved-from values are IS_UNDEF, distinct from PHP null. Accepts
/// ordinary PHP values, including references, or IS_UNDEF; internal engine-only
/// slot types such as IS_INDIRECT are outside this contract. Destruction can invoke
/// PHP object/resource cleanup and must precede request shutdown. PHP bailout may
/// skip C++ destructors; this type does not provide a bailout recovery boundary.
class Value {
public:
    Value() noexcept = default;
    ~Value() noexcept;
    Value(Value const&)                    = delete;
    auto operator=(Value const&) -> Value& = delete;
    Value(Value&& other) noexcept;
    auto operator=(Value&& other) noexcept -> Value&;

    /// Construct a valid PHP null value.
    static auto null() noexcept -> Value;

    /// Transfer a zval's ownership without retaining it and set the source slot to IS_UNDEF.
    static auto adopt(zval& value) noexcept -> Value;

    /// Copy using ZVAL_COPY semantics, retaining PHP reference wrappers without dereferencing.
    static auto retain(ValueView value) noexcept -> Value;

    /// Use ZVAL_DUP semantics: duplicate a direct array, otherwise retain/copy its PHP value.
    auto duplicate() const -> Value;

    /// Transfer ownership to the returned zval; caller must consume/destroy it. Leave IS_UNDEF.
    auto release() noexcept -> zval;

    auto get() const noexcept -> zval const* { return &_value; }

    auto view() const& noexcept -> ValueView { return ValueView(&_value); }

    auto view() const&& -> ValueView = delete;

    auto is_initialized() const noexcept -> bool { return view().is_initialized(); }

    auto is_null() const noexcept -> bool { return view().is_null(); }

private:
    zval _value{};
};

} // namespace php
