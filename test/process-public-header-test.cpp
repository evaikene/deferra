#include "process.hpp"

#include <concepts>
#include <cstdint>
#include <optional>
#include <type_traits>

using namespace jb::core;

static_assert(std::is_final_v<Process> && std::is_base_of_v<Object, Process>);
static_assert(std::has_virtual_destructor_v<Process>);
static_assert(!std::is_copy_constructible_v<Process> && !std::is_move_constructible_v<Process>);
static_assert(!std::is_copy_assignable_v<Process> && !std::is_move_assignable_v<Process>);
static_assert(std::same_as<std::underlying_type_t<ProcessState>, std::uint8_t>);
static_assert(std::same_as<std::underlying_type_t<ProcessExitKind>, std::uint8_t>);
static_assert(std::same_as<std::underlying_type_t<ProcessStopReason>, std::uint8_t>);
static_assert(ProcessState::Finishing != ProcessState::NotRunning);
static_assert(std::is_copy_constructible_v<ProcessStartInfo> && std::is_copy_constructible_v<ProcessExit>);
static_assert(std::same_as<decltype(&Process::start), Result<void, Error> (Process::*)(ProcessStartInfo)>);
static_assert(std::same_as<decltype(&Process::stop), Result<void, Error> (Process::*)(ProcessStopReason)>);
static_assert(std::same_as<decltype(&Process::state), ProcessState (Process::*)() const noexcept>);
static_assert(std::same_as<decltype(&Process::process_id), std::optional<std::int64_t> (Process::*)() const noexcept>);
static_assert(std::same_as<decltype(Process::started), Signal<>>);
static_assert(std::same_as<decltype(Process::standard_output), Signal<ByteBuffer>>);
static_assert(std::same_as<decltype(Process::standard_error), Signal<ByteBuffer>>);
static_assert(std::same_as<decltype(Process::finished), Signal<ProcessExit>>);

auto main() -> int
{
    Process process;
    return process.state() == ProcessState::NotRunning && !process.process_id() && !process.parent() ? 0 : 1;
}
