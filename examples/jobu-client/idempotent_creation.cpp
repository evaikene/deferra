#include "control_client.hpp"

#include <string>
#include <utility>

namespace example {

// This separate opt-in operation is never called by the read-only example's main().
auto submit_once_now(jb::jobu::ControlClient& client,
                     jb::jobu::QueueSelector  queue,
                     jb::core::JsonValue      payload,
                     std::string user_selected_key) -> jb::core::Result<jb::jobu::ControlCallId, jb::core::Error>
{
    auto request = jb::jobu::CreateJobRequest{
        .queue           = std::move(queue),
        .schedule        = jb::jobu::ImmediateSchedule{},
        .payload         = std::move(payload),
        .idempotency_key = std::move(user_selected_key),
    };
    return client.create_job(request);
}

} // namespace example
