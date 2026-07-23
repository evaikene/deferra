#include "cron.hpp"

#include <type_traits>

using ValidateResult   = jb::core::Result<void, jb::core::Error>;
using OccurrenceResult = jb::core::Result<jb::core::UtcTimePoint, jb::core::Error>;

static_assert(std::is_abstract_v<jb::jobu::CronEngine>);
static_assert(std::has_virtual_destructor_v<jb::jobu::CronEngine>);
static_assert(std::is_same_v<decltype(&jb::jobu::CronEngine::validate),
                             ValidateResult (jb::jobu::CronEngine::*)(jb::jobu::CronSchedule const&) const>);
static_assert(std::is_same_v<decltype(&jb::jobu::CronEngine::next_after),
                             OccurrenceResult (jb::jobu::CronEngine::*)(jb::jobu::CronSchedule const&,
                                                                        jb::core::UtcTimePoint) const>);

auto main() -> int
{
    return 0;
}
