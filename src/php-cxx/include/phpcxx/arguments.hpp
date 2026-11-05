#pragma once

#include <phpcxx/result.hpp>
#include <phpcxx/views.hpp>

#include <cstdint>

namespace php {

/// Borrow the currently executing internal function's Zend argument frame.
/// The frame must remain active during every operation (Zend uses it for strict
/// types and diagnostics). Indices are one-based, matching PHP diagnostics.
/// Named arguments/defaults are resolved by Zend/arginfo, not by this wrapper.
/// Parsing dereferences references and may coerce frame slots through Zend; views
/// borrow those slots/payloads and must not survive mutation or the active call.
class Arguments {
public:
    /// frame must equal EG(current_execute_data) and describe an internal call.
    explicit Arguments(zend_execute_data* frame) noexcept
        : _frame(frame)
    {}

    /// Number of frame arguments, including defaults materialized by Zend.
    auto size() const noexcept -> std::uint32_t;

    /// Borrow a raw slot without coercion/dereferencing; out-of-range returns absence.
    /// Inspect get()==nullptr, is_undef(), and dereference().is_null() separately.
    /// Handle optional/default/nullable policy explicitly before typed extraction.
    auto get(std::uint32_t index) const noexcept -> ValueView;

    /// Validate an inclusive count range; minimum must not exceed maximum.
    /// Returns an ArgumentCount failure without raising a PHP exception itself.
    auto count(std::uint32_t minimum, std::uint32_t maximum) const -> Result<void>;

    /// Parse non-null scalars using Zend's caller-sensitive weak/strict rules.
    /// Missing/undefined slots fail; nullable arguments should be branched on with get().
    /// A PHP exception raised by coercion remains pending and takes precedence.
    auto string(std::uint32_t index) const -> Result<StringView>;

    /// Parse a boolean using Zend argument rules, not unconditional truthiness.
    auto boolean(std::uint32_t index) const -> Result<bool>;

    /// Parse an integer using Zend argument rules, including its diagnostics.
    auto integer(std::uint32_t index) const -> Result<zend_long>;

    /// Parse a double using Zend argument rules (including integer widening).
    auto floating(std::uint32_t index) const -> Result<double>;

    /// Borrow an array; no coercion, retain, or separation.
    auto array(std::uint32_t index) const -> Result<ArrayView>;

    /// Borrow an instance of a class/interface, including subclasses.
    auto object(std::uint32_t index, zend_class_entry const* expected) const -> Result<zend_object const*>;

private:
    auto               slot(std::uint32_t index) const -> Result<zval*>;
    zend_execute_data* _frame;
};

} // namespace php
