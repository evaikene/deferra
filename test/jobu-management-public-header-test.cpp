#include "management.hpp"

#include <type_traits>

static_assert(!std::is_copy_constructible_v<jb::jobu::ManagementService>);
static_assert(!std::is_move_constructible_v<jb::jobu::ManagementService>);
static_assert(std::is_same_v<jb::jobu::QueueSelector, std::variant<jb::core::Uuid, std::string>>);
static_assert(std::is_same_v<decltype(jb::jobu::JobPage::items), std::vector<jb::jobu::JobDefinition>>);
static_assert(std::is_same_v<decltype(&jb::jobu::ManagementService::create_queue),
                             jb::core::Result<jb::jobu::Queue, jb::core::Error> (jb::jobu::ManagementService::*)(
                                 jb::jobu::CreateQueueRequest)>);
static_assert(std::is_same_v<decltype(&jb::jobu::ManagementService::update_queue),
                             jb::core::Result<jb::jobu::Queue, jb::core::Error> (jb::jobu::ManagementService::*)(
                                 jb::jobu::UpdateQueueRequest)>);
static_assert(std::is_same_v<decltype(&jb::jobu::ManagementService::suspend_queue),
                             jb::core::Result<jb::jobu::Queue, jb::core::Error> (jb::jobu::ManagementService::*)(
                                 jb::jobu::QueueSelector const&)>);
static_assert(std::is_same_v<decltype(&jb::jobu::ManagementService::resume_queue),
                             jb::core::Result<jb::jobu::Queue, jb::core::Error> (jb::jobu::ManagementService::*)(
                                 jb::jobu::QueueSelector const&)>);
static_assert(std::is_same_v<decltype(&jb::jobu::ManagementService::create_job),
                             jb::core::Result<jb::jobu::JobDefinition, jb::core::Error> (
                                 jb::jobu::ManagementService::*)(jb::jobu::CreateJobRequest)>);
static_assert(std::is_same_v<decltype(&jb::jobu::ManagementService::update_job),
                             jb::core::Result<jb::jobu::JobDefinition, jb::core::Error> (
                                 jb::jobu::ManagementService::*)(jb::jobu::UpdateJobRequest)>);
static_assert(std::is_same_v<decltype(&jb::jobu::ManagementService::suspend_job),
                             jb::core::Result<jb::jobu::JobDefinition, jb::core::Error> (
                                 jb::jobu::ManagementService::*)(jb::core::Uuid const&)>);
static_assert(std::is_same_v<decltype(&jb::jobu::ManagementService::resume_job),
                             jb::core::Result<jb::jobu::JobDefinition, jb::core::Error> (
                                 jb::jobu::ManagementService::*)(jb::core::Uuid const&)>);

int main()
{
    jb::jobu::PageOptions const      page;
    jb::jobu::CreateJobRequest const create_request;
    jb::jobu::UpdateJobRequest const update_request;
    return page.limit == 100 && create_request.type == jb::jobu::JobType::Cli && update_request.expected_revision == 0
             ? 0
             : 1;
}
