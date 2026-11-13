#pragma once

#include <phpcxx/zend.hpp>

namespace jb::php {
/// Install the non-cloneable, request-local Queue binding during MINIT.
auto bind_queue_class(zend_class_entry* queue) -> bool;
} // namespace jb::php
