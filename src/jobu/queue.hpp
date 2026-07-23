/** @file queue.hpp
 * @brief Defines the persistent JobU queue domain value.
 */
#pragma once

#include "attribute.hpp"
#include "time_source.hpp"
#include "uuid.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace jb::jobu {

/// Durable lifecycle state of a queue.
enum class QueueState : std::uint8_t {
    Active,     ///< New work may become eligible in the queue.
    Suspending, ///< Suspension is waiting for running work to finish.
    Suspended,  ///< No work in the queue may become eligible.
    Deleted,    ///< The queue is soft-deleted and retained only for history.
};

/// Policy applied to attempts found running during daemon recovery.
enum class RecoveryPolicy : std::uint8_t {
    FailInterrupted,  ///< Finish interrupted work without retrying it.
    RetryInterrupted, ///< Retry interrupted work when its attempt policy permits.
};

/** Persistent queue configuration and lifecycle state.
 *
 * A missing history_retention inherits the daemon policy, zero means unlimited retention, and a positive value is a
 * queue-specific duration. Deleted queues expose their original user-facing name; any internal uniqueness rewrite is
 * a private storage detail.
 */
struct Queue {

    /// Default scheduler weight for newly constructed queues.
    static constexpr std::uint32_t kDefaultWeight           = 1;
    /// Default combined concurrency limit for newly constructed queues.
    static constexpr std::uint32_t kDefaultConcurrencyLimit = 1;
    /// Default runnable-work warning delay in milliseconds.
    static constexpr long          kDefaultRunnableWaitTime = 10000;

    /// Stable queue identity.
    jb::core::Uuid                        id;
    /// User-facing queue name.
    std::string                           name;
    /// Current lifecycle state.
    QueueState                            state{QueueState::Active};
    /// Positive scheduler weight.
    std::uint32_t                         weight{kDefaultWeight};
    /// Positive combined concurrency limit.
    std::uint32_t                         concurrency_limit{kDefaultConcurrencyLimit};
    /// Startup recovery behavior for interrupted attempts.
    RecoveryPolicy                        recovery_policy{RecoveryPolicy::FailInterrupted};
    /// Partial queue-default attribute values applied only to newly created jobs.
    AttributeSet                          defaults;
    /// Queue-specific history retention, zero for unlimited, or no value to inherit the daemon policy.
    std::optional<std::chrono::seconds>   history_retention;
    /// Delay after which runnable work should produce a warning.
    std::chrono::milliseconds             runnable_wait_warning{kDefaultRunnableWaitTime};
    /// Creation time in UTC.
    jb::core::UtcTimePoint                created_at;
    /// Time of the most recent durable mutation in UTC.
    jb::core::UtcTimePoint                updated_at;
    /// Soft-deletion time in UTC, or no value for a non-deleted queue.
    std::optional<jb::core::UtcTimePoint> deleted_at;
};

} // namespace jb::jobu
