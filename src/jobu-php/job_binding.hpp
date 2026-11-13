#pragma once

#include <jb/jobu/management.hpp>
#include <phpcxx/result.hpp>
#include <phpcxx/value.hpp>

namespace jb::php {
/// Publish a decoded resource without fetching details; retain its originating Client.
auto job_to_php(::php::ValueView client, jobu::JobDefinition value) -> ::php::Result<::php::Value>;
/// Install the request-local Job resource during MINIT.
auto bind_job_class(zend_class_entry* entry) -> bool;
} // namespace jb::php
