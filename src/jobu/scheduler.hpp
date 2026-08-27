/** @file scheduler.hpp
 * @brief Defines public scheduler operation results.
 */
#pragma once

#include "run.hpp"

#include <cstdint>

namespace jb::jobu {

/** Describes whether run cancellation completed durably or was delegated to an active executor. */
enum class CancelDisposition : std::uint8_t {
    /** The cancellation transaction committed and `CancelRunResult::run` is the terminal cancelled snapshot. */
    Completed,
    /** The active executor accepted cancellation and `CancelRunResult::run` remains a running snapshot. */
    Requested,
};

/** Owning result of cancelling one non-terminal run.
 *
 * `Completed` means the run is durably terminal, any required recurring successor and suspension drains committed in
 * the same transaction, and capacity may be reused. `Requested` means the executor still owns an active attempt; the
 * scheduler retains its capacity until the executor reports completion and the forced cancelled outcome commits.
 */
struct CancelRunResult {
    /// Owning run snapshot at the cancellation operation's durable boundary.
    JobRun            run;
    /// Whether cancellation completed synchronously or awaits executor completion.
    CancelDisposition disposition{CancelDisposition::Completed};
};

} // namespace jb::jobu
