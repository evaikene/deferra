#pragma once

#include <jb/core/json.hpp>
#include <phpcxx/result.hpp>
#include <phpcxx/value.hpp>

#include <functional>

namespace jb::php {

/// Install the mutable CLI payload binding during MINIT.
auto bind_cli_payload_class(zend_class_entry* entry) -> bool;

/// Copy and validate a complete CLI template for native create/update requests.
/// Does not resolve secrets, inherit environment, execute PHP, or perform RPC.
auto cli_payload_from_php(::php::ValueView value) -> ::php::Result<core::JsonValue>;

/// Replace every field atomically after complete validation, preserving target identity.
auto replace_cli_payload(zend_object* target, ::php::ValueView source) -> ::php::Result<void>;

/// Attach once to a request-local owner retained and traced for GC. Notification
/// runs only on actual changes and must not execute PHP or mutate this payload.
/// It may borrow the retained owner; detached clones discard both edges.
auto attach_cli_payload(zend_object* target, ::php::ValueView owner, std::function<void()> changed)
    -> ::php::Result<void>;

} // namespace jb::php
