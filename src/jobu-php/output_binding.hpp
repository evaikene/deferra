#pragma once

#include <jb/core/event_loop_types.hpp>
#include <jb/jobu/history.hpp>
#include <phpcxx/arguments.hpp>
#include <phpcxx/result.hpp>
#include <phpcxx/value.hpp>

#include <optional>

namespace jb::php {
/// Read a bounded slice using the Attempt's stable identity and trusted runner type.
/// start is captured at PHP entry; the result is detached from the resource cache.
auto read_output(::php::ValueView        client,
                 jobu::AttemptKey const& key,
                 jobu::JobType           type,
                 ::php::Arguments const& args,
                 core::TimePoint         start) -> ::php::Result<::php::Value>;

/// Select a retained tail without refreshing resources or waiting for execution.
/// An absent number selects the latest attempt once. Every internal read/reconnect
/// shares the deadline measured from start; the borrowed Client stays request-local.
auto get_output(::php::ValueView                   client,
                core::Uuid const&                  run,
                std::optional<jobu::AttemptNumber> number,
                jobu::JobType                      type,
                ::php::Arguments const&            args,
                core::TimePoint                    start) -> ::php::Result<::php::Value>;
} // namespace jb::php
