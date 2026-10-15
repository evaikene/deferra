#include "scheduler.hpp"

#include <cstdint>
#include <type_traits>
#include <utility>

static_assert(std::is_same_v<std::underlying_type_t<jb::jobu::CancelDisposition>, std::uint8_t>);
static_assert(std::is_same_v<std::underlying_type_t<jb::jobu::SchedulerState>, std::uint8_t>);
static_assert(std::is_copy_constructible_v<jb::jobu::CancelRunResult>);
static_assert(std::is_move_constructible_v<jb::jobu::CancelRunResult>);
static_assert(std::is_default_constructible_v<jb::jobu::SchedulerOptions>);
static_assert(std::is_base_of_v<jb::core::Object, jb::jobu::Scheduler>);
static_assert(
    std::is_same_v<decltype(jb::jobu::Scheduler::attempt_started), jb::core::Signal<jb::jobu::AttemptStarted>>);
static_assert(
    std::is_same_v<decltype(jb::jobu::Scheduler::attempt_finished), jb::core::Signal<jb::jobu::AttemptFinished>>);
static_assert(
    std::is_same_v<decltype(jb::jobu::Scheduler::retry_scheduled), jb::core::Signal<jb::jobu::RetryScheduled>>);
static_assert(std::is_same_v<decltype(jb::jobu::Scheduler::run_finished), jb::core::Signal<jb::jobu::RunFinished>>);
static_assert(!std::is_copy_constructible_v<jb::jobu::Scheduler>);
static_assert(!std::is_move_constructible_v<jb::jobu::Scheduler>);
static_assert(std::is_constructible_v<jb::jobu::Scheduler,
                                      jb::db::Database&,
                                      jb::jobu::AttributeRegistry const&,
                                      jb::jobu::CronEngine const&,
                                      jb::core::UuidGenerator&,
                                      jb::core::TimeSource&,
                                      jb::jobu::AttemptExecutor&,
                                      jb::jobu::SecretProvider&>);
static_assert(noexcept(std::declval<jb::jobu::Scheduler&>().shutdown()));
static_assert(std::is_same_v<decltype(std::declval<jb::jobu::Scheduler&>().shutdown()), void>);

auto main() -> int
{
    return 0;
}
