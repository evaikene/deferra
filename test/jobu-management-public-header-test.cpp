#include "management.hpp"

#include <type_traits>

static_assert(std::is_base_of_v<jb::core::Object, jb::jobu::ManagementService>);
static_assert(std::is_same_v<decltype(jb::jobu::ManagementService::mutation_committed), jb::core::Signal<>>);
static_assert(!std::is_copy_constructible_v<jb::jobu::ManagementService>);
static_assert(!std::is_move_constructible_v<jb::jobu::ManagementService>);
static_assert(std::is_constructible_v<jb::jobu::ManagementService,
                                      jb::db::Database&,
                                      jb::jobu::AttributeRegistry const&,
                                      jb::jobu::CronEngine const&,
                                      jb::core::UuidGenerator&,
                                      jb::core::TimeSource&>);
static_assert(std::is_constructible_v<jb::jobu::ManagementService,
                                      jb::db::Database&,
                                      jb::jobu::AttributeRegistry const&,
                                      jb::jobu::CronEngine const&,
                                      jb::core::UuidGenerator&,
                                      jb::core::TimeSource&,
                                      jb::jobu::AttributeSet,
                                      jb::core::Object*>);
static_assert(std::is_same_v<jb::jobu::QueueSelector, std::variant<jb::core::Uuid, std::string>>);
static_assert(std::is_same_v<decltype(jb::jobu::JobPage::items), std::vector<jb::jobu::JobDefinition>>);
static_assert(std::is_same_v<decltype(jb::jobu::MoveJobRequest::job_id), jb::core::Uuid>);
static_assert(std::is_same_v<decltype(jb::jobu::MoveJobRequest::expected_revision), jb::jobu::JobRevision>);
static_assert(std::is_same_v<decltype(jb::jobu::MoveJobRequest::target_queue), jb::jobu::QueueSelector>);
static_assert(std::is_same_v<decltype(jb::jobu::DeleteJobRequest::job_id), jb::core::Uuid>);
static_assert(std::is_same_v<decltype(jb::jobu::DeleteJobRequest::expected_revision), jb::jobu::JobRevision>);
static_assert(std::is_same_v<decltype(jb::jobu::RunNowRequest::job_id), jb::core::Uuid>);
static_assert(std::is_same_v<decltype(jb::jobu::RunNowRequest::idempotency_key), std::optional<std::string>>);
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
static_assert(std::is_same_v<decltype(&jb::jobu::ManagementService::delete_queue),
                             jb::core::Result<void, jb::core::Error> (jb::jobu::ManagementService::*)(
                                 jb::jobu::QueueSelector const&)>);
static_assert(std::is_same_v<decltype(&jb::jobu::ManagementService::create_job),
                             jb::core::Result<jb::jobu::JobDefinition, jb::core::Error> (
                                 jb::jobu::ManagementService::*)(jb::jobu::CreateJobRequest)>);
static_assert(std::is_same_v<decltype(&jb::jobu::ManagementService::update_job),
                             jb::core::Result<jb::jobu::JobDefinition, jb::core::Error> (
                                 jb::jobu::ManagementService::*)(jb::jobu::UpdateJobRequest)>);
static_assert(std::is_same_v<decltype(&jb::jobu::ManagementService::run_now),
                             jb::core::Result<jb::jobu::JobRun, jb::core::Error> (jb::jobu::ManagementService::*)(
                                 jb::jobu::RunNowRequest)>);
static_assert(std::is_same_v<decltype(&jb::jobu::ManagementService::suspend_job),
                             jb::core::Result<jb::jobu::JobDefinition, jb::core::Error> (
                                 jb::jobu::ManagementService::*)(jb::core::Uuid const&)>);
static_assert(std::is_same_v<decltype(&jb::jobu::ManagementService::resume_job),
                             jb::core::Result<jb::jobu::JobDefinition, jb::core::Error> (
                                 jb::jobu::ManagementService::*)(jb::core::Uuid const&)>);
static_assert(std::is_same_v<decltype(&jb::jobu::ManagementService::move_job),
                             jb::core::Result<jb::jobu::JobDefinition, jb::core::Error> (
                                 jb::jobu::ManagementService::*)(jb::jobu::MoveJobRequest const&)>);
static_assert(std::is_same_v<decltype(&jb::jobu::ManagementService::delete_job),
                             jb::core::Result<void, jb::core::Error> (jb::jobu::ManagementService::*)(
                                 jb::jobu::DeleteJobRequest const&)>);

int main()
{
    jb::jobu::PageOptions const      page;
    jb::jobu::CreateJobRequest const create_request;
    jb::jobu::UpdateJobRequest const update_request;
    jb::jobu::MoveJobRequest const   move_request;
    jb::jobu::DeleteJobRequest const delete_request;
    jb::jobu::RunNowRequest const    run_now_request;
    return page.limit == 100 && create_request.type == jb::jobu::JobType::Cli &&
                   update_request.expected_revision == 0 && move_request.expected_revision == 0 &&
                   delete_request.expected_revision == 0 && !run_now_request.idempotency_key
             ? 0
             : 1;
}
