#include "execution_telemetry.hpp"

#include <type_traits>
#include <utility>

static_assert(std::is_copy_constructible_v<jb::jobu::TelemetryOptions>);
static_assert(std::is_base_of_v<jb::core::Object, jb::jobu::ExecutionTelemetry>);
static_assert(!std::is_copy_constructible_v<jb::jobu::ExecutionTelemetry>);
static_assert(!std::is_move_constructible_v<jb::jobu::ExecutionTelemetry>);
static_assert(std::is_constructible_v<jb::jobu::ExecutionTelemetry,
                                      jb::db::Database&,
                                      jb::jobu::AttributeRegistry const&,
                                      jb::core::TimeSource&,
                                      jb::core::UuidGenerator&>);
static_assert(noexcept(std::declval<jb::jobu::ExecutionTelemetry&>().request_stop()));

auto main() -> int
{
    return 0;
}
