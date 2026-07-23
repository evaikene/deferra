/** @file cron.hpp
 * @brief Defines dependency-independent cron validation and occurrence calculation.
 */
#pragma once

#include "error.hpp"
#include "job.hpp"
#include "result.hpp"
#include "time_source.hpp"

namespace jb::jobu {

/** Validates recurring schedules and calculates their UTC occurrences.
 *
 * Implementations do not retain references to schedules supplied to member functions. An implementation may impose
 * an owner-thread requirement, which its concrete class must document.
 */
class CronEngine {
public:
    /// Destroys a cron engine through its interface.
    virtual ~CronEngine() = default;

    /** Validates a complete recurring schedule.
     *
     * Validation covers both the five-field expression and the timezone name. The supplied strings are borrowed only
     * for the duration of the call and are not retained.
     *
     * @param schedule Schedule to validate.
     * @return Success when the complete schedule is usable, or a stable schedule error.
     */
    [[nodiscard]] virtual auto validate(CronSchedule const& schedule) const
        -> jb::core::Result<void, jb::core::Error> = 0;

    /** Finds the first occurrence strictly after a UTC instant.
     *
     * The supplied schedule is borrowed only for the duration of the call and is not retained. Returned occurrences
     * are aligned to a UTC minute after the implementation applies the schedule's timezone rules.
     *
     * @param schedule Complete recurring schedule to evaluate.
     * @param exclusive_lower_bound UTC instant that the returned occurrence must exceed.
     * @return The next representable UTC occurrence, or a stable schedule error.
     */
    [[nodiscard]] virtual auto next_after(CronSchedule const&    schedule,
                                          jb::core::UtcTimePoint exclusive_lower_bound) const
        -> jb::core::Result<jb::core::UtcTimePoint, jb::core::Error> = 0;
};

} // namespace jb::jobu
