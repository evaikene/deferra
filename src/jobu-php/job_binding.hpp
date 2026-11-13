#pragma once

#include <phpcxx/zend.hpp>

namespace jb::php {
/// Install the request-local Job resource during MINIT.
auto bind_job_class(zend_class_entry* entry) -> bool;
} // namespace jb::php
