#pragma once

#include <jb/core/event_loop_types.hpp>
#include <jb/jobu/history.hpp>
#include <phpcxx/arguments.hpp>
#include <phpcxx/result.hpp>
#include <phpcxx/value.hpp>

#include <optional>

namespace jb::php {
/// Use the originating Client's default or the explicit override, measured from PHP entry.
auto history_deadline(::php::ValueView        client,
                      ::php::Arguments const& args,
                      std::uint32_t           position,
                      core::TimePoint         start) -> ::php::Result<core::TimePoint>;
/// Read retained history by stable identity and reject mismatched replies before publication.
auto read_run_details(::php::ValueView client, core::Uuid const& id, core::TimePoint deadline)
    -> ::php::Result<jobu::RunDetails>;
auto read_attempt_details(::php::ValueView client, jobu::AttemptKey const& key, core::TimePoint deadline)
    -> ::php::Result<jobu::AttemptDetails>;
/// Parse the query and optional job/queue scope without changing the caller's immutable query.
auto list_run_history(::php::ValueView          client,
                      ::php::Arguments const&   args,
                      core::TimePoint           start,
                      std::optional<core::Uuid> job   = {},
                      std::optional<core::Uuid> queue = {}) -> ::php::Result<::php::Value>;
/// List summaries using their Run's immutable type, without detail reads.
auto list_attempt_history(::php::ValueView        client,
                          core::Uuid const&       run,
                          ::php::Arguments const& args,
                          core::TimePoint         start,
                          jobu::JobType           type) -> ::php::Result<::php::Value>;
} // namespace jb::php
