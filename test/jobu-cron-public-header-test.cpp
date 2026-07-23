#include "cron.hpp"

#include <type_traits>

using ValidateResult    = jb::core::Result<void, jb::core::Error>;
using OccurrenceResult  = jb::core::Result<jb::core::UtcTimePoint, jb::core::Error>;
using OccurrencesResult = jb::core::Result<std::vector<jb::core::UtcTimePoint>, jb::core::Error>;

static_assert(std::is_abstract_v<jb::jobu::CronEngine>);
static_assert(std::has_virtual_destructor_v<jb::jobu::CronEngine>);
static_assert(std::is_same_v<decltype(&jb::jobu::CronEngine::validate),
                             ValidateResult (jb::jobu::CronEngine::*)(jb::jobu::CronSchedule const&) const>);
static_assert(std::is_same_v<decltype(&jb::jobu::CronEngine::next_after),
                             OccurrenceResult (jb::jobu::CronEngine::*)(jb::jobu::CronSchedule const&,
                                                                        jb::core::UtcTimePoint) const>);

static_assert(std::is_final_v<jb::jobu::SystemCronEngine>);
static_assert(std::is_base_of_v<jb::jobu::CronEngine, jb::jobu::SystemCronEngine>);
static_assert(std::is_default_constructible_v<jb::jobu::SystemCronEngine>);
static_assert(!std::is_copy_constructible_v<jb::jobu::SystemCronEngine>);
static_assert(!std::is_copy_assignable_v<jb::jobu::SystemCronEngine>);
static_assert(std::is_nothrow_move_constructible_v<jb::jobu::SystemCronEngine>);
static_assert(std::is_nothrow_move_assignable_v<jb::jobu::SystemCronEngine>);
static_assert(std::is_same_v<decltype(&jb::jobu::SystemCronEngine::validate),
                             ValidateResult (jb::jobu::SystemCronEngine::*)(jb::jobu::CronSchedule const&) const>);
static_assert(std::is_same_v<decltype(&jb::jobu::SystemCronEngine::next_after),
                             OccurrenceResult (jb::jobu::SystemCronEngine::*)(jb::jobu::CronSchedule const&,
                                                                              jb::core::UtcTimePoint) const>);
static_assert(std::is_same_v<decltype(&jb::jobu::next_cron_occurrences),
                             OccurrencesResult (*)(jb::jobu::CronEngine const&,
                                                   jb::jobu::CronSchedule const&,
                                                   jb::core::UtcTimePoint,
                                                   std::size_t)>);

auto main() -> int
{
    return 0;
}
