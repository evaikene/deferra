#pragma once

#include "resource_state.hpp"

#include <jb/core/json.hpp>
#include <phpcxx/result.hpp>
#include <phpcxx/value.hpp>

#include <functional>

namespace jb::php {

/// Install the mutable HTTP payload and immutable header bindings during MINIT.
auto bind_http_payload_classes(zend_class_entry* payload, zend_class_entry* header) -> bool;

/// Copy a validated HTTP template for native create/update, without resolving secrets or running PHP/RPC.
auto http_payload_from_php(::php::ValueView value) -> ::php::Result<core::JsonValue>;

/// Validate and replace all fields atomically, preserving the target identity and attachment.
auto replace_http_payload(zend_object* target, ::php::ValueView source) -> ::php::Result<void>;

/// Validate a detached/attached source and prepare notification-free restoration.
/// The source is copied now; the target object and its attachment retain identity.
auto prepare_http_payload_restore(zend_object* target, ::php::ValueView source) -> ::php::Result<ConfigurationRestore>;

/// Attach once to a request-local owner retained and traced for GC. Notification runs
/// only for actual edits and must not execute PHP or mutate the payload. Clones detach.
auto attach_http_payload(zend_object*           target,
                         ::php::ValueView       owner,
                         std::function<void()>  changed,
                         ConfigurationEditCheck editable = {}) -> ::php::Result<void>;

} // namespace jb::php
