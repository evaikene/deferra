#include "page-bindings.hpp"

#include "client_binding.hpp"
#include "page_binding.hpp"
#include "value_binding.hpp"
#include "value_snapshot.hpp"

#include <phpcxx/arguments.hpp>
#include <phpcxx/conversion.hpp>
#include <phpcxx/exception.hpp>

#include <chrono>
#include <optional>
#include <string>
#include <utility>

#include "page_arginfo.h"

namespace jobu_php_test {
namespace {
// Later summary/statistics classes are deliberately absent. These fixture-only
// projections exercise the production typed paging adapters without detail reads.
template <typename Items, typename Project>
auto project_items(Items const& items, Project project) -> php::Result<php::Value>
{
    zval slot;
    array_init(&slot);
    auto result = php::Value::adopt(slot);
    for (auto const& item : items) {
        auto text = project(item);
        add_next_index_stringl(const_cast<zval*>(result.get()), text.data(), text.size());
    }
    return result;
}

auto runs([[maybe_unused]] php::ValueView client, jb::jobu::RunPage const& page) -> php::Result<php::Value>
{
    return project_items(page.items, [](auto const& item) { return item.id.to_string(); });
}

auto run_objects([[maybe_unused]] php::ValueView client, jb::jobu::RunPage const& page) -> php::Result<php::Value>
{
    zval slot;
    array_init(&slot);
    auto result = php::Value::adopt(slot);
    for ([[maybe_unused]] auto const& item : page.items) {
        zval object;
        object_init(&object);
        zend_hash_next_index_insert(Z_ARR_P(result.get()), &object);
    }
    return result;
}

auto attempts([[maybe_unused]] php::ValueView client, jb::jobu::AttemptPage const& page) -> php::Result<php::Value>
{
    return project_items(page.items, [](auto const& item) { return std::to_string(item.attempt_number); });
}

auto statistics([[maybe_unused]] php::ValueView client, jb::jobu::StatisticsPage const& page) -> php::Result<php::Value>
{
    return project_items(page.groups, [](auto const& item) { return std::to_string(item.runs.total); });
}

auto create(zend_execute_data* frame) noexcept -> php::Result<php::Value>
{
    auto           start = jb::core::Clock::now();
    php::Arguments args(frame);
    auto           count = args.count(2, 4);
    if (!count) {
        return count.error();
    }
    auto object = args.object(1, jb::php::snapshot::entry("jobu\\client"));
    if (!object) {
        return object.error();
    }
    auto client = jb::php::native_client(const_cast<zend_object*>(object.value()));
    if (!client) {
        return client.error();
    }
    auto kind  = args.string(2);
    auto limit = args.get(3).get() ? args.integer(3) : php::Result<zend_long>(1);
    if (!kind || !limit) {
        return !kind ? kind.error() : limit.error();
    }
    if (limit.value() < 1 || limit.value() > 200) {
        return php::Error{.kind = php::ErrorKind::Value, .message = "Invalid page limit"};
    }
    std::optional<jb::core::Uuid> id;
    if (args.get(4).get() && !args.get(4).is_null()) {
        auto parsed = jb::php::uuid_from_php(args.get(4));
        if (!parsed) {
            return parsed.error();
        }
        id = parsed.value();
    }
    auto                size = static_cast<std::size_t>(limit.value());
    auto                name = kind.value().bytes();
    jb::php::PageSource source;
    if (name == "queues") {
        source = jb::php::queue_page_source({.include_deleted = true, .page = {.limit = size}});
    }
    else if (name == "jobs") {
        jb::jobu::JobListRequest query{.include_deleted = true,
                                       .type            = jb::jobu::JobType::Cli,
                                       .page            = {.limit = size}};
        if (id) {
            query.queue = *id;
        }
        source = jb::php::job_page_source(std::move(query));
    }
    else if (name == "secrets") {
        source = jb::php::secret_page_source({.limit = size});
    }
    else if (name == "runs" || name == "reject-runs" || name == "cycle-runs") {
        source = jb::php::run_page_source({.filters = {.queue_id = id}, .limit = size},
                                          name == "cycle-runs" ? run_objects : runs);
    }
    else if (name == "attempts" && id) {
        source = jb::php::attempt_page_source({.run_id = *id, .limit = size}, attempts);
    }
    else if (name == "system" || (name == "queue-stats" && id)) {
        jb::jobu::StatisticsRequest query{
            .queue_id = name == "system" ? id : std::nullopt,
            .planned  = {.from = jb::core::UtcClock::now() - std::chrono::hours(1),
                         .to   = jb::core::UtcClock::now() + std::chrono::hours(2)},
            .group_by = jb::jobu::StatisticsGroupBy::Job,
            .limit    = size
        };
        source = name == "system"
                   ? jb::php::system_statistics_page_source(query, statistics)
                   : jb::php::queue_statistics_page_source({.selector = *id, .statistics = query}, statistics);
    }
    else {
        return php::Error{.kind = php::ErrorKind::Value, .message = "Unknown fixture page kind"};
    }

    if (name == "reject-runs") {
        // Reject after native decoding and item projection. A retry must use the
        // same server boundary, not the continuation from this discarded reply.
        source = [fetch = std::move(source),
                  calls = 0](php::ValueView                   owner,
                             jb::php::PageContinuation const& boundary,
                             jb::core::TimePoint              deadline) mutable -> php::Result<jb::php::PageBatch> {
            auto result = fetch(owner, boundary, deadline);
            if (result && ++calls == 2) {
                return php::Error{.kind = php::ErrorKind::Value, .message = "Injected projection failure"};
            }
            return result;
        };
    }
    auto deadline = client.value()->deadline(start);
    if (!deadline) {
        return php::Error{.kind = php::ErrorKind::Value, .message = deadline.error().message};
    }
    return jb::php::make_page(args.get(1), std::move(source), deadline.value());
}
} // namespace

auto register_page_classes() noexcept -> zend_result
{
    register_class_JobUTestPages();
    return SUCCESS;
}
} // namespace jobu_php_test

ZEND_METHOD(JobUTestPages, create)
{
    jb::php::snapshot::return_value(jobu_php_test::create(execute_data), return_value);
}
