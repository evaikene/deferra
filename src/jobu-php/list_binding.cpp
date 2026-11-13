#include "client_binding.hpp"
#include "page_binding.hpp"
#include "queue_binding.hpp"
#include "value_binding.hpp"
#include "value_snapshot.hpp"

#include <jb/core/text_validation.hpp>
#include <jb/jobu/management_json.hpp>
#include <phpcxx/arguments.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace jb::php {
namespace {
using core::JsonValue;

auto add_enum(JsonValue::Object& fields, ::php::ValueView input, std::string const& name, std::string_view type)
    -> ::php::Result<void>
{
    if (!snapshot::present(input)) {
        return {};
    }
    auto text = snapshot::enum_text(input, type);
    if (!text) {
        return text.error();
    }
    fields.emplace(name, JsonValue{.data = std::move(text).value()});
    return {};
}

// includeDeleted and limit are adjacent in every public list signature.
auto add_page_options(JsonValue::Object& fields, ::php::Arguments const& args, std::uint32_t position)
    -> ::php::Result<void>
{
    auto include = args.size() >= position ? args.boolean(position) : ::php::Result<bool>(false);
    if (!include) {
        return include.error();
    }
    auto limit = args.size() > position ? args.integer(position + 1) : ::php::Result<zend_long>(100);
    if (!limit) {
        return limit.error();
    }
    if (limit.value() < 1 || limit.value() > 200) {
        return snapshot::invalid("List limit must be between 1 and 200");
    }
    fields.emplace("include_deleted", JsonValue{.data = include.value()});
    fields.emplace("limit", JsonValue{.data = static_cast<std::int64_t>(limit.value())});
    return {};
}

auto add_queue_selector(JsonValue::Object& fields, ::php::Arguments const& args) -> ::php::Result<void>
{
    if (snapshot::present(args.get(1)) && snapshot::present(args.get(2))) {
        return snapshot::invalid("Specify only one of queueName and queueId");
    }
    if (snapshot::present(args.get(1))) {
        auto text = args.string(1);
        if (!text) {
            return text.error();
        }
        auto name     = std::string(text.value().bytes());
        auto marker   = name.rfind("-deleted#");
        bool reserved = marker != std::string::npos && core::Uuid::parse(std::string_view(name).substr(marker + 9));
        if (name.empty() || name.size() > 128 || !core::is_valid_utf8(name) || core::has_ascii_control(name) ||
            reserved) {
            return snapshot::invalid("Invalid queue name");
        }
        fields.emplace("queue_name", JsonValue{.data = std::move(name)});
    }
    else if (snapshot::present(args.get(2))) {
        auto text = args.string(2);
        if (!text) {
            return text.error();
        }
        auto id = uuid_from_php(args.get(2));
        if (!id) {
            return id.error();
        }
        fields.emplace("queue_id", JsonValue{.data = id.value().to_string()});
    }
    return {};
}

auto list_queues(zend_execute_data* frame) noexcept -> ::php::Result<::php::Value>
{
    auto             start = core::Clock::now();
    ::php::Arguments args(frame);
    auto             count = args.count(0, 4);
    if (!count) {
        return count.error();
    }
    auto client = native_client(Z_OBJ(frame->This));
    if (!client) {
        return client.error();
    }

    auto fields = JsonValue::Object{};
    auto step   = add_enum(fields, args.get(1), "state", "jobu\\queuestate");
    if (!step) {
        return step.error();
    }
    step = add_page_options(fields, args, 2);
    if (!step) {
        return step.error();
    }
    auto request = jobu::queue_list_request_from_json(JsonValue{.data = std::move(fields)});
    if (!request) {
        return snapshot::invalid("Invalid queue list filters");
    }
    auto deadline = operation_deadline(*client.value(), args, 4, start);
    if (!deadline) {
        return deadline.error();
    }
    return make_page(::php::ValueView(&frame->This), queue_page_source(std::move(request).value()), deadline.value());
}

auto list_jobs(zend_execute_data* frame, bool queue_scoped) noexcept -> ::php::Result<::php::Value>
{
    auto             start = core::Clock::now();
    ::php::Arguments args(frame);
    auto             count = args.count(0, queue_scoped ? 5 : 7);
    if (!count) {
        return count.error();
    }

    auto owner  = ::php::ValueView(&frame->This);
    auto fields = JsonValue::Object{};
    if (queue_scoped) {
        auto state = queue_state(Z_OBJ(frame->This));
        if (!state) {
            return state.error();
        }
        // Listing is a read of persisted definitions, even with local edits or
        // deletion. Use stable identity; the daemon applies includeDeleted.
        owner = state.value()->client();
        fields.emplace("queue_id", JsonValue{.data = state.value()->baseline().id.to_string()});
    }
    else {
        auto selected = add_queue_selector(fields, args);
        if (!selected) {
            return selected.error();
        }
    }
    auto client = native_client(const_cast<zend_object*>(owner.get_object()));
    if (!client) {
        return client.error();
    }

    // Both entry points share the same trailing filters. Own each converted
    // value before any later scalar conversion can call back into PHP.
    std::uint32_t position = queue_scoped ? 1 : 3;
    auto          step     = add_enum(fields, args.get(position), "state", "jobu\\jobstate");
    if (!step) {
        return step.error();
    }
    step = add_enum(fields, args.get(position + 1), "type", "jobu\\jobtype");
    if (!step) {
        return step.error();
    }
    step = add_page_options(fields, args, position + 2);
    if (!step) {
        return step.error();
    }
    auto request = jobu::job_list_request_from_json(JsonValue{.data = std::move(fields)});
    if (!request) {
        return snapshot::invalid("Invalid job list filters");
    }
    auto deadline = operation_deadline(*client.value(), args, position + 4, start);
    if (!deadline) {
        return deadline.error();
    }
    return make_page(owner, job_page_source(std::move(request).value()), deadline.value());
}
} // namespace
} // namespace jb::php

ZEND_METHOD(JobU_Client, listQueues)
{
    jb::php::snapshot::return_value(jb::php::list_queues(execute_data), return_value);
}

ZEND_METHOD(JobU_Client, listJobs)
{
    jb::php::snapshot::return_value(jb::php::list_jobs(execute_data, false), return_value);
}

ZEND_METHOD(JobU_Queue, listJobs)
{
    jb::php::snapshot::return_value(jb::php::list_jobs(execute_data, true), return_value);
}
