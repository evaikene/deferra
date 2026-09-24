#include "control_client_priv.hpp"

#include "control_json.hpp"
#include "history_json.hpp"
#include "management_json.hpp"
#include "secret_json.hpp"
#include "statistics_json.hpp"

#include "json.hpp"

#include <optional>
#include <set>
#include <string>
#include <utility>
#include <variant>

namespace jb::jobu {
namespace {

auto client_error(jb::core::ErrorCategory category, std::string_view code, std::string_view message) -> jb::core::Error
{
    return {
        .category = category,
        .code     = std::string{code},
        .message  = std::string{message},
    };
}

auto invalid_response_error() -> jb::core::Error
{
    return client_error(jb::core::ErrorCategory::InvalidArgument,
                        "jobu.client.invalid_response",
                        "JobU response is invalid");
}

template <typename T>
auto decoded_reply(jb::core::Result<T, jb::core::Error> decoded) -> std::optional<ControlReply>
{
    if (!decoded) {
        return std::nullopt;
    }
    return ControlReply{std::move(decoded).value()};
}

auto local_failure(jb::core::Error error, bool outcome_unknown) -> ControlFailure
{
    return {
        .kind            = ControlFailureKind::Local,
        .error           = std::move(error),
        .outcome_unknown = outcome_unknown,
    };
}

auto remote_failure(jb::rpc::RpcError error) -> ControlFailure
{
    return {
        .kind            = ControlFailureKind::Remote,
        .error           = std::move(error),
        .outcome_unknown = false,
    };
}

} // anonymous namespace

auto ControlClient::Private::is_mutation(Method method) noexcept -> bool
{
    switch (method) {
        case Method::CreateQueue:
        case Method::UpdateQueue:
        case Method::SuspendQueue:
        case Method::ResumeQueue:
        case Method::DeleteQueue:
        case Method::CreateJob:
        case Method::UpdateJob:
        case Method::SuspendJob:
        case Method::ResumeJob:
        case Method::MoveJob:
        case Method::DeleteJob:
        case Method::RunNow:
        case Method::CancelRun:
        case Method::SetSecret:
        case Method::DeleteSecret:
            return true;
        default:
            return false;
    }
}

void ControlClient::Private::deliver_one(ControlCallId id, Pending call)
{
    if (auto* remote = std::get_if<jb::rpc::RpcError>(&call.outcome)) {
        if (call.method == Method::Handshake) {
            fail_handshake(client_error(jb::core::ErrorCategory::Unavailable,
                                        "jobu.client.handshake_failed",
                                        "JobU handshake failed"));
        }
        else {
            owner->emit(owner->call_failed, id, remote_failure(std::move(*remote)));
        }
        return;
    }

    auto const* value = std::get_if<jb::core::JsonValue>(&call.outcome);
    if (value == nullptr) {
        return;
    }

    if (call.method == Method::Handshake) {
        auto decoded = system_info_from_json(*value);
        if (!decoded) {
            fail_handshake(invalid_response_error());
            return;
        }
        if (decoded->api_version.major != 1U) {
            fail_handshake(client_error(jb::core::ErrorCategory::Unsupported,
                                        "jobu.client.unsupported_api",
                                        "JobU API major version is unsupported"));
            return;
        }

        capabilities = std::set<std::string>{decoded->capabilities.begin(), decoded->capabilities.end()};
        phase        = Phase::Ready;
        owner->emit(owner->ready, std::move(decoded).value());
        return;
    }

    // Decode the selected method only; another method's compatible JSON shape must not change its reply type.
    auto decoded = [&]() -> std::optional<ControlReply> {
        switch (call.method) {
            case Method::Handshake:
                return std::nullopt;
            case Method::SystemInfo:
                return decoded_reply(system_info_from_json(*value));
            case Method::SystemStats:
            case Method::QueueStats:
                return decoded_reply(statistics_page_from_json(*value));
            case Method::CreateQueue:
            case Method::GetQueue:
            case Method::UpdateQueue:
            case Method::SuspendQueue:
            case Method::ResumeQueue:
                return decoded_reply(queue_from_json(*value, attributes));
            case Method::ListQueues:
                return decoded_reply(queue_page_from_json(*value, attributes));
            case Method::CreateJob:
            case Method::GetJob:
            case Method::UpdateJob:
            case Method::SuspendJob:
            case Method::ResumeJob:
            case Method::MoveJob:
                return decoded_reply(job_from_json(*value, attributes));
            case Method::ListJobs:
                return decoded_reply(job_page_from_json(*value, attributes));
            case Method::RunNow:
            case Method::GetRun:
                return decoded_reply(run_details_from_json(*value, attributes));
            case Method::ListRuns:
                return decoded_reply(run_page_from_json(*value));
            case Method::CancelRun:
                return decoded_reply(cancel_run_result_from_json(*value, attributes));
            case Method::GetAttempt:
                return decoded_reply(attempt_details_from_json(*value));
            case Method::ListAttempts:
                return decoded_reply(attempt_page_from_json(*value));
            case Method::ReadAttemptOutput:
                return decoded_reply(attempt_output_chunk_from_json(*value));
            case Method::SetSecret:
                return decoded_reply(secret_metadata_from_json(*value));
            case Method::ListSecrets:
                return decoded_reply(secret_page_from_json(*value));
            case Method::ValidateSchedule: {
                auto valid = schedule_validate_result_from_json(*value);
                if (!valid) {
                    return std::nullopt;
                }
                return ControlReply{ScheduleValidationReply{}};
            }
            case Method::NextSchedule: {
                auto occurrences = schedule_next_result_from_json(*value);
                if (!occurrences) {
                    return std::nullopt;
                }
                return ControlReply{ScheduleNextReply{.occurrences = std::move(occurrences).value()}};
            }
            case Method::DeleteQueue:
            case Method::DeleteJob:
            case Method::DeleteSecret:
                if (std::holds_alternative<jb::core::JsonNull>(value->data)) {
                    return ControlReply{EmptyReply{}};
                }
                return std::nullopt;
        }
        return std::nullopt;
    }();

    if (decoded) {
        owner->emit(owner->reply_received, id, *decoded);
    }
    else {
        owner->emit(owner->call_failed, id, local_failure(invalid_response_error(), is_mutation(call.method)));
    }
}

} // namespace jb::jobu
