#include "history_rpc.hpp"

#include "history_json.hpp"
#include "history_service.hpp"
#include "json.hpp"
#include "protocol.hpp"
#include "server.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace jb::jobu {
namespace {

constexpr std::array<std::string_view, 5> history_methods{"run.get",
                                                          "run.list",
                                                          "attempt.get",
                                                          "attempt.list",
                                                          "attempt.output"};

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

template <typename Decode, typename Invoke, typename Encode>
auto handle_read(jb::rpc::RequestContext const&            context,
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

} // namespace

auto history_rpc_method_names() noexcept -> std::span<std::string_view const>
{
    return history_methods;
}

auto register_history_methods(jb::rpc::Server& server, HistoryService& service, AttributeRegistry const& attributes)
    -> bool
{
    // HistoryService owns cursor, read-failure, and shutdown policy; handlers only translate one call.
    return server.register_method(
               std::string{history_methods[0]},
               [&service, &attributes](jb::rpc::RequestContext const&            context,
                                       std::optional<jb::core::JsonValue> const& params) -> jb::rpc::MethodResult {
                   return handle_read(
                       context,
                       params,
                       run_get_request_from_json,
                       [&service](jb::core::Uuid const& id) { return service.get_run(id); },
                       [&attributes](RunDetails const& details) { return run_details_to_json(details, attributes); });
               }) &&
           server.register_method(
               std::string{history_methods[1]},
               [&service](jb::rpc::RequestContext const&            context,
                          std::optional<jb::core::JsonValue> const& params) -> jb::rpc::MethodResult {
                   return handle_read(
                       context,
                       params,
                       run_list_request_from_json,
                       [&service](RunListRequest const& request) { return service.list_runs(request); },
                       run_page_to_json);
               }) &&
           server.register_method(
               std::string{history_methods[2]},
               [&service](jb::rpc::RequestContext const&            context,
                          std::optional<jb::core::JsonValue> const& params) -> jb::rpc::MethodResult {
                   return handle_read(
                       context,
                       params,
                       attempt_get_request_from_json,
                       [&service](AttemptKey const& key) { return service.get_attempt(key); },
                       attempt_details_to_json);
               }) &&
           server.register_method(
               std::string{history_methods[3]},
               [&service](jb::rpc::RequestContext const&            context,
                          std::optional<jb::core::JsonValue> const& params) -> jb::rpc::MethodResult {
                   return handle_read(
                       context,
                       params,
                       attempt_list_request_from_json,
                       [&service](AttemptListRequest const& request) { return service.list_attempts(request); },
                       attempt_page_to_json);
               }) &&
           server.register_method(
               std::string{history_methods[4]},
               [&service](jb::rpc::RequestContext const&            context,
                          std::optional<jb::core::JsonValue> const& params) -> jb::rpc::MethodResult {
                   return handle_read(
                       context,
                       params,
                       attempt_output_request_from_json,
                       [&service](AttemptOutputRequest const& request) { return service.read_output(request); },
                       attempt_output_chunk_to_json);
               });
}

} // namespace jb::jobu
