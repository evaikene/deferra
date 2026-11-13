#pragma once

#include "resource_state.hpp"
#include <phpcxx/zend.hpp>

namespace jb::php {
/// Publish a decoded resource without fetching details; retain its originating Client.
auto queue_to_php(::php::ValueView client, jobu::Queue value) -> ::php::Result<::php::Value>;
/// Borrow the initialized Queue state for a synchronous creation/move operation.
auto queue_state(zend_object* object) -> ::php::Result<ResourceState<jobu::Queue>*>;

/// Install the non-cloneable, request-local Queue binding during MINIT.
auto bind_queue_class(zend_class_entry* queue) -> bool;
} // namespace jb::php
