#include "statistics_rpc.hpp"

#include "json.hpp"
#include "management.hpp"
#include "protocol.hpp"
#include "server.hpp"
#include "statistics_json.hpp"
#include "statistics_service.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace jb::jobu {
namespace {

constexpr std::array<std::string_view, 2> statistics_methods{"system.stats", "queue.stats"};

auto invalid_params() -> jb::rpc::MethodResult
{
    return jb::rpc::MethodResult::failure(
        {.code = static_cast<std::int64_t>(jb::rpc::ErrorCode::InvalidParams), .message = "Invalid params"});
}

auto internal_error() -> jb::rpc::MethodResult
{
    return jb::rpc::MethodResult::failure(
        {.code = static_cast<std::int64_t>(jb::rpc::ErrorCode::InternalError), .message = "Internal error"});
}

auto response_too_large() -> jb::rpc::MethodResult
{
    return jb::rpc::MethodResult::failure(
        jb::rpc::application_error({.category = jb::core::ErrorCategory::ResourceExhausted,
                                    .code     = "jobu.response.too_large",
                                    .message  = "Resource result exceeds the configured response limit"}));
}

auto bounded_success(jb::rpc::RequestContext const& context, jb::core::JsonValue value) -> jb::rpc::MethodResult
{
    if (context.success_result_max_bytes) {
        auto serialized = jb::core::serialize_json(value);
        if (!serialized) {
            return internal_error();
        }
        if (serialized->size() > *context.success_result_max_bytes) {
            return response_too_large();
        }
    }
    return jb::rpc::MethodResult::success(std::move(value));
}

auto serve_result(jb::rpc::RequestContext const& context, jb::core::Result<StatisticsPage, jb::core::Error> result)
    -> jb::rpc::MethodResult
{
    if (!result) {
        return jb::rpc::MethodResult::failure(jb::rpc::application_error(result.error()));
    }
    auto encoded = statistics_page_to_json(result.value());
    if (!encoded) {
        return internal_error();
    }
    return bounded_success(context, std::move(encoded).value());
}

auto serve_system(jb::rpc::RequestContext const&            context,
                  std::optional<jb::core::JsonValue> const& params,
                  StatisticsService&                        statistics) -> jb::rpc::MethodResult
{
    if (!params) {
        return invalid_params();
    }
    auto request = system_statistics_request_from_json(*params);
    if (!request) {
        return invalid_params();
    }
    return serve_result(context, statistics.read(*request, StatisticsScope::System));
}

auto serve_queue(jb::rpc::RequestContext const&            context,
                 std::optional<jb::core::JsonValue> const& params,
                 StatisticsService&                        statistics,
                 ManagementService&                        management) -> jb::rpc::MethodResult
{
    if (!params) {
        return invalid_params();
    }
    auto request = queue_statistics_request_from_json(*params);
    if (!request) {
        return invalid_params();
    }
    if (auto const* cursor = std::get_if<CursorRequest>(&*request)) {
        return serve_result(context, statistics.read(StatisticsListRequest{*cursor}, StatisticsScope::Queue));
    }

    // Resolve even a deleted queue once; later pages use the stable ID stored in the service cursor.
    auto const& initial = std::get<QueueStatisticsQuery>(*request);
    auto        queue   = management.get_queue(initial.selector, true);
    if (!queue) {
        return jb::rpc::MethodResult::failure(jb::rpc::application_error(queue.error()));
    }
    auto query     = initial.statistics;
    query.queue_id = queue->id;
    return serve_result(context, statistics.read(StatisticsListRequest{query}, StatisticsScope::Queue));
}

} // namespace

auto statistics_rpc_method_names() noexcept -> std::span<std::string_view const>
{
    return statistics_methods;
}

auto register_statistics_methods(jb::rpc::Server& server, StatisticsService& statistics, ManagementService& management)
    -> bool
{
    return server.register_method(
               std::string{statistics_methods[0]},
               [&statistics](jb::rpc::RequestContext const& context, std::optional<jb::core::JsonValue> const& params)
                   -> jb::rpc::MethodResult { return serve_system(context, params, statistics); }) &&
           server.register_method(
               std::string{statistics_methods[1]},
               [&statistics, &management](jb::rpc::RequestContext const&            context,
                                          std::optional<jb::core::JsonValue> const& params) -> jb::rpc::MethodResult {
                   return serve_queue(context, params, statistics, management);
               });
}

} // namespace jb::jobu
