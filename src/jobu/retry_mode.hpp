/// @file retry_mode.hpp
/// @brief Defines how a scheduled retry uses queue capacity during its wait.
#pragma once

#include <cstdint>

namespace jb::jobu {

/// Queue-capacity policy selected by the existing retry policy.
/// A retry still obeys its persisted runnable time and execution eligibility gates in either mode.
enum class RetryMode : std::uint8_t {
    /// Retain the queue slot during retry wait while the job and queue permit execution.
    Blocking,
    /// Release the queue slot during retry wait so other eligible work may use it.
    Reschedule,
};

} // namespace jb::jobu
