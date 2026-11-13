#pragma once

#include "page_state.hpp"

#include <jb/jobu/management.hpp>
#include <jb/jobu/secret.hpp>
#include <jb/jobu/statistics.hpp>
#include <jb/jobu/statistics_json.hpp>

namespace jb::php {
/// Install the non-cloneable Page class during MINIT.
auto bind_page_class(zend_class_entry* entry) -> bool;
/// Eagerly fetch and project the first page before publishing a before-first Page.
auto make_page(::php::ValueView client, PageSource source, core::TimePoint deadline) -> ::php::Result<::php::Value>;

/// Typed private sources retain initial filters/limits and use the existing bridge.
auto queue_page_source(jobu::QueueListRequest request) -> PageSource;
auto job_page_source(jobu::JobListRequest request) -> PageSource;
auto secret_page_source(jobu::SecretListRequest request) -> PageSource;

/// Native projection seams for later public history/statistics bindings. Project
/// the complete decoded page to an indexed array without callbacks or hidden RPC.
/// Function pointers own no Zend state. Tests supply fixture-only projections.
template <typename Page>
using PageProjection = auto (*)(::php::ValueView, Page const&) -> ::php::Result<::php::Value>;
auto run_page_source(jobu::RunQuery query, PageProjection<jobu::RunPage> project) -> PageSource;
auto attempt_page_source(jobu::AttemptQuery query, PageProjection<jobu::AttemptPage> project) -> PageSource;
auto system_statistics_page_source(jobu::StatisticsRequest query, PageProjection<jobu::StatisticsPage> project)
    -> PageSource;
auto queue_statistics_page_source(jobu::QueueStatisticsQuery query, PageProjection<jobu::StatisticsPage> project)
    -> PageSource;
} // namespace jb::php
