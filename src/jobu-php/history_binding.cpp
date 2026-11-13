#include "history_binding.hpp"

#include "attempt_binding.hpp"
#include "client_binding.hpp"
#include "operation.hpp"
#include "page_binding.hpp"
#include "query_binding.hpp"
#include "queue_binding.hpp"
#include "resource_state.hpp"
#include "run_binding.hpp"
#include "value_snapshot.hpp"

#include <utility>
#include <variant>

namespace jb::php {
namespace {
auto history_runs(zend_execute_data* frame, bool queue_scoped) noexcept -> ::php::Result<::php::Value>
{
    auto                      start = core::Clock::now();
    auto                      owner = ::php::ValueView(&frame->This);
    std::optional<core::Uuid> queue;
    if (queue_scoped) {
        auto state = queue_state(Z_OBJ(frame->This));
        if (!state) {
            return state.error();
        }
        // Retained history is readable without saving or refreshing its owner.
        owner = state.value()->client();
        queue = state.value()->current().id;
    }
    return list_run_history(owner, ::php::Arguments(frame), start, {}, queue);
}
} // namespace

auto history_deadline(::php::ValueView        client,
                      ::php::Arguments const& args,
                      std::uint32_t           position,
                      core::TimePoint         start) -> ::php::Result<core::TimePoint>
{
    auto native = native_client(Z_OBJ_P(client.get()));
    if (!native) {
        return native.error();
    }
    return operation_deadline(*native.value(), args, position, start);
}

auto read_run_details(::php::ValueView client, core::Uuid const& id, core::TimePoint deadline)
    -> ::php::Result<jobu::RunDetails>
{
    auto operation = Operation::bind("run.get", &jobu::ControlClient::get_run, id);
    if (!operation) {
        return snapshot::invalid("Invalid run request");
    }
    auto reply = resource_call(client, deadline, operation.value());
    if (!reply) {
        return reply.error();
    }
    auto* value = std::get_if<jobu::RunDetails>(&reply.value());
    if (!value || value->id != id) {
        return snapshot::invalid("Unexpected run reply identity");
    }
    return std::move(*value);
}

auto read_attempt_details(::php::ValueView client, jobu::AttemptKey const& key, core::TimePoint deadline)
    -> ::php::Result<jobu::AttemptDetails>
{
    auto operation = Operation::bind("attempt.get", &jobu::ControlClient::get_attempt, key);
    if (!operation) {
        return snapshot::invalid("Invalid attempt request");
    }
    auto reply = resource_call(client, deadline, operation.value());
    if (!reply) {
        return reply.error();
    }
    auto* value = std::get_if<jobu::AttemptDetails>(&reply.value());
    if (!value || value->run_id != key.run_id || value->attempt_number != key.attempt_number) {
        return snapshot::invalid("Unexpected attempt reply identity");
    }
    return std::move(*value);
}

auto list_run_history(::php::ValueView          client,
                      ::php::Arguments const&   args,
                      core::TimePoint           start,
                      std::optional<core::Uuid> job,
                      std::optional<core::Uuid> queue) -> ::php::Result<::php::Value>
{
    auto count = args.count(0, 2);
    if (!count) {
        return count.error();
    }
    auto query = snapshot::present(args.get(1)) ? run_query_from_php(args.get(1))
                                                : ::php::Result<jobu::RunQuery>(jobu::RunQuery{});
    if (!query) {
        return query.error();
    }
    if (job) {
        if (query.value().filters.job_id && query.value().filters.job_id != job) {
            return snapshot::invalid("Run query jobId must match this Job");
        }
        // Historical queue IDs belong to each run, not the Job's current queue.
        query.value().filters.job_id = job;
    }
    if (queue) {
        if (query.value().filters.queue_id && query.value().filters.queue_id != queue) {
            return snapshot::invalid("Run query queueId must match this Queue");
        }
        query.value().filters.queue_id = queue;
    }
    auto deadline = history_deadline(client, args, 2, start);
    if (!deadline) {
        return deadline.error();
    }
    return make_page(client, run_page_source(std::move(query).value(), run_summaries_to_php), deadline.value());
}

auto list_attempt_history(::php::ValueView        client,
                          core::Uuid const&       run,
                          ::php::Arguments const& args,
                          core::TimePoint         start,
                          jobu::JobType           type) -> ::php::Result<::php::Value>
{
    auto limit = args.size() >= 1 ? args.integer(1) : ::php::Result<zend_long>(100);
    if (!limit) {
        return limit.error();
    }
    if (limit.value() < 1 || limit.value() > 200) {
        return snapshot::invalid("List limit must be between 1 and 200");
    }
    auto deadline = history_deadline(client, args, 2, start);
    if (!deadline) {
        return deadline.error();
    }
    jobu::AttemptQuery query{.run_id = run, .limit = static_cast<std::uint32_t>(limit.value())};
    return make_page(client, attempt_page_source(query, attempt_summary_projection(type)), deadline.value());
}
} // namespace jb::php

ZEND_METHOD(JobU_Client, listRuns)
{
    jb::php::snapshot::return_value(jb::php::history_runs(execute_data, false), return_value);
}

ZEND_METHOD(JobU_Queue, listRuns)
{
    jb::php::snapshot::return_value(jb::php::history_runs(execute_data, true), return_value);
}
