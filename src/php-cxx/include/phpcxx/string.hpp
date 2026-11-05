#pragma once

/// @file
/// Move-only Zend string ownership. See zend.hpp for common integration/lifetime rules.

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
    /// Construct an absent owner; no Zend allocation or reference is acquired.
    String() noexcept = default;

    /// Release the owned reference with zend_string_release(); an absent owner is a no-op.
    ~String() noexcept;

    /// Copying is disabled; use retain() to share or duplicate() to duplicate explicitly.
    String(String const&) = delete;

    /// Copy assignment is disabled; use retain() or duplicate() explicitly.
    auto operator=(String const&) -> String& = delete;

    /// Transfer other's reference without retaining it and leave other absent.
    String(String&& other) noexcept;

    /// Release this owner's previous reference, take other's reference, and return *this.
    /// Leaves other absent; self-move is a no-op. Borrows of released storage may expire.
    auto operator=(String&& other) noexcept -> String&;

    /// Allocate and copy request-local bytes, including embedded NULs, with a trailing NUL.
    /// Empty input creates a present, zero-length string; input storage is not retained.
    static auto create(std::string_view bytes) -> String;

    /// Consume one caller-owned Zend string reference without retaining it; nullptr is allowed.
    /// The caller must not subsequently release the transferred reference. The allocation
    /// must support zend_string_release(); interned strings need no counted reference.
    static auto adopt(zend_string* string) noexcept -> String;

    /// Acquire a reference to a borrowed string; an absent view yields an absent owner.
    static auto retain(StringView string) noexcept -> String;

    /// Duplicate ordinary string storage; interned strings may remain shared and immutable.
    /// An absent owner yields an absent owner. The result owns its reference independently.
    auto duplicate() const -> String;

    /// Transfer the reference without decrementing it and leave this owner absent.
    /// Returns nullptr when absent. The caller must transfer it into another owner/Zend
    /// value or eventually call zend_string_release() on a non-null returned pointer.
    auto release() noexcept -> zend_string*;

    /// Whether a string is present; a present empty string is true (not PHP truthiness).
    explicit operator bool() const noexcept { return _string != nullptr; }

    /// Borrow the read-only Zend pointer, or nullptr when absent; never release through it.
    /// It remains valid while its backing allocation lives, including across ownership transfer.
    auto get() const noexcept -> zend_string const* { return _string; }

    /// Borrow without retaining; an absent owner yields an absent view.
    /// The view and any returned bytes require the backing allocation/request to remain alive.
    auto view() const& noexcept -> StringView { return StringView(_string); }

    /// Borrowing from a temporary owner is disabled to prevent immediately dangling views.
    auto view() const&& -> StringView = delete;

private:
    zend_string* _string = nullptr;
};

} // namespace php
