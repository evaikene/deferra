#include "command_registry_priv.hpp"

#include "commands/commands_priv.hpp"

#include <algorithm>
#include <array>

namespace jb::jobuctl::detail {

using namespace jb::core;

namespace {

constexpr auto
value_option(std::string_view name, std::string_view value, std::string_view description, bool repeatable = false)
    -> OptionSpec
{
    return {
        .option      = {.long_name = name, .value_mode = CommandLineValueMode::Required},
        .value_name  = value,
        .description = description,
        .repeatable  = repeatable
    };
}

constexpr auto flag(std::string_view name, std::string_view description, char short_name = '\0') -> OptionSpec
{
    return {
        .option      = {.long_name = name, .short_name = short_name},
        .value_name  = {},
        .description = description
    };
}

constexpr std::array groups{
    GroupSpec{.name = "system", .summary = "Inspect the daemon"               },
    GroupSpec{.name = "queue",  .summary = "Create and manage queues"         },
    GroupSpec{.name = "job",    .summary = "Create and manage job definitions"},
};
constexpr std::array globals{
    value_option("socket",
                 "PATH",
                 "Daemon socket; required for remote commands. May precede or follow the command path."),
    flag("help", "Show local help without contacting a daemon.", 'h'),
    flag("version", "Show the local executable version (root only)."),
};

constexpr auto id            = value_option("id", "UUID", "Select a queue by ID; mutually exclusive with --name.");
constexpr auto name_selector = value_option("name", "NAME", "Select a queue by name; mutually exclusive with --id.");
constexpr auto queue_id =
    value_option("queue-id", "UUID", "Select a queue by ID; mutually exclusive with --queue-name.");
constexpr auto queue_name =
    value_option("queue-name", "NAME", "Select a queue by name; mutually exclusive with --queue-id.");
constexpr auto key =
    value_option("idempotency-key", "KEY", "Optional key for safely replaying the same creation request.");
constexpr auto       deleted  = flag("include-deleted", "Include deleted definitions; default: excluded.");
constexpr auto       limit    = value_option("limit", "N", "Page size, 1..200; default: 100.");
constexpr auto       after    = value_option("after", "UUID", "Continue after the ID returned by the previous page.");
constexpr auto       revision = value_option("revision", "N", "Required current revision, 1..18446744073709551615.");
constexpr auto       at       = value_option("at", "UTC", "Once schedule timestamp, for example 2030-01-01T00:00:00Z.");
constexpr auto       priority = value_option("priority", "N", "Scheduling priority, -2147483648..2147483647.");
constexpr std::array queue_selector{id, name_selector};
constexpr std::array queue_create{
    value_option("weight", "N", "Scheduler weight, 1..4294967295; default: 1."),
    value_option("concurrency-limit", "N", "Concurrency limit, 1..4294967295; default: 1."),
    value_option("recovery-policy", "POLICY", "fail_interrupted (default) or retry_interrupted."),
    key,
};
constexpr std::array queue_list{deleted, limit, after};
constexpr std::array queue_update{
    id,
    name_selector,
    value_option("new-name", "NAME", "Replace the queue name."),
    value_option("weight", "N", "Replace scheduler weight, 1..4294967295."),
    value_option("concurrency-limit", "N", "Replace concurrency limit, 1..4294967295."),
};
constexpr std::array job_create{
    queue_id,
    queue_name,
    value_option("type", "cli|http", "Required runner type."),
    at,
    value_option("name", "NAME", "Optional job name; omitted by default."),
    value_option("priority", "N", "Scheduling priority, -2147483648..2147483647; default: 0."),
    key,
    value_option("command", "PATH", "CLI executable: absolute path or bare name with explicit --env PATH=... ."),
    value_option("arg", "VALUE", "CLI argument; repeat in order. Use --arg=VALUE for dash-leading values.", true),
    value_option("working-directory", "PATH", "Absolute CLI working directory; default: /."),
    value_option("env", "NAME=VALUE", "CLI environment assignment; repeat for distinct names.", true),
    value_option("unset-env", "NAME", "Remove a CLI environment variable; repeat for distinct names.", true),
    value_option("expected-exit-code",
                 "N",
                 "Successful CLI exit code, 0..255; repeat distinct codes; default: 0.",
                 true),
    value_option("url", "URL", "Required HTTP URL for --type http."),
    value_option("method", "METHOD", "HTTP method; default: GET."),
};
constexpr std::array job_list{queue_id, queue_name, deleted, limit, after};
constexpr std::array job_update{
    revision,
    value_option("name", "NAME", "Replace the job name; mutually exclusive with --clear-name."),
    flag("clear-name", "Remove the job name; mutually exclusive with --name."),
    priority,
    at,
};
constexpr std::array job_move{revision, queue_id, queue_name};
constexpr std::array job_delete{revision};

constexpr std::string_view select_queue = "Supply exactly one of --id or --name.";
constexpr std::string_view job_uuid     = "UUID is the job ID.";

// Each canonical leaf owns its alias and dispatch strategy. Adding an alias cannot introduce a new wire method.
constexpr CommandSpec system_info_command{
    .group            = "system",
    .name             = "info",
    .alias            = {},
    .summary          = "Show daemon version and capabilities",
    .operands         = {},
    .maximum_operands = 0,
    .options          = {},
    .rules            = {},
    .example          = "jobuctl --socket /run/jobu.sock system info",
    .capability       = "system.info",
    .build            = parse_system_command,
};

constexpr CommandSpec queue_create_command{
    .group            = "queue",
    .name             = "create",
    .alias            = "add",
    .summary          = "Create a queue",
    .operands         = "NAME",
    .maximum_operands = 1,
    .options          = queue_create,
    .rules            = "NAME is required.",
    .example          = "jobuctl --socket /run/jobu.sock queue create reports",
    .capability       = "queue.create",
    .build            = parse_queue_command,
};

constexpr CommandSpec queue_get_command{
    .group            = "queue",
    .name             = "get",
    .alias            = {},
    .summary          = "Show a queue",
    .operands         = {},
    .maximum_operands = 0,
    .options          = queue_selector,
    .rules            = select_queue,
    .example          = "jobuctl --socket /run/jobu.sock queue get --name reports",
    .capability       = "queue.get",
    .build            = parse_queue_command,
};

constexpr CommandSpec queue_list_command{
    .group            = "queue",
    .name             = "list",
    .alias            = {},
    .summary          = "List queues",
    .operands         = {},
    .maximum_operands = 0,
    .options          = queue_list,
    .rules            = {},
    .example          = "jobuctl --socket /run/jobu.sock queue list --limit 20",
    .capability       = "queue.list",
    .build            = parse_queue_command,
};

constexpr CommandSpec queue_update_command{
    .group            = "queue",
    .name             = "update",
    .alias            = {},
    .summary          = "Update a queue",
    .operands         = {},
    .maximum_operands = 0,
    .options          = queue_update,
    .rules            = "Supply exactly one of --id or --name and at least one replacement field. Unspecified fields "
                        "remain unchanged.",
    .example          = "jobuctl --socket /run/jobu.sock queue update --name reports --weight 2",
    .capability       = "queue.update",
    .build            = parse_queue_command,
};

constexpr CommandSpec queue_suspend_command{
    .group            = "queue",
    .name             = "suspend",
    .alias            = {},
    .summary          = "Suspend a queue",
    .operands         = {},
    .maximum_operands = 0,
    .options          = queue_selector,
    .rules            = select_queue,
    .example          = "jobuctl --socket /run/jobu.sock queue suspend --name reports",
    .capability       = "queue.suspend",
    .build            = parse_queue_command,
};

constexpr CommandSpec queue_resume_command{
    .group            = "queue",
    .name             = "resume",
    .alias            = {},
    .summary          = "Resume a queue",
    .operands         = {},
    .maximum_operands = 0,
    .options          = queue_selector,
    .rules            = select_queue,
    .example          = "jobuctl --socket /run/jobu.sock queue resume --name reports",
    .capability       = "queue.resume",
    .build            = parse_queue_command,
};

constexpr CommandSpec queue_delete_command{
    .group            = "queue",
    .name             = "delete",
    .alias            = {},
    .summary          = "Delete a queue",
    .operands         = {},
    .maximum_operands = 0,
    .options          = queue_selector,
    .rules            = select_queue,
    .example          = "jobuctl --socket /run/jobu.sock queue delete --name reports",
    .capability       = "queue.delete",
    .build            = parse_queue_command,
};

constexpr CommandSpec job_create_command{
    .group            = "job",
    .name             = "create",
    .alias            = "add",
    .summary          = "Create a once-scheduled job",
    .operands         = {},
    .maximum_operands = 0,
    .options          = job_create,
    .rules      = "Require exactly one queue selector, --type, and --at. CLI requires --command; HTTP requires --url.\n"
                  "CLI and HTTP options cannot be mixed. Environment names must be unique across --env and --unset-env.\n"
                  "CLI environment defaults to empty apart from JobU-provided variables; arguments default to empty.",
    .example    = "jobuctl --socket /run/jobu.sock job create --queue-name reports --type cli \\\n"
                  "      --at 2030-01-01T00:00:00Z --command /bin/echo --arg=hello",
    .capability = "job.create",
    .build      = parse_job_command,
};

constexpr CommandSpec job_get_command{
    .group            = "job",
    .name             = "get",
    .alias            = {},
    .summary          = "Show a job",
    .operands         = "UUID",
    .maximum_operands = 1,
    .options          = {},
    .rules            = job_uuid,
    .example          = "jobuctl --socket /run/jobu.sock job get 00000000-0000-7000-8000-000000000001",
    .capability       = "job.get",
    .build            = parse_job_command,
};

constexpr CommandSpec job_list_command{
    .group            = "job",
    .name             = "list",
    .alias            = {},
    .summary          = "List jobs",
    .operands         = {},
    .maximum_operands = 0,
    .options          = job_list,
    .rules            = "Optionally select one queue; omit both queue selectors to list across queues.",
    .example          = "jobuctl --socket /run/jobu.sock job list --queue-name reports",
    .capability       = "job.list",
    .build            = parse_job_command,
};

constexpr CommandSpec job_update_command{
    .group            = "job",
    .name             = "update",
    .alias            = {},
    .summary          = "Update a job definition",
    .operands         = "UUID",
    .maximum_operands = 1,
    .options          = job_update,
    .rules = "Require UUID, --revision, and at least one replacement field. Unspecified fields remain unchanged.",
    .example =
        "jobuctl --socket /run/jobu.sock job update 00000000-0000-7000-8000-000000000001 --revision 1 --priority 2",
    .capability = "job.update",
    .build      = parse_job_command,
};

constexpr CommandSpec job_suspend_command{
    .group            = "job",
    .name             = "suspend",
    .alias            = {},
    .summary          = "Suspend a job",
    .operands         = "UUID",
    .maximum_operands = 1,
    .options          = {},
    .rules            = job_uuid,
    .example          = "jobuctl --socket /run/jobu.sock job suspend 00000000-0000-7000-8000-000000000001",
    .capability       = "job.suspend",
    .build            = parse_job_command,
};

constexpr CommandSpec job_resume_command{
    .group            = "job",
    .name             = "resume",
    .alias            = {},
    .summary          = "Resume a job",
    .operands         = "UUID",
    .maximum_operands = 1,
    .options          = {},
    .rules            = job_uuid,
    .example          = "jobuctl --socket /run/jobu.sock job resume 00000000-0000-7000-8000-000000000001",
    .capability       = "job.resume",
    .build            = parse_job_command,
};

constexpr CommandSpec job_move_command{
    .group            = "job",
    .name             = "move",
    .alias            = {},
    .summary          = "Move a job to another queue",
    .operands         = "UUID",
    .maximum_operands = 1,
    .options          = job_move,
    .rules            = "Require UUID, --revision, and exactly one of --queue-id or --queue-name.",
    .example          = "jobuctl --socket /run/jobu.sock job move 00000000-0000-7000-8000-000000000001 --revision 1 "
                        "--queue-name reports",
    .capability       = "job.move",
    .build            = parse_job_command,
};

constexpr CommandSpec job_delete_command{
    .group            = "job",
    .name             = "delete",
    .alias            = {},
    .summary          = "Delete a job",
    .operands         = "UUID",
    .maximum_operands = 1,
    .options          = job_delete,
    .rules            = "Require UUID and --revision.",
    .example          = "jobuctl --socket /run/jobu.sock job delete 00000000-0000-7000-8000-000000000001 --revision 1",
    .capability       = "job.delete",
    .build            = parse_job_command,
};

constexpr std::array commands{
    system_info_command,
    queue_create_command,
    queue_get_command,
    queue_list_command,
    queue_update_command,
    queue_suspend_command,
    queue_resume_command,
    queue_delete_command,
    job_create_command,
    job_get_command,
    job_list_command,
    job_update_command,
    job_suspend_command,
    job_resume_command,
    job_move_command,
    job_delete_command,
};

} // namespace

auto command_groups() -> std::span<GroupSpec const>
{
    return groups;
}

auto command_specs() -> std::span<CommandSpec const>
{
    return commands;
}

auto global_options() -> std::span<OptionSpec const>
{
    return globals;
}

auto find_group(std::string_view name) -> GroupSpec const*
{
    auto const* it = std::ranges::find(groups, name, &GroupSpec::name);
    return it == groups.end() ? nullptr : &*it;
}

auto find_command(std::string_view group, std::string_view action) -> CommandSpec const*
{
    for (auto const& command : commands) {
        if (command.group == group && (command.name == action || (!command.alias.empty() && command.alias == action))) {
            return &command;
        }
    }
    return nullptr;
}

auto find_option(std::span<OptionSpec const> options, std::string_view name) -> OptionSpec const*
{
    auto const it = std::ranges::find(options, name, [](OptionSpec const& spec) { return spec.option.long_name; });
    return it == options.end() ? nullptr : &*it;
}

auto lexical_options() -> std::vector<CommandLineOption>
{
    auto result = std::vector<CommandLineOption>{};
    auto append = [&](std::span<OptionSpec const> options) {
        for (auto const& spec : options) {
            if (std::ranges::find(result, spec.option.long_name, &CommandLineOption::long_name) == result.end()) {
                result.push_back(spec.option);
            }
        }
    };
    append(globals);
    for (auto const& command : commands) {
        append(command.options);
    }
    return result;
}

} // namespace jb::jobuctl::detail
