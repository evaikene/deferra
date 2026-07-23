#include "management_rpc.hpp"

#include "management.hpp"
#include "management_json.hpp"
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

constexpr std::array<std::string_view, 7> management_methods{
    "queue.create",
    "queue.get",
    "queue.list",
    "queue.update",
    "queue.suspend",
    "queue.resume",
    "queue.delete",
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

template <typename Decode, typename Invoke, typename Encode>
auto handle_value(std::optional<jb::rpc::JsonValue> const& params, Decode&& decode, Invoke&& invoke, Encode&& encode)
    -> jb::rpc::MethodResult
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
    return jb::rpc::MethodResult::success(std::move(encoded).value());
}

template <typename Decode, typename Invoke>
auto handle_void(std::optional<jb::rpc::JsonValue> const& params, Decode&& decode, Invoke&& invoke)
    -> jb::rpc::MethodResult
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
    return jb::rpc::MethodResult::success(jb::rpc::JsonValue{.data = jb::rpc::JsonNull{}});
}

} // anonymous namespace

auto management_rpc_method_names() noexcept -> std::span<std::string_view const>
{
    return management_methods;
}

auto register_management_methods(jb::rpc::Server&         server,
                                 ManagementService&       service,
                                 AttributeRegistry const& attributes) -> bool
{
    auto const* attribute_registry = &attributes;

    return server.register_method(
               std::string{management_methods[0]},
               [&service,
                attribute_registry](jb::rpc::RequestContext const&,
                                    std::optional<jb::rpc::JsonValue> const& params) -> jb::rpc::MethodResult {
                   return handle_value(
                       params,
                       [attribute_registry](jb::rpc::JsonValue const& value) {
                           return create_queue_request_from_json(value, *attribute_registry);
                       },
                       [&service](CreateQueueRequest request) { return service.create_queue(std::move(request)); },
                       [attribute_registry](Queue const& queue) { return queue_to_json(queue, *attribute_registry); });
               }) &&
           server.register_method(
               std::string{management_methods[1]},
               [&service,
                attribute_registry](jb::rpc::RequestContext const&,
                                    std::optional<jb::rpc::JsonValue> const& params) -> jb::rpc::MethodResult {
                   return handle_value(
                       params,
                       [](jb::rpc::JsonValue const& value) { return queue_selector_from_json(value); },
                       [&service](QueueSelector selector) { return service.get_queue(selector); },
                       [attribute_registry](Queue const& queue) { return queue_to_json(queue, *attribute_registry); });
               }) &&
           server.register_method(
               std::string{management_methods[2]},
               [&service,
                attribute_registry](jb::rpc::RequestContext const&,
                                    std::optional<jb::rpc::JsonValue> const& params) -> jb::rpc::MethodResult {
                   return handle_value(
                       params,
                       [](jb::rpc::JsonValue const& value) { return queue_list_request_from_json(value); },
                       [&service](QueueListRequest request) { return service.list_queues(request); },
                       [attribute_registry](QueuePage const& page) {
                           return queue_page_to_json(page, *attribute_registry);
                       });
               }) &&
           server.register_method(
               std::string{management_methods[3]},
               [&service,
                attribute_registry](jb::rpc::RequestContext const&,
                                    std::optional<jb::rpc::JsonValue> const& params) -> jb::rpc::MethodResult {
                   return handle_value(
                       params,
                       [attribute_registry](jb::rpc::JsonValue const& value) {
                           return update_queue_request_from_json(value, *attribute_registry);
                       },
                       [&service](UpdateQueueRequest request) { return service.update_queue(std::move(request)); },
                       [attribute_registry](Queue const& queue) { return queue_to_json(queue, *attribute_registry); });
               }) &&
           server.register_method(
               std::string{management_methods[4]},
               [&service,
                attribute_registry](jb::rpc::RequestContext const&,
                                    std::optional<jb::rpc::JsonValue> const& params) -> jb::rpc::MethodResult {
                   return handle_value(
                       params,
                       [](jb::rpc::JsonValue const& value) { return queue_selector_from_json(value); },
                       [&service](QueueSelector selector) { return service.suspend_queue(selector); },
                       [attribute_registry](Queue const& queue) { return queue_to_json(queue, *attribute_registry); });
               }) &&
           server.register_method(
               std::string{management_methods[5]},
               [&service,
                attribute_registry](jb::rpc::RequestContext const&,
                                    std::optional<jb::rpc::JsonValue> const& params) -> jb::rpc::MethodResult {
                   return handle_value(
                       params,
                       [](jb::rpc::JsonValue const& value) { return queue_selector_from_json(value); },
                       [&service](QueueSelector selector) { return service.resume_queue(selector); },
                       [attribute_registry](Queue const& queue) { return queue_to_json(queue, *attribute_registry); });
               }) &&
           server.register_method(
               std::string{management_methods[6]},
               [&service](jb::rpc::RequestContext const&,
                          std::optional<jb::rpc::JsonValue> const& params) -> jb::rpc::MethodResult {
                   return handle_void(
                       params,
                       [](jb::rpc::JsonValue const& value) { return queue_selector_from_json(value); },
                       [&service](QueueSelector selector) { return service.delete_queue(selector); });
               });
}

} // namespace jb::jobu
