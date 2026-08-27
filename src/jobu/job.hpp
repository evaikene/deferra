/** @file job.hpp
 * @brief Defines persistent JobU job definitions and schedule values.
 */
#pragma once

#include "attribute.hpp"
#include "json.hpp"
#include "time_source.hpp"
#include "uuid.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <variant>

namespace jb::jobu {

/// Optimistic-concurrency revision of a job definition.
using JobRevision = std::uint64_t;

/// Durable lifecycle state of a job definition.
enum class JobState : std::uint8_t {
    Active,     ///< The definition may produce eligible work.
    Suspending, ///< Suspension is waiting for running work to finish.
    Suspended,  ///< The definition cannot produce eligible work.
    Deleted,    ///< The definition is soft-deleted and retained only for history.
};

/// Runner family selected by a job definition and copied into each run snapshot.
enum class JobType : std::uint8_t {
    Cli,  ///< Execute a local command through the future CLI runner.
    Http, ///< Execute a request through the future HTTP runner.
};

/// Schedule for exactly one planned occurrence.
struct OnceSchedule {
    /// Planned execution time in UTC.
    jb::core::UtcTimePoint planned_at;
};

/** Recurring schedule evaluated in one IANA timezone.
 *
 * Management creation validates both fields through its borrowed CronEngine before persisting the definition.
 */
struct CronSchedule {
    /// Five-field cron expression interpreted using JobU's documented calendar rules.
    std::string expression;
    /// IANA timezone name, defaulting to UTC.
    std::string timezone{"UTC"};
};

/// Schedule representation stored by a job definition.
using JobSchedule = std::variant<OnceSchedule, CronSchedule>;

/** Persistent job definition managed through optimistic revisions.
 *
 * Attributes are complete and materialized when the definition is persisted. Payload is an owning project JSON
 * object whose runner-specific structural validation is performed by management operations.
 */
struct JobDefinition {
    /// Stable definition identity.
    jb::core::Uuid                        id;
    /// Queue that currently owns the definition.
    jb::core::Uuid                        queue_id;
    /// Positive optimistic-concurrency revision.
    JobRevision                           revision{1};
    /// Optional non-unique display name.
    std::optional<std::string>            name;
    /// Current lifecycle state.
    JobState                              state{JobState::Active};
    /// Runner family copied into run snapshots.
    JobType                               type{JobType::Cli};
    /// One-time or recurring schedule owned by this definition.
    JobSchedule                           schedule;
    /// Signed scheduling priority.
    std::int32_t                          priority{0};
    /// Complete materialized attribute set.
    AttributeSet                          attributes;
    /// Owning runner payload JSON object.
    jb::core::JsonValue                   payload;
    /// Creation time in UTC.
    jb::core::UtcTimePoint                created_at;
    /// Time of the most recent durable mutation in UTC.
    jb::core::UtcTimePoint                updated_at;
    /// Soft-deletion time in UTC, or no value for a non-deleted definition.
    std::optional<jb::core::UtcTimePoint> deleted_at;
};

} // namespace jb::jobu
