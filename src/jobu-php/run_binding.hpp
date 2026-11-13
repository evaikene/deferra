#pragma once

#include <jb/jobu/history.hpp>
#include <phpcxx/result.hpp>
#include <phpcxx/value.hpp>

namespace jb::php {
/// Install the request-local cached Run resource during MINIT.
auto bind_run_class(zend_class_entry* entry) -> bool;
/// Publish a complete run reply, retaining its originating Client without hidden reads.
auto run_to_php(::php::ValueView client, jobu::RunDetails value) -> ::php::Result<::php::Value>;
/// Install immutable summaries and project complete list pages without detail reads.
auto bind_run_summary_class(zend_class_entry* entry) -> bool;
auto run_summaries_to_php(::php::ValueView client, jobu::RunPage const& page) -> ::php::Result<::php::Value>;
} // namespace jb::php
