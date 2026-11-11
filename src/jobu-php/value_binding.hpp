#pragma once

#include <jb/core/time_source.hpp>
#include <jb/core/uuid.hpp>
#include <jb/jobu/job.hpp>
#include <jb/jobu/management.hpp>
#include <jb/jobu/schedule_input.hpp>

#include <phpcxx/result.hpp>
#include <phpcxx/value.hpp>

#include <string>

namespace jb::php {

/// Install immutable value bindings on generated classes during MINIT.
/// Class entries and all returned PHP values belong to the current runtime/request.
auto bind_value_classes(zend_class_entry* secret, zend_class_entry* once, zend_class_entry* cron) -> bool;

/// Validate canonical lowercase UUID spelling without changing the shared parser.
auto uuid_from_php(::php::ValueView value) -> ::php::Result<core::Uuid>;

/// Copy a DateTimeInterface instant into the native clock, checking its narrower range.
auto time_from_php(::php::ValueView value) -> ::php::Result<core::UtcTimePoint>;

/// Return an owned UTC DateTimeImmutable; reject unrepresentable submicrosecond precision.
auto time_to_php(core::UtcTimePoint value) -> ::php::Result<::php::Value>;

/// Copy a symbolic secret name; never resolves or retains secret bytes.
auto secret_name_from_php(::php::ValueView value) -> ::php::Result<std::string>;

/// Create an immutable symbolic reference from a previously validated native name.
auto secret_name_to_php(std::string const& name) -> ::php::Result<::php::Value>;

/// Copy a schedule for creation. Symbolic now remains unresolved for the daemon.
auto creation_schedule_from_php(::php::ValueView value) -> ::php::Result<jobu::JobCreationSchedule>;

/// Copy a replacement schedule, rejecting the creation-only symbolic now marker.
auto update_schedule_from_php(::php::ValueView value) -> ::php::Result<jobu::JobScheduleInput>;

/// Materialize a loaded immutable schedule, including the daemon's resolved timezone.
auto schedule_to_php(jobu::JobSchedule const& value) -> ::php::Result<::php::Value>;

} // namespace jb::php
