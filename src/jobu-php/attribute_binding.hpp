#pragma once

#include <jb/jobu/attribute.hpp>
#include <phpcxx/result.hpp>
#include <phpcxx/value.hpp>

#include <functional>

namespace jb::php {

/// Install mutable configuration bindings during MINIT; entries belong to Zend.
auto bind_attribute_classes(zend_class_entry* attributes,
                            zend_class_entry* cli,
                            zend_class_entry* http,
                            zend_class_entry* defaults) -> bool;

/// Copy visible supplied values and validate partial cross-field relationships.
/// Does not add defaults, call PHP, or perform RPC.
auto attributes_from_php(::php::ValueView value) -> ::php::Result<jobu::AttributeSet>;

/// Merge every supplied source key, validating before changing the target in place.
/// Source and target must be JobAttributes; target runner applicability is enforced.
auto merge_attributes(zend_object* target, ::php::ValueView source) -> ::php::Result<void>;

/// Replace a QueueDefaults layer in place, preserving all issued view identities.
/// Source must be QueueDefaults; failures preserve the entire target layer.
auto replace_defaults(zend_object* target, ::php::ValueView source) -> ::php::Result<void>;

/// Attach configuration to one owner for this request. The owner is retained and
/// traced for GC. The notification must not execute PHP or mutate configuration;
/// it may borrow the retained owner and is called only for actual local changes.
/// Attach once, before publishing views; clones discard both owner and notification.
auto attach_attributes(zend_object* target, ::php::ValueView owner, std::function<void()> changed)
    -> ::php::Result<void>;

} // namespace jb::php
