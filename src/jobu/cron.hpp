/** @file cron.hpp
 * @brief Defines dependency-independent cron validation and occurrence calculation.
 */
#pragma once

#include "error.hpp"
#include "job.hpp"
#include "result.hpp"
#include "time_source.hpp"

#include <cstddef>
#include <memory>
#include <vector>

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

/** Calculates cron occurrences from the operating system's IANA timezone data.
 *
 * The engine lazily loads platform TZif files and keeps immutable data cached by the exact validated timezone name.
 * `UTC` is handled as a fixed zero offset without filesystem access. Cached timezone data is not reloaded during the
 * engine lifetime.
 *
 * Construct and use an instance on one owner thread. Member functions may update the owner-thread-only timezone cache,
 * may perform synchronous filesystem reads on the first use of a timezone, and invoke no callbacks.
 */
class SystemCronEngine final : public CronEngine {
public:
    /** Constructs an empty engine without reading timezone data.
     *
     * The engine does not require an event loop and may be constructed before one exists.
     */
    SystemCronEngine();

    /// Destroys the cached immutable timezone data.
    ~SystemCronEngine() override;

    /// Copying is disabled so a potentially large timezone cache is never duplicated implicitly.
    SystemCronEngine(SystemCronEngine const&) = delete;

    /** Transfers the complete timezone cache from another engine.
     *
     * The moved-from engine may only be destroyed or assigned another engine.
     */
    SystemCronEngine(SystemCronEngine&&) noexcept;

    /// Copy assignment is disabled with copy construction.
    auto operator=(SystemCronEngine const&) -> SystemCronEngine& = delete;

    /** Replaces this engine with another engine's complete timezone cache.
     *
     * The moved-from engine may only be destroyed or assigned another engine.
     *
     * @return This engine.
     */
    auto operator=(SystemCronEngine&&) noexcept -> SystemCronEngine&;

    /** Validates a cron expression and resolves its complete timezone.
     *
     * The schedule strings are borrowed only for this synchronous call and are not retained. Successfully loaded
     * timezone data is cached under a copied exact name.
     *
     * @param schedule Complete recurring schedule to validate.
     * @return Success, or the parser or timezone loader's stable schedule error.
     * @warning Call only from the engine's owner thread.
     */
    [[nodiscard]] auto validate(CronSchedule const& schedule) const -> jb::core::Result<void, jb::core::Error> override;

    /** Finds the first DST-aware UTC occurrence strictly after an instant.
     *
     * Spring-forward gaps shift the intended local occurrence by the exact offset increase. Fall-back overlaps select
     * the earlier UTC mapping. The 400-year Gregorian search is bounded and every returned instant is strictly greater
     * than `exclusive_lower_bound`.
     *
     * The schedule strings are borrowed only for this synchronous call and are not retained. Successfully loaded
     * timezone data is cached under a copied exact name.
     *
     * @param schedule Complete recurring schedule to evaluate.
     * @param exclusive_lower_bound UTC instant that the returned occurrence must exceed.
     * @return The next occurrence, or a stable parser, timezone, exhaustion, or range error.
     * @warning Call only from the engine's owner thread.
     */
    [[nodiscard]] auto next_after(CronSchedule const& schedule, jb::core::UtcTimePoint exclusive_lower_bound) const
        -> jb::core::Result<jb::core::UtcTimePoint, jb::core::Error> override;

private:
    /// Owns the exact-name immutable timezone cache.
    struct Private;
    std::unique_ptr<Private> _data;
};

/** Calculates a bounded sequence of strictly increasing cron occurrences.
 *
 * This function borrows `engine` and `schedule` for the duration of the call, retains neither, and calls
 * CronEngine::next_after() synchronously once per returned occurrence.
 *
 * @param engine Cron policy implementation that must outlive this call.
 * @param schedule Complete recurring schedule borrowed for the call.
 * @param exclusive_lower_bound UTC instant excluded from the result sequence.
 * @param count Number of occurrences to calculate, from 1 through 200.
 * @return Exactly `count` occurrences, or the first stable engine error. A count outside the accepted range returns
 * `jobu.schedule.invalid_count`.
 */
[[nodiscard]] auto next_cron_occurrences(CronEngine const&      engine,
                                         CronSchedule const&    schedule,
                                         jb::core::UtcTimePoint exclusive_lower_bound,
                                         std::size_t            count)
    -> jb::core::Result<std::vector<jb::core::UtcTimePoint>, jb::core::Error>;

} // namespace jb::jobu
