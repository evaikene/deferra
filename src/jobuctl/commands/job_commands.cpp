#include "commands_priv.hpp"

#include "attribute_registry.hpp"
#include "command_helpers_priv.hpp"
#include "job_validation_priv.hpp"
#include "management_json.hpp"
#include "utc_timestamp.hpp"

#include <fmt/format.h>

#include <bitset>
#include <charconv>
#include <cstdio>
#include <limits>
#include <system_error>
#include <utility>
#include <variant>

namespace jb::jobuctl::detail {

using namespace jb::core;
using namespace jb::jobu;

auto is_cli_creation_option(std::string_view name) -> bool
{
    return name == "working-directory" || name == "env" || name == "unset-env" || name == "expected-exit-code";
}

namespace {

auto parse_priority(std::string_view text) -> std::optional<std::int32_t>
{
    auto value  = std::int32_t{};
    auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

auto add_attribute(JsonValue::Object& attributes, std::string_view assignment) -> bool
{
    auto const separator = assignment.find('=');
    if (separator == std::string_view::npos || separator == 0U) {
        return false;
    }
    auto value = parse_json(assignment.substr(separator + 1U));
    if (!value) {
        return false;
    }
    return attributes.emplace(std::string{assignment.substr(0, separator)}, std::move(*value)).second;
}

auto parse_job_attributes(JsonValue::Object values, StandardAttributeRegistry const& registry)
    -> std::optional<AttributeSet>
{
    auto parsed = attribute_set_from_json(JsonValue{.data = std::move(values)}, registry, AttributeScope::Job);
    if (!parsed) {
        return std::nullopt;
    }
    return std::move(parsed).value();
}

auto option_value_allowing_unknown_following(std::span<CommandLineArgument const> arguments,
                                             std::size_t&                         index,
                                             bool allow_empty = false) -> std::optional<std::string_view>
{
    auto const& argument = arguments[index];
    if (argument.kind() != CommandLineArgumentKind::Option || !argument.known()) {
        return std::nullopt;
    }
    if (argument.value() && (allow_empty || !argument.value()->empty())) {
        return argument.value();
    }
    if (!argument.missing_value() || index + 1U >= arguments.size() ||
        arguments[index + 1U].kind() != CommandLineArgumentKind::Unknown) {
        return std::nullopt;
    }

    // Recover dash-leading values rejected as options by the lexer. A short-option cluster yields
    // several Unknown entries for the same raw token; consume that token only once.
    ++index;
    auto const value = arguments[index].token();
    while (value.size() > 2U && value[0] == '-' && value[1] != '-' && index + 1U < arguments.size() &&
           arguments[index + 1U].kind() == CommandLineArgumentKind::Unknown && arguments[index + 1U].token() == value) {
        ++index;
    }
    return value;
}

auto add_job_queue_selector(CommandLineArgument const& argument, std::optional<QueueSelector>& selector) -> bool
{
    if (selector || (argument.name() != "queue-id" && argument.name() != "queue-name")) {
        return false;
    }
    auto const value = option_value(argument);
    if (!value) {
        return false;
    }

    if (argument.name() == "queue-id") {
        auto id = Uuid::parse(*value);
        if (!id) {
            return false;
        }
        selector = QueueSelector{std::move(id).value()};
        return true;
    }

    selector = QueueSelector{std::string{*value}};
    return true;
}

auto make_job_id_command(std::filesystem::path                socket_path,
                         CommandKind                          kind,
                         std::string_view                     method,
                         std::span<CommandLineArgument const> arguments) -> CommandBuildResult
{
    if (arguments.size() != 1U || arguments.front().kind() != CommandLineArgumentKind::Positional) {
        return parse_failure(fmt::format("{} requires one job UUID", method));
    }
    auto id = Uuid::parse(arguments.front().token());
    if (!id) {
        return parse_failure(fmt::format("{} requires a valid job UUID", method));
    }

    auto params = job_id_to_json(id.value());
    if (!params) {
        return parse_failure(params.error().message);
    }
    return {
        .command =
            Command{
                    .socket_path = std::move(socket_path),
                    .kind        = kind,
                    .method      = method,
                    .request     = std::move(id).value(),
                    },
        .error = {},
    };
}

struct CliCreationOptions {
    std::optional<std::string> working_directory;
    JsonValue::Object          environment;
    JsonValue::Array           expected_exit_codes;
    std::bitset<256>           seen_exit_codes;

    auto supplied() const -> bool { return working_directory || !environment.empty() || !expected_exit_codes.empty(); }
};

auto add_cli_creation_option(CliCreationOptions& options, std::string_view name, std::string_view value)
    -> std::optional<std::string>
{
    if (name == "working-directory") {
        if (options.working_directory) {
            return "--working-directory may be supplied only once";
        }
        options.working_directory = std::string{value};
        return std::nullopt;
    }

    if (name == "expected-exit-code") {
        auto const code = parse_unsigned(value, 0, 255);
        if (!code || options.seen_exit_codes.test(*code)) {
            return "--expected-exit-code requires a unique integer from 0 through 255";
        }
        options.seen_exit_codes.set(*code);
        options.expected_exit_codes.push_back(JsonValue{.data = *code});
        return std::nullopt;
    }

    // Keep one namespace for assignments and removals so neither can silently overwrite the other.
    auto entry = JsonValue{.data = JsonNull{}};
    auto key   = value;
    if (name == "env") {
        auto const separator = value.find('=');
        if (separator == std::string_view::npos) {
            return "--env requires NAME=VALUE";
        }
        key        = value.substr(0, separator);
        entry.data = std::string{value.substr(separator + 1U)};
    }
    if (!options.environment.emplace(std::string{key}, std::move(entry)).second) {
        return "environment names must be unique across --env and --unset-env";
    }
    return std::nullopt;
}

auto parse_job_create(std::filesystem::path                socket_path,
                      std::span<CommandLineArgument const> arguments,
                      StandardAttributeRegistry const&     registry) -> CommandBuildResult
{
    auto selector         = std::optional<QueueSelector>{};
    auto type             = std::optional<JobType>{};
    auto schedule         = std::optional<JobCreationSchedule>{};
    auto timezone         = std::optional<std::string>{};
    auto name             = std::optional<std::string>{};
    auto priority         = std::int32_t{0};
    auto command          = std::optional<std::string>{};
    auto url              = std::optional<std::string>{};
    auto http_method      = std::optional<std::string>{};
    auto idempotency_key  = std::optional<std::string>{};
    auto arguments_json   = JsonValue::Array{};
    auto headers_json     = JsonValue::Array{};
    auto attributes_json  = JsonValue::Object{};
    auto body             = std::optional<std::string>{};
    auto cli_options      = CliCreationOptions{};
    auto type_seen        = false;
    auto name_seen        = false;
    auto priority_seen    = false;
    auto command_seen     = false;
    auto url_seen         = false;
    auto method_seen      = false;
    auto idempotency_seen = false;

    // Collect explicit fields without applying daemon defaults. Repeated arguments retain their order.
    for (auto index = std::size_t{0}; index < arguments.size(); ++index) {
        auto const& argument = arguments[index];
        if (argument.name() == "queue-id" || argument.name() == "queue-name") {
            if (!add_job_queue_selector(argument, selector)) {
                return parse_failure("job create requires exactly one valid --queue-id or --queue-name selector");
            }
            continue;
        }

        if (argument.name() == "arg") {
            auto const value = option_value_allowing_unknown_following(arguments, index, true);
            if (!value) {
                return parse_failure("--arg requires a value");
            }
            arguments_json.push_back(JsonValue{.data = std::string{*value}});
            continue;
        }
        if (argument.name() == "now" && argument.kind() == CommandLineArgumentKind::Option && argument.known() &&
            !argument.has_value()) {
            if (schedule) {
                return parse_failure("choose exactly one of --now, --at, or --cron");
            }
            schedule = ImmediateSchedule{};
            continue;
        }
        if (argument.name() == "body" && argument.kind() == CommandLineArgumentKind::Option && argument.known() &&
            argument.has_value() && !body) {
            body = std::string{*argument.value()};
            continue;
        }

        auto value = argument.name() == "priority" ? option_value_allowing_unknown_following(arguments, index)
                                                   : option_value(argument);
        if (!value) {
            return parse_failure("job create has an invalid option");
        }

        if (is_cli_creation_option(argument.name())) {
            if (auto error = add_cli_creation_option(cli_options, argument.name(), *value)) {
                return parse_failure(std::move(*error));
            }
            continue;
        }

        if (argument.name() == "type" && !type_seen) {
            if (*value == "cli") {
                type = JobType::Cli;
            }
            else if (*value == "http") {
                type = JobType::Http;
            }
            else {
                return parse_failure("--type must be cli or http");
            }
            type_seen = true;
            continue;
        }
        if (argument.name() == "at") {
            if (schedule) {
                return parse_failure("choose exactly one of --now, --at, or --cron");
            }
            auto parsed = parse_utc_timestamp(*value);
            if (!parsed) {
                return parse_failure("--at must be a valid canonical UTC timestamp");
            }
            schedule = OnceSchedule{.planned_at = std::move(parsed).value()};
            continue;
        }
        if (argument.name() == "cron") {
            if (schedule) {
                return parse_failure("choose exactly one of --now, --at, or --cron");
            }
            schedule = CronSchedule{.expression = std::string{*value}};
            continue;
        }
        if (argument.name() == "timezone" && !timezone) {
            timezone = std::string{*value};
            continue;
        }
        if (argument.name() == "attribute") {
            if (!add_attribute(attributes_json, *value)) {
                return parse_failure("--attribute requires a unique NAME=JSON_VALUE assignment");
            }
            continue;
        }
        if (argument.name() == "name" && !name_seen) {
            name      = std::string{*value};
            name_seen = true;
            continue;
        }
        if (argument.name() == "priority" && !priority_seen) {
            auto parsed = parse_priority(*value);
            if (!parsed) {
                return parse_failure("--priority must be an integer from -2147483648 through 2147483647");
            }
            priority      = *parsed;
            priority_seen = true;
            continue;
        }
        if (argument.name() == "command" && !command_seen) {
            command      = std::string{*value};
            command_seen = true;
            continue;
        }
        if (argument.name() == "url" && !url_seen) {
            url      = std::string{*value};
            url_seen = true;
            continue;
        }
        if (argument.name() == "method" && !method_seen) {
            http_method = std::string{*value};
            method_seen = true;
            continue;
        }
        if (argument.name() == "header") {
            auto const separator = value->find('=');
            if (separator == std::string_view::npos || separator == 0U) {
                return parse_failure("--header requires NAME=VALUE");
            }
            headers_json.push_back(JsonValue{
                .data = JsonValue::Object{
                                          {"name", JsonValue{.data = std::string{value->substr(0, separator)}}},
                                          {"value", JsonValue{.data = std::string{value->substr(separator + 1U)}}},
                                          }
            });
            continue;
        }
        if (argument.name() == "idempotency-key" && !idempotency_seen) {
            idempotency_key  = std::string{*value};
            idempotency_seen = true;
            continue;
        }
        return parse_failure("job create has an unknown or duplicate option");
    }

    if (!selector) {
        return parse_failure("job create requires exactly one --queue-id or --queue-name selector");
    }
    if (!type) {
        return parse_failure("job create requires --type cli or --type http");
    }
    if (!schedule) {
        return parse_failure("job create requires exactly one --now, --at, or --cron schedule");
    }
    if (timezone) {
        auto* cron = std::get_if<CronSchedule>(&*schedule);
        if (!cron) {
            return parse_failure("--timezone requires --cron");
        }
        cron->timezone = std::move(*timezone);
    }
    auto attributes = parse_job_attributes(std::move(attributes_json), registry);
    if (!attributes) {
        return parse_failure("--attribute contains an invalid JobU attribute value");
    }

    // Only the selected runner's fields may enter its payload; omission preserves server-side defaults.
    auto payload = JsonValue::Object{};
    if (*type == JobType::Cli) {
        if (!command || url || http_method || !headers_json.empty() || body) {
            return parse_failure("CLI job creation requires --command and rejects HTTP options");
        }
        payload.emplace("command", JsonValue{.data = std::move(*command)});
        if (!arguments_json.empty()) {
            payload.emplace("arguments", JsonValue{.data = std::move(arguments_json)});
        }
        // Omit absent fields so the daemon remains responsible for payload defaults.
        if (cli_options.working_directory) {
            payload.emplace("working_directory", JsonValue{.data = std::move(*cli_options.working_directory)});
        }
        if (!cli_options.environment.empty()) {
            payload.emplace("environment", JsonValue{.data = std::move(cli_options.environment)});
        }
        if (!cli_options.expected_exit_codes.empty()) {
            payload.emplace("expected_exit_codes", JsonValue{.data = std::move(cli_options.expected_exit_codes)});
        }
    }
    else {
        if (!url || command || !arguments_json.empty() || cli_options.supplied()) {
            return parse_failure("HTTP job creation requires --url and rejects CLI options");
        }
        payload.emplace("method", JsonValue{.data = http_method.value_or("GET")});
        payload.emplace("url", JsonValue{.data = std::move(*url)});
        if (!headers_json.empty()) {
            payload.emplace("headers", JsonValue{.data = std::move(headers_json)});
        }
        if (body) {
            payload.emplace("body",
                            JsonValue{
                                .data = JsonValue::Object{
                                                          {"encoding", JsonValue{.data = std::string{"utf8"}}},
                                                          {"data", JsonValue{.data = std::move(*body)}},
                                                          }
            });
        }
    }

    auto request = CreateJobRequest{
        .queue           = std::move(*selector),
        .name            = std::move(name),
        .type            = *type,
        .schedule        = std::move(*schedule),
        .priority        = priority,
        .attributes      = std::move(*attributes),
        .payload         = JsonValue{.data = std::move(payload)},
        .idempotency_key = std::move(idempotency_key),
    };
    // The request encoder checks the wire shape, not CLI policy. Reuse management's complete validation before IPC.
    auto validated = jb::jobu::detail::validate_and_serialize_job_payload(request.type, request.payload);
    if (!validated) {
        return parse_failure(
            fmt::format("invalid job payload: {}", jb::jobu::detail::job_payload_issue_text(validated.error())));
    }
    auto params = create_job_request_to_json(request, registry);
    if (!params) {
        return parse_failure(params.error().message);
    }
    return {
        .command =
            Command{
                    .socket_path = std::move(socket_path),
                    .kind        = CommandKind::JobCreate,
                    .method      = "job.create",
                    .request     = std::move(request),
                    },
        .error = {},
    };
}

auto parse_job_list(std::filesystem::path socket_path, std::span<CommandLineArgument const> arguments)
    -> CommandBuildResult
{
    auto request              = JobListRequest{};
    auto include_deleted_seen = false;
    auto limit_seen           = false;
    auto after_seen           = false;

    for (auto const& argument : arguments) {
        if (argument.name() == "queue-id" || argument.name() == "queue-name") {
            if (!add_job_queue_selector(argument, request.queue)) {
                return parse_failure("job list accepts at most one valid --queue-id or --queue-name selector");
            }
            continue;
        }
        if (argument.kind() != CommandLineArgumentKind::Option || !argument.known()) {
            return parse_failure("job list has an invalid option");
        }
        if (argument.name() == "include-deleted" && !include_deleted_seen && !argument.has_value()) {
            request.include_deleted = true;
            include_deleted_seen    = true;
            continue;
        }

        auto const value = option_value(argument);
        if (!value) {
            return parse_failure("job list has an invalid option value");
        }
        if (argument.name() == "limit" && !limit_seen) {
            auto parsed = parse_unsigned(*value, 1, 200);
            if (!parsed) {
                return parse_failure("--limit must be an integer from 1 through 200");
            }
            request.page.limit = static_cast<std::size_t>(*parsed);
            limit_seen         = true;
            continue;
        }
        if (argument.name() == "after" && !after_seen) {
            auto id = Uuid::parse(*value);
            if (!id) {
                return parse_failure("--after must be a UUID");
            }
            request.page.after_id = std::move(id).value();
            after_seen            = true;
            continue;
        }
        return parse_failure("job list has an unknown or duplicate option");
    }

    auto params = job_list_request_to_json(request);
    if (!params) {
        return parse_failure(params.error().message);
    }
    return {
        .command =
            Command{
                    .socket_path = std::move(socket_path),
                    .kind        = CommandKind::JobList,
                    .method      = "job.list",
                    .request     = std::move(request),
                    },
        .error = {},
    };
}

auto parse_job_update(std::filesystem::path                socket_path,
                      std::span<CommandLineArgument const> arguments,
                      StandardAttributeRegistry const&     registry) -> CommandBuildResult
{
    if (arguments.empty() || arguments.front().kind() != CommandLineArgumentKind::Positional) {
        return parse_failure("job update requires a job UUID");
    }
    auto id = Uuid::parse(arguments.front().token());
    if (!id) {
        return parse_failure("job update requires a valid job UUID");
    }

    // The nested optional distinguishes an omitted name from an explicit --clear-name patch.
    auto revision        = std::optional<JobRevision>{};
    auto name            = std::optional<std::optional<std::string>>{};
    auto priority        = std::optional<std::int32_t>{};
    auto schedule        = std::optional<JobSchedule>{};
    auto timezone        = std::optional<std::string>{};
    auto attributes_json = JsonValue::Object{};
    auto revision_seen   = false;
    auto name_seen       = false;
    auto priority_seen   = false;
    auto remaining       = arguments.subspan(1);

    for (auto index = std::size_t{0}; index < remaining.size(); ++index) {
        auto const& argument = remaining[index];
        if (argument.name() == "clear-name" && argument.kind() == CommandLineArgumentKind::Option && argument.known() &&
            !argument.has_value() && !name_seen) {
            name.emplace(std::nullopt);
            name_seen = true;
            continue;
        }

        auto value = argument.name() == "priority" ? option_value_allowing_unknown_following(remaining, index)
                                                   : option_value(argument);
        if (!value) {
            return parse_failure("job update has an invalid option");
        }
        if (argument.name() == "revision" && !revision_seen) {
            auto parsed = parse_unsigned(*value, 1, std::numeric_limits<std::uint64_t>::max());
            if (!parsed) {
                return parse_failure("--revision must be a positive 64-bit integer");
            }
            revision      = parsed;
            revision_seen = true;
            continue;
        }
        if (argument.name() == "name" && !name_seen) {
            name.emplace(std::string{*value});
            name_seen = true;
            continue;
        }
        if (argument.name() == "priority" && !priority_seen) {
            auto parsed = parse_priority(*value);
            if (!parsed) {
                return parse_failure("--priority must be an integer from -2147483648 through 2147483647");
            }
            priority      = parsed;
            priority_seen = true;
            continue;
        }
        if (argument.name() == "at") {
            if (schedule) {
                return parse_failure("choose exactly one of --at or --cron");
            }
            auto parsed = parse_utc_timestamp(*value);
            if (!parsed) {
                return parse_failure("--at must be a valid canonical UTC timestamp");
            }
            schedule = JobSchedule{OnceSchedule{.planned_at = std::move(parsed).value()}};
            continue;
        }
        if (argument.name() == "cron") {
            if (schedule) {
                return parse_failure("choose exactly one of --at or --cron");
            }
            schedule = JobSchedule{CronSchedule{.expression = std::string{*value}}};
            continue;
        }
        if (argument.name() == "timezone" && !timezone) {
            timezone = std::string{*value};
            continue;
        }
        if (argument.name() == "attribute") {
            if (!add_attribute(attributes_json, *value)) {
                return parse_failure("--attribute requires a unique NAME=JSON_VALUE assignment");
            }
            continue;
        }
        return parse_failure("job update has an unknown or duplicate option");
    }

    if (!revision) {
        return parse_failure("job update requires --revision");
    }
    if (timezone) {
        auto* cron = schedule ? std::get_if<CronSchedule>(&*schedule) : nullptr;
        if (!cron) {
            return parse_failure("--timezone requires --cron");
        }
        cron->timezone = std::move(*timezone);
    }
    auto attributes = parse_job_attributes(std::move(attributes_json), registry);
    if (!attributes) {
        return parse_failure("--attribute contains an invalid JobU attribute value");
    }
    if (!name && !priority && !schedule && attributes->empty()) {
        return parse_failure("job update requires at least one mutable field");
    }

    auto request = UpdateJobRequest{
        .job_id            = id.value(),
        .expected_revision = *revision,
        .name              = std::move(name),
        .schedule          = std::move(schedule),
        .priority          = priority,
        .attribute_changes = std::move(*attributes),
    };
    auto params = update_job_request_to_json(request, registry);
    if (!params) {
        return parse_failure(params.error().message);
    }
    return {
        .command =
            Command{
                    .socket_path = std::move(socket_path),
                    .kind        = CommandKind::JobUpdate,
                    .method      = "job.update",
                    .request     = std::move(request),
                    },
        .error = {},
    };
}

auto parse_job_move(std::filesystem::path socket_path, std::span<CommandLineArgument const> arguments)
    -> CommandBuildResult
{
    if (arguments.empty() || arguments.front().kind() != CommandLineArgumentKind::Positional) {
        return parse_failure("job move requires a job UUID");
    }
    auto id = Uuid::parse(arguments.front().token());
    if (!id) {
        return parse_failure("job move requires a valid job UUID");
    }

    auto revision      = std::optional<JobRevision>{};
    auto target_queue  = std::optional<QueueSelector>{};
    auto revision_seen = false;
    for (auto const& argument : arguments.subspan(1)) {
        if (argument.name() == "queue-id" || argument.name() == "queue-name") {
            if (!add_job_queue_selector(argument, target_queue)) {
                return parse_failure("job move requires exactly one valid --queue-id or --queue-name selector");
            }
            continue;
        }
        auto const value = option_value(argument);
        if (!value || argument.name() != "revision" || revision_seen) {
            return parse_failure("job move has an unknown, invalid, or duplicate option");
        }
        auto parsed = parse_unsigned(*value, 1, std::numeric_limits<std::uint64_t>::max());
        if (!parsed) {
            return parse_failure("--revision must be a positive 64-bit integer");
        }
        revision      = parsed;
        revision_seen = true;
    }
    if (!revision) {
        return parse_failure("job move requires --revision");
    }
    if (!target_queue) {
        return parse_failure("job move requires exactly one --queue-id or --queue-name selector");
    }

    // Carry the caller's revision unchanged; parsing never fetches a newer definition for a retry.
    auto request = MoveJobRequest{
        .job_id            = id.value(),
        .expected_revision = *revision,
        .target_queue      = std::move(*target_queue),
    };
    auto params = move_job_request_to_json(request);
    if (!params) {
        return parse_failure(params.error().message);
    }
    return {
        .command =
            Command{
                    .socket_path = std::move(socket_path),
                    .kind        = CommandKind::JobMove,
                    .method      = "job.move",
                    .request     = std::move(request),
                    },
        .error = {},
    };
}

auto parse_job_delete(std::filesystem::path socket_path, std::span<CommandLineArgument const> arguments)
    -> CommandBuildResult
{
    if (arguments.empty() || arguments.front().kind() != CommandLineArgumentKind::Positional) {
        return parse_failure("job delete requires a job UUID");
    }
    auto id = Uuid::parse(arguments.front().token());
    if (!id) {
        return parse_failure("job delete requires a valid job UUID");
    }
    if (arguments.size() != 2U) {
        return parse_failure("job delete requires exactly one --revision option");
    }
    auto const revision_value = option_value(arguments[1]);
    if (!revision_value || arguments[1].name() != "revision") {
        return parse_failure("job delete requires exactly one valid --revision option");
    }
    auto revision = parse_unsigned(*revision_value, 1, std::numeric_limits<std::uint64_t>::max());
    if (!revision) {
        return parse_failure("--revision must be a positive 64-bit integer");
    }

    auto request = DeleteJobRequest{
        .job_id            = id.value(),
        .expected_revision = *revision,
    };
    auto params = delete_job_request_to_json(request);
    if (!params) {
        return parse_failure(params.error().message);
    }
    return {
        .command =
            Command{
                    .socket_path = std::move(socket_path),
                    .kind        = CommandKind::JobDelete,
                    .method      = "job.delete",
                    .request     = request,
                    },
        .error = {},
    };
}

auto job_state_text(JobState state) noexcept -> std::string_view
{
    switch (state) {
        case JobState::Active:
            return "active";
        case JobState::Suspending:
            return "suspending";
        case JobState::Suspended:
            return "suspended";
        case JobState::Deleted:
            return "deleted";
    }
    return "unknown";
}

auto job_type_text(JobType type) noexcept -> std::string_view
{
    switch (type) {
        case JobType::Cli:
            return "cli";
        case JobType::Http:
            return "http";
    }
    return "unknown";
}

auto print_job(JobDefinition const& job) -> bool
{
    auto schedule = std::string{};
    if (auto const* once = std::get_if<OnceSchedule>(&job.schedule)) {
        auto at = format_utc_timestamp(once->planned_at);
        if (!at) {
            return false;
        }
        schedule = fmt::format("at={}", *at);
    }
    else if (auto const* cron = std::get_if<CronSchedule>(&job.schedule)) {
        schedule = fmt::format("cron={}, timezone={}", escape_human(cron->expression), escape_human(cron->timezone));
    }

    fmt::print(stdout,
               "Job {}: queue_id={}, revision={}, name={}, state={}, type={}, {}, priority={}\n",
               job.id.to_string(),
               job.queue_id.to_string(),
               job.revision,
               escape_human(job.name.value_or("<unnamed>")),
               job_state_text(job.state),
               job_type_text(job.type),
               schedule,
               job.priority);
    return true;
}

auto print_job_page(JobPage const& page) -> bool
{
    if (page.items.empty()) {
        fmt::print(stdout, "No jobs\n");
    }
    for (auto const& job : page.items) {
        if (!print_job(job)) {
            return false;
        }
    }
    if (page.next_after_id) {
        fmt::print(stdout, "Next after: {}\n", page.next_after_id->to_string());
    }
    return true;
}

} // namespace

auto parse_job_command(std::filesystem::path                socket_path,
                       std::string_view                     action,
                       std::span<CommandLineArgument const> arguments,
                       StandardAttributeRegistry const&     registry) -> CommandBuildResult
{
    if (action == "create") {
        return parse_job_create(std::move(socket_path), arguments, registry);
    }
    if (action == "get") {
        return make_job_id_command(std::move(socket_path), CommandKind::JobGet, "job.get", arguments);
    }
    if (action == "list") {
        return parse_job_list(std::move(socket_path), arguments);
    }
    if (action == "update") {
        return parse_job_update(std::move(socket_path), arguments, registry);
    }
    if (action == "suspend") {
        return make_job_id_command(std::move(socket_path), CommandKind::JobSuspend, "job.suspend", arguments);
    }
    if (action == "resume") {
        return make_job_id_command(std::move(socket_path), CommandKind::JobResume, "job.resume", arguments);
    }
    if (action == "move") {
        return parse_job_move(std::move(socket_path), arguments);
    }
    if (action == "delete") {
        return parse_job_delete(std::move(socket_path), arguments);
    }
    return parse_failure("unknown job action");
}

auto print_job_result(Command const& command, ControlReply const& value) -> bool
{
    if (command.kind == CommandKind::JobList) {
        auto const* page = std::get_if<JobPage>(&value);
        if (!page) {
            return false;
        }
        return print_job_page(*page);
    }
    if (command.kind == CommandKind::JobDelete) {
        auto const* request = std::get_if<DeleteJobRequest>(&command.request);
        if (!std::holds_alternative<EmptyReply>(value) || !request) {
            return false;
        }
        fmt::print(stdout, "Deleted job id={}\n", request->job_id.to_string());
        return true;
    }
    auto const* job = std::get_if<JobDefinition>(&value);
    if (!job) {
        return false;
    }
    return print_job(*job);
}

} // namespace jb::jobuctl::detail
