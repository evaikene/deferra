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

constexpr std::array<std::string_view, 15> management_methods{
    "queue.create",
    "queue.get",
    "queue.list",
    "queue.update",
    "queue.suspend",
    "queue.resume",
    "queue.delete",
    "job.create",
    "job.get",
    "job.list",
    "job.update",
    "job.suspend",
    "job.resume",
    "job.move",
    "job.delete",
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

void notify_committed_mutation(ManagementMutationHandler const& mutation_committed)
{
    // Service success is the durable boundary; notify before response encoding so a later encoding failure cannot hide
    // committed work from the scheduler.
    if (mutation_committed) {
        mutation_committed();
    }
}

template <typename Decode, typename Invoke, typename Encode>
auto handle_value(std::optional<jb::core::JsonValue> const& params,
                  Decode&&                                  decode,
                  Invoke&&                                  invoke,
                  Encode&&                                  encode,
                  ManagementMutationHandler const&          mutation_committed = {}) -> jb::rpc::MethodResult
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

    notify_committed_mutation(mutation_committed);
    auto encoded = encode(result.value());
    if (!encoded) {
        return internal_error();
    }
    return jb::rpc::MethodResult::success(std::move(encoded).value());
}

template <typename Decode, typename Invoke>
auto handle_void(std::optional<jb::core::JsonValue> const& params,
                 Decode&&                                  decode,
                 Invoke&&                                  invoke,
                 ManagementMutationHandler const&          mutation_committed = {}) -> jb::rpc::MethodResult
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
    notify_committed_mutation(mutation_committed);
    return jb::rpc::MethodResult::success(jb::core::JsonValue{.data = jb::core::JsonNull{}});
}

} // anonymous namespace

auto management_rpc_method_names() noexcept -> std::span<std::string_view const>
{
    return management_methods;
}

