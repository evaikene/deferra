#pragma once

#include <jb/jobu/history.hpp>
#include <jb/jobu/statistics.hpp>
#include <phpcxx/result.hpp>
#include <phpcxx/views.hpp>

namespace jb::php {
/// Install immutable query bindings on generated classes during MINIT.
auto bind_query_classes(zend_class_entry* range, zend_class_entry* runs, zend_class_entry* statistics) -> bool;
/// Copy validated inputs without resolving server-time defaults or performing RPC.
auto run_query_from_php(::php::ValueView value) -> ::php::Result<jobu::RunQuery>;
auto statistics_query_from_php(::php::ValueView value) -> ::php::Result<jobu::StatisticsRequest>;
} // namespace jb::php
