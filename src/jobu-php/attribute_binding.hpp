#pragma once

#include "resource_state.hpp"

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

/// Construct detached runner-specific attributes from a validated native reply.
auto job_attributes_to_php(jobu::JobType runner, jobu::AttributeSet values) -> ::php::Result<::php::Value>;

/// Construct a detached defaults object from a validated native reply without calling PHP.
auto defaults_to_php(jobu::AttributeSet values) -> ::php::Result<::php::Value>;

/// Merge every supplied source key, validating before changing the target in place.
/// Source and target must be JobAttributes; target runner applicability is enforced.
auto merge_attributes(zend_object* target, ::php::ValueView source) -> ::php::Result<void>;

/// Replace a QueueDefaults layer in place, preserving all issued view identities.
/// Source must be QueueDefaults; failures preserve the entire target layer.
auto replace_defaults(zend_object* target, ::php::ValueView source) -> ::php::Result<void>;

/// Copy current values without whole-layer validation, for local dirty tracking.
/// Intermediate edits may violate cross-field rules until save validates them.
auto attribute_snapshot(::php::ValueView value) -> ::php::Result<jobu::AttributeSet>;
auto equal_attributes(jobu::AttributeSet const& left, jobu::AttributeSet const& right) -> bool;

/// Compute the job.update patch without validating intermediate cross-field edits.
/// Reject key removal: the job update protocol only supports supplied-key merges.
auto changed_attributes(jobu::AttributeSet const& baseline, jobu::AttributeSet const& current)
    -> ::php::Result<jobu::AttributeSet>;

/// Validate and prepare a full native layer restoration, preserving attached
/// object/view identity. Queue typed views cannot replace their shared root layer.
auto prepare_attributes_restore(zend_object* target, jobu::AttributeSet values) -> ::php::Result<ConfigurationRestore>;

/// Attach configuration to one owner for this request. The owner is retained and
/// traced for GC. The notification must not execute PHP or mutate configuration;
/// it may borrow the retained owner and is called only for actual local changes.
/// Attach once, before publishing views; clones discard both owner and notification.
auto attach_attributes(zend_object*           target,
                       ::php::ValueView       owner,
                       std::function<void()>  changed,
                       ConfigurationEditCheck editable = {}) -> ::php::Result<void>;

} // namespace jb::php
