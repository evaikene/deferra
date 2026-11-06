#pragma once

#include "client.hpp"

#include <phpcxx/result.hpp>
#include <phpcxx/zend.hpp>

namespace jb::php {

/// Bind generated classes during MINIT. No class entry survives module shutdown.
auto bind_client_classes(zend_class_entry* duration, zend_class_entry* client) -> bool;

/// Borrow initialized native Client state within the owning PHP request.
/// Used by future typed methods and test-only bindings; never retains the object.
auto native_client(zend_object* object) -> ::php::Result<Client*>;

} // namespace jb::php
