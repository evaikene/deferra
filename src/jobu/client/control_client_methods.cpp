#include "control_client.hpp"

#include "control_client_priv.hpp"

#include "control_json.hpp"
#include "history_json.hpp"
#include "management_json.hpp"
#include "secret_json.hpp"
#include "statistics_json.hpp"

#include <optional>
#include <string_view>

namespace jb::jobu {

using CallResult = jb::core::Result<ControlCallId, jb::core::Error>;

auto ControlClient::get_system_info(ControlCallOptions options) -> CallResult
{
    return d_ptr<Private>()->start_call(Private::Method::SystemInfo, "system.info", std::nullopt, options);
}

auto ControlClient::system_statistics(StatisticsListRequest const& request, ControlCallOptions options) -> CallResult
{
    return d_ptr<Private>()->start_encoded_call(Private::Method::SystemStats,
                                                "system.stats",
                                                system_statistics_request_to_json(request),
                                                options);
}

auto ControlClient::create_queue(CreateQueueRequest const& request, ControlCallOptions options) -> CallResult
{
    auto* data = d_ptr<Private>();
    return data->start_encoded_call(Private::Method::CreateQueue,
                                    "queue.create",
                                    create_queue_request_to_json(request, data->attributes),
                                    options);
}

auto ControlClient::get_queue(QueueSelector const& selector, ControlCallOptions options) -> CallResult
{
    return d_ptr<Private>()->start_encoded_call(Private::Method::GetQueue,
                                                "queue.get",
                                                queue_selector_to_json(selector),
                                                options);
}

auto ControlClient::list_queues(QueueListRequest const& request, ControlCallOptions options) -> CallResult
{
    return d_ptr<Private>()->start_encoded_call(Private::Method::ListQueues,
                                                "queue.list",
                                                queue_list_request_to_json(request),
                                                options);
}

auto ControlClient::update_queue(UpdateQueueRequest const& request, ControlCallOptions options) -> CallResult
{
    auto* data = d_ptr<Private>();
    return data->start_encoded_call(Private::Method::UpdateQueue,
                                    "queue.update",
                                    update_queue_request_to_json(request, data->attributes),
                                    options);
}

auto ControlClient::suspend_queue(QueueSelector const& selector, ControlCallOptions options) -> CallResult
{
    return d_ptr<Private>()->start_encoded_call(Private::Method::SuspendQueue,
                                                "queue.suspend",
                                                queue_selector_to_json(selector),
                                                options);
}

auto ControlClient::resume_queue(QueueSelector const& selector, ControlCallOptions options) -> CallResult
{
    return d_ptr<Private>()->start_encoded_call(Private::Method::ResumeQueue,
                                                "queue.resume",
                                                queue_selector_to_json(selector),
                                                options);
}

auto ControlClient::delete_queue(QueueSelector const& selector, ControlCallOptions options) -> CallResult
{
    return d_ptr<Private>()->start_encoded_call(Private::Method::DeleteQueue,
                                                "queue.delete",
                                                queue_selector_to_json(selector),
                                                options);
}

auto ControlClient::queue_statistics(QueueStatisticsListRequest const& request, ControlCallOptions options)
    -> CallResult
{
    return d_ptr<Private>()->start_encoded_call(Private::Method::QueueStats,
                                                "queue.stats",
                                                queue_statistics_request_to_json(request),
                                                options);
}

auto ControlClient::create_job(CreateJobRequest const& request, ControlCallOptions options) -> CallResult
{
    auto* data = d_ptr<Private>();
    return data->start_encoded_call(Private::Method::CreateJob,
                                    "job.create",
                                    create_job_request_to_json(request, data->attributes),
                                    options);
}

auto ControlClient::get_job(jb::core::Uuid const& id, ControlCallOptions options) -> CallResult
{
    return d_ptr<Private>()->start_encoded_call(Private::Method::GetJob, "job.get", job_id_to_json(id), options);
}

auto ControlClient::list_jobs(JobListRequest const& request, ControlCallOptions options) -> CallResult
{
    return d_ptr<Private>()->start_encoded_call(Private::Method::ListJobs,
                                                "job.list",
                                                job_list_request_to_json(request),
                                                options);
}

auto ControlClient::update_job(UpdateJobRequest const& request, ControlCallOptions options) -> CallResult
{
    auto* data = d_ptr<Private>();
    return data->start_encoded_call(Private::Method::UpdateJob,
                                    "job.update",
                                    update_job_request_to_json(request, data->attributes),
                                    options);
}

auto ControlClient::suspend_job(jb::core::Uuid const& id, ControlCallOptions options) -> CallResult
{
    return d_ptr<Private>()->start_encoded_call(Private::Method::SuspendJob,
                                                "job.suspend",
                                                job_id_to_json(id),
                                                options);
}

auto ControlClient::resume_job(jb::core::Uuid const& id, ControlCallOptions options) -> CallResult
{
    return d_ptr<Private>()->start_encoded_call(Private::Method::ResumeJob, "job.resume", job_id_to_json(id), options);
}

auto ControlClient::move_job(MoveJobRequest const& request, ControlCallOptions options) -> CallResult
{
    return d_ptr<Private>()->start_encoded_call(Private::Method::MoveJob,
                                                "job.move",
                                                move_job_request_to_json(request),
                                                options);
}

auto ControlClient::delete_job(DeleteJobRequest const& request, ControlCallOptions options) -> CallResult
{
    return d_ptr<Private>()->start_encoded_call(Private::Method::DeleteJob,
                                                "job.delete",
                                                delete_job_request_to_json(request),
                                                options);
}

auto ControlClient::run_now(RunNowRequest const& request, ControlCallOptions options) -> CallResult
{
    return d_ptr<Private>()->start_encoded_call(Private::Method::RunNow,
                                                "job.run_now",
                                                run_now_request_to_json(request),
                                                options);
}

auto ControlClient::get_run(jb::core::Uuid const& id, ControlCallOptions options) -> CallResult
{
    return d_ptr<Private>()->start_encoded_call(Private::Method::GetRun,
                                                "run.get",
                                                run_get_request_to_json(id),
                                                options);
}

auto ControlClient::list_runs(RunListRequest const& request, ControlCallOptions options) -> CallResult
{
    return d_ptr<Private>()->start_encoded_call(Private::Method::ListRuns,
                                                "run.list",
                                                run_list_request_to_json(request),
                                                options);
}

auto ControlClient::cancel_run(jb::core::Uuid const& id, ControlCallOptions options) -> CallResult
{
    return d_ptr<Private>()->start_encoded_call(Private::Method::CancelRun,
                                                "run.cancel",
                                                cancel_run_request_to_json(id),
                                                options);
}

auto ControlClient::get_attempt(AttemptKey const& key, ControlCallOptions options) -> CallResult
{
    return d_ptr<Private>()->start_encoded_call(Private::Method::GetAttempt,
                                                "attempt.get",
                                                attempt_get_request_to_json(key),
                                                options);
}

auto ControlClient::list_attempts(AttemptListRequest const& request, ControlCallOptions options) -> CallResult
{
    return d_ptr<Private>()->start_encoded_call(Private::Method::ListAttempts,
                                                "attempt.list",
                                                attempt_list_request_to_json(request),
                                                options);
}

auto ControlClient::read_attempt_output(AttemptOutputRequest const& request, ControlCallOptions options) -> CallResult
{
    return d_ptr<Private>()->start_encoded_call(Private::Method::ReadAttemptOutput,
                                                "attempt.output",
                                                attempt_output_request_to_json(request),
                                                options);
}

auto ControlClient::set_secret(SetSecretRequest const& request, ControlCallOptions options) -> CallResult
{
    return d_ptr<Private>()->start_encoded_call(Private::Method::SetSecret,
                                                "secret.set",
                                                set_secret_request_to_json(request),
                                                options);
}

auto ControlClient::list_secrets(SecretListRequest const& request, ControlCallOptions options) -> CallResult
{
    return d_ptr<Private>()->start_encoded_call(Private::Method::ListSecrets,
                                                "secret.list",
                                                secret_list_request_to_json(request),
                                                options);
}

auto ControlClient::delete_secret(std::string_view name, ControlCallOptions options) -> CallResult
{
    return d_ptr<Private>()->start_encoded_call(Private::Method::DeleteSecret,
                                                "secret.delete",
                                                secret_delete_request_to_json(name),
                                                options);
}

auto ControlClient::validate_schedule(CronSchedule const& schedule, ControlCallOptions options) -> CallResult
{
    return d_ptr<Private>()->start_encoded_call(Private::Method::ValidateSchedule,
                                                "schedule.validate",
                                                schedule_validate_request_to_json(schedule),
                                                options);
}

auto ControlClient::next_schedule_occurrences(ScheduleNextRequest const& request, ControlCallOptions options)
    -> CallResult
{
    return d_ptr<Private>()->start_encoded_call(Private::Method::NextSchedule,
                                                "schedule.next",
                                                schedule_next_request_to_json(request),
                                                options);
}

} // namespace jb::jobu
