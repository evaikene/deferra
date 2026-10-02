#include "execution_telemetry.hpp"

#include <type_traits>
#include <utility>

static_assert(std::is_copy_constructible_v<jb::jobu::TelemetryOptions>);
static_assert(std::is_copy_constructible_v<jb::jobu::DelayedRun>);
static_assert(std::is_same_v<decltype(jb::jobu::DelayedRun::runnable_wait), std::chrono::microseconds>);
static_assert(std::is_same_v<decltype(jb::jobu::DelayedRun::threshold), std::chrono::milliseconds>);
static_assert(std::is_same_v<decltype(jb::jobu::ExecutionTelemetry::delayed), jb::core::Signal<jb::jobu::DelayedRun>>);
static_assert(std::is_base_of_v<jb::core::Object, jb::jobu::ExecutionTelemetry>);
static_assert(!std::is_copy_constructible_v<jb::jobu::ExecutionTelemetry>);
static_assert(!std::is_move_constructible_v<jb::jobu::ExecutionTelemetry>);
static_assert(std::is_constructible_v<jb::jobu::ExecutionTelemetry,
                                      jb::db::Database&,
                                      jb::jobu::AttributeRegistry const&,
                                      jb::core::TimeSource&,
                                      jb::core::UuidGenerator&>);
static_assert(noexcept(std::declval<jb::jobu::ExecutionTelemetry&>().request_stop()));
static_assert(std::is_same_v<decltype(std::declval<jb::jobu::ExecutionTelemetry&>().finish_stop()),
                             jb::core::Result<void, jb::core::Error>>);

auto main() -> int
{
    return 0;
}
