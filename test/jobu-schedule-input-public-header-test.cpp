#include "schedule_input.hpp"

#include <type_traits>

static_assert(std::is_same_v<decltype(jb::jobu::CronScheduleInput::timezone), std::optional<std::string>>);
static_assert(std::is_constructible_v<jb::jobu::JobScheduleInput, jb::jobu::CronScheduleInput>);
static_assert(!std::is_constructible_v<jb::jobu::JobSchedule, jb::jobu::CronScheduleInput>);

int main()
{
    return jb::jobu::CronScheduleInput{}.timezone ? 1 : 0;
}
