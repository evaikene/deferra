#include "page_binding.hpp"

#include "client_binding.hpp"
#include "job_binding.hpp"
#include "operation.hpp"
#include "queue_binding.hpp"
#include "resource_state.hpp"
#include "result_binding.hpp"
#include "statistics_binding.hpp"
#include "value_snapshot.hpp"

#include <ext/spl/spl_exceptions.h>
#include <phpcxx/arguments.hpp>
#include <phpcxx/exception.hpp>

#include <string>
#include <utility>
#include <variant>

namespace jb::php {
namespace {
using Binding = ::php::ObjectBinding<PageState>;

auto invalid(char const* message) -> ::php::Error
{
    return {.kind = ::php::ErrorKind::Value, .message = message};
}

// Continuations replace only the boundary for keysets. Cursor variants deliberately
// discard every initial field on the wire, while the source retains its query.
template <typename Request>
auto continuation_request(Request const& initial, PageContinuation const& continuation) -> ::php::Result<Request>
{
    if (std::holds_alternative<std::monostate>(continuation)) {
        return initial;
    }
    if constexpr (std::is_same_v<Request, jobu::QueueListRequest> || std::is_same_v<Request, jobu::JobListRequest>) {
        if (auto const* id = std::get_if<core::Uuid>(&continuation)) {
            auto request          = initial;
            request.page.after_id = *id;
            return request;
        }
    }
    else if constexpr (std::is_same_v<Request, jobu::SecretListRequest>) {
        if (auto const* name = std::get_if<std::string>(&continuation)) {
            auto request       = initial;
            request.after_name = *name;
            return request;
        }
    }
    else {
        if (auto const* cursor = std::get_if<jobu::CursorRequest>(&continuation)) {
            return Request(*cursor);
        }
    }
    return invalid("Unexpected page continuation type");
}

template <typename Page>
auto boundary(Page const& page) -> PageContinuation
{
    if constexpr (std::is_same_v<Page, jobu::QueuePage> || std::is_same_v<Page, jobu::JobPage>) {
        if (page.next_after_id) {
            return *page.next_after_id;
        }
    }
    else if constexpr (std::is_same_v<Page, jobu::SecretPage>) {
        if (page.next_after_name) {
            return *page.next_after_name;
        }
    }
    else if (page.next_cursor) {
        return jobu::CursorRequest{*page.next_cursor};
    }
    return std::monostate{};
}

template <typename Request, typename Page>
auto source(std::string name,
            Operation::Accepted (jobu::ControlClient::*method)(Request const&, jobu::ControlCallOptions),
            Request              initial,
            PageProjection<Page> project) -> PageSource
{
    return [name = std::move(name), method, initial = std::move(initial), project](
               ::php::ValueView        client,
               PageContinuation const& continuation,
               core::TimePoint         deadline) -> ::php::Result<PageBatch> {
        auto request = continuation_request(initial, continuation);
        if (!request) {
            return request.error();
        }
        auto operation = Operation::bind(name, method, std::move(request).value());
        if (!operation) {
            return invalid("Unable to prepare page request");
        }
        auto reply = resource_call(client, deadline, operation.value());
        if (!reply) {
            return reply.error();
        }
        auto const* page = std::get_if<Page>(&reply.value());
        if (!page) {
            return invalid("Unexpected page reply type");
        }

        // Finish all item conversions before the caller publishes any new state.
        auto items = project(client, *page);
        if (!items) {
            return items.error();
        }
        if (!items.value().view().is_array()) {
            return invalid("Page projection must return an array");
        }
        auto next     = items.value().view().get_array().empty() ? PageContinuation{} : boundary(*page);
        auto metadata = ::php::Value::null();
        if constexpr (std::is_same_v<Page, jobu::StatisticsPage>) {
            auto converted = statistics_metadata_to_php(*page);
            if (!converted) {
                return converted.error();
            }
            metadata = std::move(converted).value();
        }
        return PageBatch{std::move(items).value(), std::move(next), std::move(metadata)};
    };
}

template <typename Items, typename Project>
auto project_items(Items const& items, Project project) -> ::php::Result<::php::Value>
{
    zval slot;
    array_init(&slot);
    auto result = ::php::Value::adopt(slot);
    for (auto const& item : items) {
        auto converted = project(item);
        if (!converted) {
            return converted.error();
        }
        auto value = std::move(converted).value().release();
        zend_hash_next_index_insert(Z_ARR_P(result.get()), &value);
    }
    return result;
}

auto queues(::php::ValueView client, jobu::QueuePage const& page) -> ::php::Result<::php::Value>
{
    return project_items(page.items, [client](auto const& item) { return queue_to_php(client, item); });
}

auto jobs(::php::ValueView client, jobu::JobPage const& page) -> ::php::Result<::php::Value>
{
    return project_items(page.items, [client](auto const& item) { return job_to_php(client, item); });
}

auto secrets([[maybe_unused]] ::php::ValueView client, jobu::SecretPage const& page) -> ::php::Result<::php::Value>
{
    return project_items(page.items, secret_metadata_to_php);
}

auto read(zend_execute_data* frame, bool items) noexcept -> ::php::Result<::php::Value>
{
    auto count = ::php::Arguments(frame).count(0, 0);
    if (!count) {
        return count.error();
    }
    auto page = Binding::get(Z_OBJ(frame->This));
    if (!page) {
        return page.error();
    }
    if (items) {
        return page.value()->items();
    }
    zval result;
    ZVAL_BOOL(&result, page.value()->has_next());
    return ::php::Value::adopt(result);
}

auto advance(zend_execute_data* frame) noexcept -> ::php::Result<::php::Value>
{
    auto             start = core::Clock::now();
    ::php::Arguments args(frame);
    auto             count = args.count(0, 1);
    if (!count) {
        return count.error();
    }
    auto page = Binding::get(Z_OBJ(frame->This));
    if (!page) {
        return page.error();
    }
    auto client = native_client(const_cast<zend_object*>(page.value()->client().get_object()));
    if (!client) {
        return client.error();
    }
    auto deadline = operation_deadline(*client.value(), args, 1, start);
    if (!deadline) {
        return deadline.error();
    }
    if (!page.value()->has_next()) {
        ::php::raise({.kind = ::php::ErrorKind::Runtime, .message = "Page has no next page"},
                     spl_ce_OutOfBoundsException);
        return ::php::pending_exception();
    }
    auto advanced = page.value()->next(deadline.value());
    if (!advanced) {
        return advanced.error();
    }
    return ::php::Value::null();
}
} // namespace

auto bind_page_class(zend_class_entry* entry) -> bool
{
    entry->ce_flags |= ZEND_ACC_NO_DYNAMIC_PROPERTIES;
    return static_cast<bool>(Binding::bind(entry));
}

auto make_page(::php::ValueView client, PageSource source, core::TimePoint deadline) -> ::php::Result<::php::Value>
{
    auto first = source(client, PageContinuation{}, deadline);
    if (!first) {
        return first.error();
    }
    zval slot{};
    object_init_ex(&slot, snapshot::entry("jobu\\page"));
    auto owner   = ::php::Value::adopt(slot);
    auto created = Binding::construct(Z_OBJ_P(owner.get()),
                                      ::php::Value::retain(client),
                                      std::move(source),
                                      std::move(first).value());
    if (!created) {
        return created.error();
    }
    return owner;
}

auto queue_page_source(jobu::QueueListRequest request) -> PageSource
{
    return source("queue.list", &jobu::ControlClient::list_queues, request, queues);
}

auto job_page_source(jobu::JobListRequest request) -> PageSource
{
    return source("job.list", &jobu::ControlClient::list_jobs, std::move(request), jobs);
}

auto secret_page_source(jobu::SecretListRequest request) -> PageSource
{
    return source("secret.list", &jobu::ControlClient::list_secrets, std::move(request), secrets);
}

auto run_page_source(jobu::RunQuery query, PageProjection<jobu::RunPage> project) -> PageSource
{
    return source("run.list", &jobu::ControlClient::list_runs, jobu::RunListRequest(std::move(query)), project);
}

auto attempt_page_source(jobu::AttemptQuery query, PageProjection<jobu::AttemptPage> project) -> PageSource
{
    return source("attempt.list", &jobu::ControlClient::list_attempts, jobu::AttemptListRequest(query), project);
}

auto system_statistics_page_source(jobu::StatisticsRequest query, PageProjection<jobu::StatisticsPage> project)
    -> PageSource
{
    return source("system.stats", &jobu::ControlClient::system_statistics, jobu::StatisticsListRequest(query), project);
}

auto queue_statistics_page_source(jobu::QueueStatisticsQuery query, PageProjection<jobu::StatisticsPage> project)
    -> PageSource
{
    return source("queue.stats",
                  &jobu::ControlClient::queue_statistics,
                  jobu::QueueStatisticsListRequest(std::move(query)),
                  project);
}
} // namespace jb::php

ZEND_METHOD(JobU_Page, __construct)
{
    (void)execute_data;
    (void)return_value;
    php::raise({.kind = php::ErrorKind::Runtime, .message = "Pages are created by JobU"});
}

ZEND_METHOD(JobU_Page, hasNext)
{
    jb::php::snapshot::return_value(jb::php::read(execute_data, false), return_value);
}

ZEND_METHOD(JobU_Page, items)
{
    jb::php::snapshot::return_value(jb::php::read(execute_data, true), return_value);
}

ZEND_METHOD(JobU_Page, next)
{
    jb::php::snapshot::return_value(jb::php::advance(execute_data), return_value);
}

ZEND_METHOD(JobU_Page, getStatisticsMetadata)
{
    auto count = php::Arguments(execute_data).count(0, 0);
    if (!count) {
        php::raise(count.error());
        return;
    }
    auto page = php::ObjectBinding<jb::php::PageState>::get(Z_OBJ(execute_data->This));
    if (!page) {
        php::raise(page.error());
        return;
    }
    *return_value = page.value()->statistics_metadata().release();
}
