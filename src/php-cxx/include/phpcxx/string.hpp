#pragma once

#include <phpcxx/views.hpp> // IWYU pragma: export

#include <string_view>

namespace php {

/// One owned Zend string reference, confined to its PHP request/execution thread.
///
/// Default and moved-from owners contain no string. Reference acquisition is
/// explicit; interned strings follow Zend's immutable, uncounted ownership rules.
/// Destroy owners before their PHP request ends. PHP bailout may skip destructors.
class String {
public:
    String() noexcept = default;
    ~String() noexcept;
    String(String const&)                    = delete;
    auto operator=(String const&) -> String& = delete;
    String(String&& other) noexcept;
    auto operator=(String&& other) noexcept -> String&;

    /// Allocate a request-local string, preserving all bytes including NUL.
    static auto create(std::string_view bytes) -> String;

    /// Consume one caller-owned reference without incrementing it; nullptr is allowed.
    static auto adopt(zend_string* string) noexcept -> String;

    /// Acquire a reference to a borrowed string; an absent view yields an absent owner.
    static auto retain(StringView string) noexcept -> String;

    /// Copy bytes using Zend duplication semantics; interned strings may stay shared.
    auto duplicate() const -> String;

    /// Transfer the reference to the caller and leave this owner absent.
    auto release() noexcept -> zend_string*;

    explicit operator bool() const noexcept { return _string != nullptr; }

    auto get() const noexcept -> zend_string const* { return _string; }

    auto view() const& noexcept -> StringView { return StringView(_string); }

    auto view() const&& -> StringView = delete;

private:
    zend_string* _string = nullptr;
};

} // namespace php
