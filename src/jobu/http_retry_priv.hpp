#pragma once

#include "attempt_executor.hpp"
#include "attribute.hpp"
#include "error.hpp"
#include "http_client.hpp"
#include "http_job_payload_priv.hpp"
#include "json.hpp"
#include "result.hpp"
#include "time_source.hpp"

#include <optional>

namespace jb::jobu::detail {

struct HttpCompletionPolicy {
    AttemptOutcome                        outcome{AttemptOutcome::Failed};
    std::optional<FailureDisposition>     failure_disposition;
    std::optional<jb::core::UtcTimePoint> retry_not_before;
    jb::core::JsonValue                   result;
};

[[nodiscard]] auto map_http_completion(jb::net::HttpCompletionResult const& transfer,
                                       HttpStatusSet const&                 expected_statuses,
                                       AttributeSet const&                  attributes,
                                       jb::core::UtcTimePoint               completed_at)
    -> jb::core::Result<HttpCompletionPolicy, jb::core::Error>;

} // namespace jb::jobu::detail
