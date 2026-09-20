#pragma once

#include <cstdint>

namespace jb::jobud::detail {

/// Deterministic signal/retirement boundaries, compiled only into the subprocess test helper.
enum class SignalTestCheckpoint : std::uint8_t {
    HandlerEntered,
    DescriptorAcquired,
    DescriptorWritten,
    RetirementWaiting,
};

/// Supplied by the helper. Handler checkpoints must use only lock-free atomic operations.
void signal_test_checkpoint(SignalTestCheckpoint checkpoint) noexcept;

} // namespace jb::jobud::detail
