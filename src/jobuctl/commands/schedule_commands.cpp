#include "commands_priv.hpp"

#include "command_helpers_priv.hpp"
#include "control_json.hpp"
#include "json.hpp"
#include "utc_timestamp.hpp"

#include <fmt/format.h>

#include <cstdio> // IWYU pragma: keep for stdout macro
#include <string>
#include <utility>

namespace jb::jobuctl::detail {

using namespace jb::core;
using namespace jb::jobu;

auto parse_schedule_command(std::filesystem::path                socket_path,
                            std::string_view                     action,
                            std::span<CommandLineArgument const> arguments,
                            StandardAttributeRegistry const& /*registry*/) -> CommandBuildResult
{
    if (arguments.empty() || arguments.front().kind() != CommandLineArgumentKind::Positional ||
        arguments.front().token().empty()) {
        return parse_failure("schedule command requires a cron expression");
    }

    auto fields   = JsonValue::Object{};
    auto schedule = JsonValue::Object{
        {"kind",       JsonValue{.data = std::string{"cron"}}                   },
        {"expression", JsonValue{.data = std::string{arguments.front().token()}}},
        {"timezone",   JsonValue{.data = std::string{"UTC"}}                    },
    };
    for (auto const& argument : arguments.subspan(1)) {
        auto value = option_value(argument);
        if (!value) {
            return parse_failure("schedule options require nonempty values");
        }
        if (argument.name() == "timezone") {
            schedule["timezone"] = JsonValue{.data = std::string{*value}};
        }
        else if (argument.name() == "count") {
            auto count = parse_unsigned(*value, 1, 200);
            if (!count) {
                return parse_failure("--count must be from 1 through 200");
            }
            fields.emplace("count", JsonValue{.data = *count});
        }
        else if (argument.name() == "after") {
            fields.emplace("after", JsonValue{.data = std::string{*value}});
        }
        else {
            return parse_failure("schedule command has an unknown option");
        }
    }
    fields.emplace("schedule", JsonValue{.data = std::move(schedule)});
    auto params = JsonValue{.data = std::move(fields)};

    if (action == "validate") {
        auto request = schedule_validate_request_from_json(params);
        if (!request) {
            return parse_failure("schedule expression or timezone is invalid");
        }
        return {
            .command = Command{.socket_path = std::move(socket_path),
                               .kind        = CommandKind::ScheduleValidate,
                               .request     = std::move(request).value()}
        };
    }
    if (action == "next") {
        auto request = schedule_next_request_from_json(params);
        if (!request) {
            return parse_failure("schedule expression, timezone, after time, or count is invalid");
        }
        return {
            .command = Command{.socket_path = std::move(socket_path),
                               .kind        = CommandKind::ScheduleNext,
                               .request     = std::move(request).value()}
        };
    }
    return parse_failure("unknown schedule command");
}

auto print_schedule_result(CommandKind kind, ControlReply const& reply) -> bool
{
    if (kind == CommandKind::ScheduleValidate && std::holds_alternative<ScheduleValidationReply>(reply)) {
        fmt::print(stdout, "Schedule is valid\n");
        return true;
    }
    if (kind == CommandKind::ScheduleNext) {
        auto const* preview = std::get_if<ScheduleNextReply>(&reply);
        if (!preview) {
            return false;
        }
        for (auto const& occurrence : preview->occurrences) {
            auto formatted = format_utc_timestamp(occurrence);
            if (!formatted) {
                return false;
            }
            fmt::print(stdout, "{}\n", *formatted);
        }
        return true;
    }
    return false;
}

} // namespace jb::jobuctl::detail
