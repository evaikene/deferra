#include "schedule_input_priv.hpp"

#include <string>
#include <utility>

namespace jb::jobu::detail {

namespace {

template <typename T>
using InputResult = jb::core::Result<T, jb::core::Error>;

auto invalid_request() -> jb::core::Error
{
    return {.category = jb::core::ErrorCategory::InvalidArgument,
            .code     = "jobu.protocol.invalid_request",
            .message  = "The JobU schedule request is invalid"};
}

} // namespace

auto cron_schedule_input_from_json(jb::core::JsonValue const& value) -> InputResult<CronScheduleInput>
{
    if (!value.is_object()) {
        return InputResult<CronScheduleInput>::failure(invalid_request());
    }
    auto const& object = value.as_object();
    for (auto const& [name, member] : object) {
        static_cast<void>(member);
        if (name != "kind" && name != "expression" && name != "timezone") {
            return InputResult<CronScheduleInput>::failure(invalid_request());
        }
    }

    auto kind       = object.find("kind");
    auto expression = object.find("expression");
    if (kind == object.end() || !kind->second.is_string() || kind->second.as_string() != "cron" ||
        expression == object.end() || !expression->second.is_string()) {
        return InputResult<CronScheduleInput>::failure(invalid_request());
    }

    auto input = CronScheduleInput{.expression = expression->second.as_string()};
    if (auto timezone = object.find("timezone"); timezone != object.end()) {
        if (!timezone->second.is_string() || timezone->second.as_string().empty()) {
            return InputResult<CronScheduleInput>::failure(invalid_request());
        }
        input.timezone = timezone->second.as_string();
    }
    return InputResult<CronScheduleInput>::success(std::move(input));
}

auto cron_schedule_input_to_json(CronScheduleInput const& schedule) -> InputResult<jb::core::JsonValue>
{
    if (schedule.timezone && schedule.timezone->empty()) {
        return InputResult<jb::core::JsonValue>::failure(invalid_request());
    }
    auto object = jb::core::JsonValue::Object{
        {"kind",       {.data = std::string{"cron"}}},
        {"expression", {.data = schedule.expression}},
    };
    if (schedule.timezone) {
        object.emplace("timezone", jb::core::JsonValue{.data = *schedule.timezone});
    }
    return InputResult<jb::core::JsonValue>::success({.data = std::move(object)});
}

auto resolve_cron_schedule(CronScheduleInput const& input, std::string_view default_timezone)
    -> InputResult<CronSchedule>
{
    auto timezone = input.timezone ? std::string_view{*input.timezone} : default_timezone;
    if (timezone.empty()) {
        return InputResult<CronSchedule>::failure({.category = jb::core::ErrorCategory::InvalidArgument,
                                                   .code     = "jobu.schedule.invalid_timezone",
                                                   .message  = "Cron timezone must be nonempty"});
    }
    return InputResult<CronSchedule>::success({.expression = input.expression, .timezone = std::string{timezone}});
}

} // namespace jb::jobu::detail
