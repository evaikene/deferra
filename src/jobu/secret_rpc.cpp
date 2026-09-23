#include "secret_rpc.hpp"

#include "protocol.hpp"
#include "secret_json.hpp"
#include "secret_service.hpp"
#include "server.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace jb::jobu {

namespace {

constexpr std::array<std::string_view, 3> secret_methods{"secret.set", "secret.list", "secret.delete"};

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
        // Oversize is a stable secret operation error; malformed wire data is JSON-RPC invalid params.
        if (request.error().code == "jobu.secret.too_large") {
            return jb::rpc::MethodResult::failure(jb::rpc::application_error(request.error()));
        }
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

auto handle_delete(jb::rpc::RequestContext const&            context,
                   std::optional<jb::core::JsonValue> const& params,
                   SecretService&                            service) -> jb::rpc::MethodResult
{
    if (!params) {
        return invalid_params();
    }
    auto name = secret_delete_request_from_json(*params);
    if (!name) {
        return invalid_params();
    }
    auto result = service.erase(*name);
    if (!result) {
        return jb::rpc::MethodResult::failure(jb::rpc::application_error(result.error()));
    }
    return bounded_success(context, jb::core::JsonValue{.data = jb::core::JsonNull{}});
}

} // namespace

auto secret_rpc_method_names() noexcept -> std::span<std::string_view const>
{
    return secret_methods;
}

auto register_secret_methods(jb::rpc::Server& server, SecretService& service) -> bool
{
    // The service owns admission, transaction, deletion protection and safe error classification.
    return server.register_method(
               std::string{secret_methods[0]},
               [&service](jb::rpc::RequestContext const&            context,
                          std::optional<jb::core::JsonValue> const& params) -> jb::rpc::MethodResult {
                   return handle_value(
                       context,
                       params,
                       set_secret_request_from_json,
                       [&service](SetSecretRequest request) { return service.set(std::move(request)); },
                       secret_metadata_to_json);
               }) &&
           server.register_method(
               std::string{secret_methods[1]},
               [&service](jb::rpc::RequestContext const&            context,
                          std::optional<jb::core::JsonValue> const& params) -> jb::rpc::MethodResult {
                   return handle_value(
                       context,
                       params,
                       secret_list_request_from_json,
                       [&service](SecretListRequest const& request) { return service.list(request); },
                       secret_page_to_json);
               }) &&
           server.register_method(
               std::string{secret_methods[2]},
               [&service](jb::rpc::RequestContext const& context, std::optional<jb::core::JsonValue> const& params)
                   -> jb::rpc::MethodResult { return handle_delete(context, params, service); });
}

} // namespace jb::jobu
