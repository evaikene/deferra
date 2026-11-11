#pragma once

#include "client.hpp"

#include <phpcxx/arguments.hpp>
#include <phpcxx/result.hpp>
#include <phpcxx/value.hpp>
#include <phpcxx/zend.hpp>

namespace jb::php {

/// Bind generated classes during MINIT. No class entry survives module shutdown.
auto bind_client_classes(zend_class_entry* duration, zend_class_entry* client) -> bool;

/// Read the existing immutable Duration without calling PHP methods.
auto duration_milliseconds(::php::ValueView value) -> ::php::Result<std::chrono::milliseconds>;

/// Create the existing immutable Duration with exact millisecond storage.
auto duration_to_php(std::chrono::milliseconds value) -> ::php::Result<::php::Value>;

/// Borrow initialized native Client state within the owning PHP request.
/// Used by future typed methods and test-only bindings; never retains the object.
auto native_client(zend_object* object) -> ::php::Result<Client*>;

/// Parse a nullable Duration override without changing the stored default.
/// start is captured at PHP call entry, before argument conversion/key generation.
auto operation_deadline(Client const&           client,
                        ::php::Arguments const& arguments,
                        std::uint32_t           position,
                        core::TimePoint         start) -> ::php::Result<core::TimePoint>;

/// Raise the classified operational error in an active PHP frame. Preserves a
/// pending exception and exposes only safe text plus stable operation metadata.
auto raise_operation_failure(Failure const& failure, Operation const& operation) noexcept -> void;

} // namespace jb::php
