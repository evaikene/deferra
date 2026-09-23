#include "control_rpc.hpp"

#include "control_json.hpp"
#include "cron.hpp"
#include "history_json.hpp"
#include "json.hpp"
#include "management.hpp"
#include "protocol.hpp"
#include "server.hpp"
#include "utc_timestamp.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace jb::jobu {

namespace {

constexpr std::array<std::string_view, 4> control_methods{
    "job.run_now",
    "run.cancel",
    "schedule.validate",
    "schedule.next",
};

auto invalid_params() -> jb::rpc::MethodResult
{
    return jb::rpc::MethodResult::failure({
        .code    = static_cast<std::int64_t>(jb::rpc::ErrorCode::InvalidParams),
        .message = "Invalid params",
    });
}

auto internal_error() -> jb::rpc::MethodResult
{
    return jb::rpc::MethodResult::failure({
        .code    = static_cast<std::int64_t>(jb::rpc::ErrorCode::InternalError),
        .message = "Internal error",
    });
}

auto response_too_large() -> jb::rpc::MethodResult
{
    return jb::rpc::MethodResult::failure(jb::rpc::application_error({
        .category = jb::core::ErrorCategory::ResourceExhausted,
        .code     = "jobu.response.too_large",
        .message  = "Resource result exceeds the configured response limit",
    }));
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

template <typename Decode, typename Invoke, typename Encode>
auto handle_value(jb::rpc::RequestContext const&            context,
                  std::optional<jb::core::JsonValue> const& params,
                  Decode&&                                  decode,
                  Invoke&&                                  invoke,
                  Encode&&                                  encode) -> jb::rpc::MethodResult
{
    if (!params) {
        return invalid_params();
    }
    auto request = decode(*params);
    if (!request) {
        return invalid_params();
    }

    auto result = invoke(std::move(request).value());
    if (!result) {
        return jb::rpc::MethodResult::failure(jb::rpc::application_error(result.error()));
    }
    auto encoded = encode(result.value());
    if (!encoded) {
        return internal_error();
    }
    return bounded_success(context, std::move(encoded).value());
}

auto handle_schedule_validate(jb::rpc::RequestContext const&            context,
                              std::optional<jb::core::JsonValue> const& params,
                              CronEngine const&                         cron) -> jb::rpc::MethodResult
{
    if (!params) {
        return invalid_params();
    }
    auto schedule = schedule_validate_request_from_json(*params);
    if (!schedule) {
        return invalid_params();
    }
    auto validated = cron.validate(*schedule);
    if (!validated) {
        return jb::rpc::MethodResult::failure(jb::rpc::application_error(validated.error()));
    }
    return bounded_success(context, schedule_validate_result_to_json());
}

auto preview_occurrences(CronEngine const& cron, ScheduleNextRequest const& request)
    -> jb::core::Result<std::vector<jb::core::UtcTimePoint>, jb::core::Error>
{
    auto occurrences = next_cron_occurrences(cron, request.schedule, request.after, request.count);
    if (!occurrences) {
        return occurrences;
    }

    // The clock may represent years beyond the four-digit public timestamp format.
    for (auto occurrence : *occurrences) {
        if (!format_utc_timestamp(occurrence)) {
            return jb::core::Result<std::vector<jb::core::UtcTimePoint>, jb::core::Error>::failure({
                .category = jb::core::ErrorCategory::ResourceExhausted,
                .code     = "jobu.schedule.out_of_range",
                .message  = "Cron occurrence is outside the representable range",
            });
        }
    }
    return occurrences;
}

} // namespace

auto control_rpc_method_names() noexcept -> std::span<std::string_view const>
{
    return control_methods;
}

auto register_control_methods(jb::rpc::Server&         server,
                              ManagementService&       management,
                              Scheduler&               scheduler,
                              CronEngine const&        cron,
                              AttributeRegistry const& attributes) -> bool
{
    // Each handler delegates policy to its existing owner; registrations share the daemon's borrowed dependencies.
    return server.register_method(
               std::string{control_methods[0]},
               [&management, &attributes](jb::rpc::RequestContext const&            context,
                                          std::optional<jb::core::JsonValue> const& params) -> jb::rpc::MethodResult {
                   return handle_value(
                       context,
                       params,
                       run_now_request_from_json,
                       [&management](RunNowRequest request) { return management.run_now(std::move(request)); },
                       [&attributes](JobRun const& run) { return run_details_to_json(run, attributes); });
               }) &&
           server.register_method(
               std::string{control_methods[1]},
               [&scheduler, &attributes](jb::rpc::RequestContext const&            context,
                                         std::optional<jb::core::JsonValue> const& params) -> jb::rpc::MethodResult {
                   return handle_value(
                       context,
                       params,
                       cancel_run_request_from_json,
                       [&scheduler](jb::core::Uuid const& id) { return scheduler.cancel_run(id); },
                       [&attributes](CancelRunResult const& result) {
                           return cancel_run_result_to_json(result, attributes);
                       });
               }) &&
           server.register_method(std::string{control_methods[2]},
                                  [&cron](jb::rpc::RequestContext const&            context,
                                          std::optional<jb::core::JsonValue> const& params) -> jb::rpc::MethodResult {
                                      return handle_schedule_validate(context, params, cron);
                                  }) &&
           server.register_method(
               std::string{control_methods[3]},
               [&cron](jb::rpc::RequestContext const&            context,
                       std::optional<jb::core::JsonValue> const& params) -> jb::rpc::MethodResult {
                   return handle_value(
                       context,
                       params,
                       schedule_next_request_from_json,
                       [&cron](ScheduleNextRequest const& request) { return preview_occurrences(cron, request); },
                       schedule_next_result_to_json);
               });
}

} // namespace jb::jobu