auto register_management_methods(jb::rpc::Server&          server,
                                 ManagementService&        service,
                                 AttributeRegistry const&  attributes,
                                 ManagementMutationHandler mutation_committed) -> bool
{
    auto const* attribute_registry = &attributes;

    return server.register_method(
               std::string{management_methods[0]},
               [&service, attribute_registry, mutation_committed](
                   jb::rpc::RequestContext const&,
                   std::optional<jb::core::JsonValue> const& params) -> jb::rpc::MethodResult {
                   return handle_value(
                       params,
                       [attribute_registry](jb::core::JsonValue const& value) {
                           return create_queue_request_from_json(value, *attribute_registry);
                       },
                       [&service](CreateQueueRequest request) { return service.create_queue(std::move(request)); },
                       [attribute_registry](Queue const& queue) { return queue_to_json(queue, *attribute_registry); },
                       mutation_committed);
               }) &&
           server.register_method(
               std::string{management_methods[1]},
               [&service,
                attribute_registry](jb::rpc::RequestContext const&,
                                    std::optional<jb::core::JsonValue> const& params) -> jb::rpc::MethodResult {
                   return handle_value(
                       params,
                       [](jb::core::JsonValue const& value) { return queue_selector_from_json(value); },
                       [&service](QueueSelector const& selector) { return service.get_queue(selector); },
                       [attribute_registry](Queue const& queue) { return queue_to_json(queue, *attribute_registry); });
               }) &&
           server.register_method(
               std::string{management_methods[2]},
               [&service,
                attribute_registry](jb::rpc::RequestContext const&,
                                    std::optional<jb::core::JsonValue> const& params) -> jb::rpc::MethodResult {
                   return handle_value(
                       params,
                       [](jb::core::JsonValue const& value) { return queue_list_request_from_json(value); },
                       [&service](QueueListRequest request) { return service.list_queues(request); },
                       [attribute_registry](QueuePage const& page) {
                           return queue_page_to_json(page, *attribute_registry);
                       });
               }) &&
           server.register_method(
               std::string{management_methods[3]},
               [&service, attribute_registry, mutation_committed](
                   jb::rpc::RequestContext const&,
                   std::optional<jb::core::JsonValue> const& params) -> jb::rpc::MethodResult {
                   return handle_value(
                       params,
                       [attribute_registry](jb::core::JsonValue const& value) {
                           return update_queue_request_from_json(value, *attribute_registry);
                       },
                       [&service](UpdateQueueRequest request) { return service.update_queue(std::move(request)); },
                       [attribute_registry](Queue const& queue) { return queue_to_json(queue, *attribute_registry); },
                       mutation_committed);
               }) &&
           server.register_method(
               std::string{management_methods[4]},
               [&service, attribute_registry, mutation_committed](
                   jb::rpc::RequestContext const&,
                   std::optional<jb::core::JsonValue> const& params) -> jb::rpc::MethodResult {
                   return handle_value(
                       params,
                       [](jb::core::JsonValue const& value) { return queue_selector_from_json(value); },
                       [&service](QueueSelector const& selector) { return service.suspend_queue(selector); },
                       [attribute_registry](Queue const& queue) { return queue_to_json(queue, *attribute_registry); },
                       mutation_committed);
               }) &&
           server.register_method(
               std::string{management_methods[5]},
               [&service, attribute_registry, mutation_committed](
                   jb::rpc::RequestContext const&,
                   std::optional<jb::core::JsonValue> const& params) -> jb::rpc::MethodResult {
                   return handle_value(
                       params,
                       [](jb::core::JsonValue const& value) { return queue_selector_from_json(value); },
                       [&service](QueueSelector const& selector) { return service.resume_queue(selector); },
                       [attribute_registry](Queue const& queue) { return queue_to_json(queue, *attribute_registry); },
                       mutation_committed);
               }) &&
           server.register_method(
               std::string{management_methods[6]},
               [&service,
                mutation_committed](jb::rpc::RequestContext const&,
                                    std::optional<jb::core::JsonValue> const& params) -> jb::rpc::MethodResult {
                   return handle_void(
                       params,
                       [](jb::core::JsonValue const& value) { return queue_selector_from_json(value); },
                       [&service](QueueSelector const& selector) { return service.delete_queue(selector); },
                       mutation_committed);
               }) &&
           server.register_method(
               std::string{management_methods[7]},
               [&service, attribute_registry, mutation_committed](
                   jb::rpc::RequestContext const&,
                   std::optional<jb::core::JsonValue> const& params) -> jb::rpc::MethodResult {
                   return handle_value(
                       params,
                       [attribute_registry](jb::core::JsonValue const& value) {
                           return create_job_request_from_json(value, *attribute_registry);
                       },
                       [&service](CreateJobRequest request) { return service.create_job(std::move(request)); },
                       [attribute_registry](JobDefinition const& job) { return job_to_json(job, *attribute_registry); },
                       mutation_committed);
               }) &&
           server.register_method(std::string{management_methods[8]},
                                  [&service, attribute_registry](
                                      jb::rpc::RequestContext const&,
                                      std::optional<jb::core::JsonValue> const& params) -> jb::rpc::MethodResult {
                                      return handle_value(
                                          params,
                                          [](jb::core::JsonValue const& value) { return job_id_from_json(value); },
                                          [&service](jb::core::Uuid id) { return service.get_job(id); },
                                          [attribute_registry](JobDefinition const& job) {
                                              return job_to_json(job, *attribute_registry);
                                          });
                                  }) &&
           server.register_method(
               std::string{management_methods[9]},
               [&service,
                attribute_registry](jb::rpc::RequestContext const&,
                                    std::optional<jb::core::JsonValue> const& params) -> jb::rpc::MethodResult {
                   return handle_value(
                       params,
                       [](jb::core::JsonValue const& value) { return job_list_request_from_json(value); },
                       [&service](JobListRequest const& request) { return service.list_jobs(request); },
                       [attribute_registry](JobPage const& page) {
                           return job_page_to_json(page, *attribute_registry);
                       });
               }) &&
           server.register_method(
               std::string{management_methods[10]},
               [&service, attribute_registry, mutation_committed](
                   jb::rpc::RequestContext const&,
                   std::optional<jb::core::JsonValue> const& params) -> jb::rpc::MethodResult {
                   return handle_value(
                       params,
                       [attribute_registry](jb::core::JsonValue const& value) {
                           return update_job_request_from_json(value, *attribute_registry);
                       },
                       [&service](UpdateJobRequest request) { return service.update_job(std::move(request)); },
                       [attribute_registry](JobDefinition const& job) { return job_to_json(job, *attribute_registry); },
                       mutation_committed);
               }) &&
           server.register_method(
               std::string{management_methods[11]},
               [&service, attribute_registry, mutation_committed](
                   jb::rpc::RequestContext const&,
                   std::optional<jb::core::JsonValue> const& params) -> jb::rpc::MethodResult {
                   return handle_value(
                       params,
                       [](jb::core::JsonValue const& value) { return job_id_from_json(value); },
                       [&service](jb::core::Uuid id) { return service.suspend_job(id); },
                       [attribute_registry](JobDefinition const& job) { return job_to_json(job, *attribute_registry); },
                       mutation_committed);
               }) &&
           server.register_method(
               std::string{management_methods[12]},
               [&service, attribute_registry, mutation_committed](
                   jb::rpc::RequestContext const&,
                   std::optional<jb::core::JsonValue> const& params) -> jb::rpc::MethodResult {
                   return handle_value(
                       params,
                       [](jb::core::JsonValue const& value) { return job_id_from_json(value); },
                       [&service](jb::core::Uuid id) { return service.resume_job(id); },
                       [attribute_registry](JobDefinition const& job) { return job_to_json(job, *attribute_registry); },
                       mutation_committed);
               }) &&
           server.register_method(
               std::string{management_methods[13]},
               [&service, attribute_registry, mutation_committed](
                   jb::rpc::RequestContext const&,
                   std::optional<jb::core::JsonValue> const& params) -> jb::rpc::MethodResult {
                   return handle_value(
                       params,
                       [](jb::core::JsonValue const& value) { return move_job_request_from_json(value); },
                       [&service](MoveJobRequest const& request) { return service.move_job(request); },
                       [attribute_registry](JobDefinition const& job) { return job_to_json(job, *attribute_registry); },
                       mutation_committed);
               }) &&
           server.register_method(
               std::string{management_methods[14]},
               [&service, mutation_committed = std::move(mutation_committed)](
                   jb::rpc::RequestContext const&,
                   std::optional<jb::core::JsonValue> const& params) -> jb::rpc::MethodResult {
                   return handle_void(
                       params,
                       [](jb::core::JsonValue const& value) { return delete_job_request_from_json(value); },
                       [&service](DeleteJobRequest const& request) { return service.delete_job(request); },
                       mutation_committed);
               });
}

} // namespace jb::jobu
