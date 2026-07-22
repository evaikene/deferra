#include "management_json.hpp"

#include <type_traits>

using JsonResult = jb::core::Result<jb::rpc::JsonValue, jb::core::Error>;

static_assert(std::is_same_v<decltype(&jb::jobu::queue_to_json),
                             JsonResult (*)(jb::jobu::Queue const&, jb::jobu::AttributeRegistry const&)>);
static_assert(
    std::is_same_v<decltype(&jb::jobu::queue_selector_from_json),
                   jb::core::Result<jb::jobu::QueueSelector, jb::core::Error> (*)(jb::rpc::JsonValue const&)>);
static_assert(std::is_same_v<
              decltype(&jb::jobu::create_queue_request_from_json),
              jb::core::Result<jb::jobu::CreateQueueRequest, jb::core::Error> (*)(jb::rpc::JsonValue const&,
                                                                                  jb::jobu::AttributeRegistry const&)>);
static_assert(std::is_same_v<decltype(&jb::jobu::update_queue_request_to_json),
                             JsonResult (*)(jb::jobu::UpdateQueueRequest const&, jb::jobu::AttributeRegistry const&)>);
static_assert(
    std::is_same_v<decltype(&jb::jobu::job_from_json),
                   jb::core::Result<jb::jobu::JobDefinition, jb::core::Error> (*)(jb::rpc::JsonValue const&,
                                                                                  jb::jobu::AttributeRegistry const&)>);
static_assert(std::is_same_v<decltype(&jb::jobu::create_job_request_to_json),
                             JsonResult (*)(jb::jobu::CreateJobRequest const&, jb::jobu::AttributeRegistry const&)>);
static_assert(std::is_same_v<
              decltype(&jb::jobu::update_job_request_from_json),
              jb::core::Result<jb::jobu::UpdateJobRequest, jb::core::Error> (*)(jb::rpc::JsonValue const&,
                                                                                jb::jobu::AttributeRegistry const&)>);
static_assert(
    std::is_same_v<decltype(&jb::jobu::move_job_request_to_json), JsonResult (*)(jb::jobu::MoveJobRequest const&)>);

auto main() -> int
{
    return 0;
}
