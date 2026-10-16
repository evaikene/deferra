#pragma once

#include "execution_events.hpp"

namespace jb::jobud::detail {

/// Emit payload-free execution diagnostics from the scheduler's owning values.
/// Formatting borrows no scheduler, database or runner state. All temporary strings live
/// through synchronous emission; sinks retaining records must copy them before returning.
/// Unrepresentable UTC fields are omitted rather than invented. Interrupted recovery
/// outcomes are outside this live stream and do not produce an execution record.
void log_attempt_started(jb::jobu::AttemptStarted const& event);
void log_attempt_finished(jb::jobu::AttemptFinished const& event);
void log_retry_scheduled(jb::jobu::RetryScheduled const& event);
void log_run_finished(jb::jobu::RunFinished const& event);

} // namespace jb::jobud::detail
