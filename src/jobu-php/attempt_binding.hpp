#pragma once

#include "page_binding.hpp"

namespace jb::php {
/// Install privately constructed, request-local attempt resources and summaries.
auto bind_attempt_classes(zend_class_entry* attempt, zend_class_entry* summary) -> bool;
/// Read one attempt using its originating Run's immutable runner type.
/// The resulting resource owns its Client and immutable runner type, not a parent Run.
auto load_attempt(::php::ValueView client, jobu::AttemptKey const& key, jobu::JobType type, core::TimePoint deadline)
    -> ::php::Result<::php::Value>;
/// Select a native projection carrying no Zend references and performing no detail reads.
auto attempt_summary_projection(jobu::JobType type) -> PageProjection<jobu::AttemptPage>;
} // namespace jb::php
