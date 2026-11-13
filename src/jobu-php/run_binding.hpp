#pragma once

#include <jb/jobu/history.hpp>
#include <phpcxx/result.hpp>
#include <phpcxx/value.hpp>

namespace jb::php {
/// Install the request-local cached Run resource during MINIT.
auto bind_run_class(zend_class_entry* entry) -> bool;
/// Publish a complete run reply, retaining its originating Client without hidden reads.
auto run_to_php(::php::ValueView client, jobu::RunDetails value) -> ::php::Result<::php::Value>;
} // namespace jb::php
