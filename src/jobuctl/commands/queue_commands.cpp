#include "commands_priv.hpp"

#include "command_helpers_priv.hpp"
#include "logging.hpp"
#include "management_json.hpp"

#include <fmt/format.h>

#include <cstdio>
#include <limits>
#include <utility>
#include <variant>

namespace jb::jobuctl::detail {

using namespace jb::core;
using namespace jb::jobu;

namespace {

struct SelectorResult {
    std::optional<QueueSelector> selector;
    std::string                  error;
};

auto selector_failure(std::string message) -> SelectorResult
{
    return {.selector = std::nullopt, .error = std::move(message)};
}

auto add_selector(CommandLineArgument const& argument, std::optional<QueueSelector>& selector) -> bool
{
    if (selector || (argument.name() != "id" && argument.name() != "name")) {
        return false;
    }
    auto const value = option_value(argument);
    if (!value) {
        return false;
    }

    if (argument.name() == "id") {
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

auto parse_selector(std::span<CommandLineArgument const> arguments) -> SelectorResult
{
    auto selector = std::optional<QueueSelector>{};
    for (auto const& argument : arguments) {
        if (!add_selector(argument, selector)) {
            return selector_failure("expected exactly one valid --id or --name selector");
        }
    }
    if (!selector) {
        return selector_failure("expected exactly one --id or --name selector");
    }
    return {.selector = std::move(selector), .error = {}};
}

auto make_selector_command(std::filesystem::path                socket_path,
                           CommandKind                          kind,
                           std::string_view                     method,
                           std::span<CommandLineArgument const> arguments) -> CommandBuildResult
{
    auto selector = parse_selector(arguments);
    if (!selector.selector) {
        return parse_failure(std::move(selector.error));
    }

    auto params = queue_selector_to_json(*selector.selector);
    if (!params) {
        return parse_failure(params.error().message);
    }
    return {
        .command =
            Command{
                    .socket_path = std::move(socket_path),
                    .kind        = kind,
                    .method      = method,
                    .params      = std::move(params).value(),
                    .selector    = std::move(selector.selector),
                    },
        .error = {},
    };
}

auto parse_queue_create(std::filesystem::path                socket_path,
                        std::span<CommandLineArgument const> arguments,
                        StandardAttributeRegistry const&     registry) -> CommandBuildResult
{
    if (arguments.empty() || arguments.front().kind() != CommandLineArgumentKind::Positional ||
        arguments.front().token().empty()) {
        return parse_failure("queue create requires a name");
    }

    auto request          = CreateQueueRequest{.name = std::string{arguments.front().token()}};
    auto weight_seen      = false;
    auto concurrency_seen = false;
    auto recovery_seen    = false;
    auto idempotency_seen = false;

    for (auto const& argument : arguments.subspan(1)) {
        auto const value = option_value(argument);
        if (!value) {
            return parse_failure("queue create has an invalid option");
        }

        if (argument.name() == "weight" && !weight_seen) {
            auto parsed = parse_unsigned(*value, 1, std::numeric_limits<std::uint32_t>::max());
            if (!parsed) {
                return parse_failure("--weight must be an integer from 1 through 4294967295");
            }
            request.weight = static_cast<std::uint32_t>(*parsed);
            weight_seen    = true;
            continue;
        }
        if (argument.name() == "concurrency-limit" && !concurrency_seen) {
            auto parsed = parse_unsigned(*value, 1, std::numeric_limits<std::uint32_t>::max());
            if (!parsed) {
                return parse_failure("--concurrency-limit must be an integer from 1 through 4294967295");
            }
            request.concurrency_limit = static_cast<std::uint32_t>(*parsed);
            concurrency_seen          = true;
            continue;
        }
        if (argument.name() == "recovery-policy" && !recovery_seen) {
            if (*value == "fail_interrupted") {
                request.recovery_policy = RecoveryPolicy::FailInterrupted;
            }
            else if (*value == "retry_interrupted") {
                request.recovery_policy = RecoveryPolicy::RetryInterrupted;
            }
            else {
                return parse_failure("--recovery-policy must be fail_interrupted or retry_interrupted");
            }
            recovery_seen = true;
            continue;
        }
        if (argument.name() == "idempotency-key" && !idempotency_seen) {
            request.idempotency_key = std::string{*value};
            idempotency_seen        = true;
            continue;
        }
        return parse_failure("queue create has an unknown or duplicate option");
    }

    auto params = create_queue_request_to_json(request, registry);
    if (!params) {
        return parse_failure(params.error().message);
    }
    return {
        .command =
            Command{
                    .socket_path = std::move(socket_path),
                    .kind        = CommandKind::QueueCreate,
                    .method      = "queue.create",
                    .params      = std::move(params).value(),
                    },
        .error = {},
    };
}

auto parse_queue_list(std::filesystem::path socket_path, std::span<CommandLineArgument const> arguments)
    -> CommandBuildResult
{
    auto request              = QueueListRequest{};
    auto include_deleted_seen = false;
    auto limit_seen           = false;
    auto after_seen           = false;

    for (auto const& argument : arguments) {
        if (argument.kind() != CommandLineArgumentKind::Option || !argument.known()) {
            return parse_failure("queue list has an invalid option");
        }
        if (argument.name() == "include-deleted" && !include_deleted_seen && !argument.has_value()) {
            request.include_deleted = true;
            include_deleted_seen    = true;
            continue;
        }

        auto const value = option_value(argument);
        if (!value) {
            return parse_failure("queue list has an invalid option value");
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
        return parse_failure("queue list has an unknown or duplicate option");
    }

    auto params = queue_list_request_to_json(request);
    if (!params) {
        return parse_failure(params.error().message);
    }
    return {
        .command =
            Command{
                    .socket_path = std::move(socket_path),
                    .kind        = CommandKind::QueueList,
                    .method      = "queue.list",
                    .params      = std::move(params).value(),
                    },
        .error = {},
    };
}

auto parse_queue_update(std::filesystem::path                socket_path,
                        std::span<CommandLineArgument const> arguments,
                        StandardAttributeRegistry const&     registry) -> CommandBuildResult
{
    auto selector         = std::optional<QueueSelector>{};
    auto new_name         = std::optional<std::string>{};
    auto weight           = std::optional<std::uint32_t>{};
    auto concurrency      = std::optional<std::uint32_t>{};
    auto new_name_seen    = false;
    auto weight_seen      = false;
    auto concurrency_seen = false;

    for (auto const& argument : arguments) {
        if (argument.name() == "id" || argument.name() == "name") {
            if (!add_selector(argument, selector)) {
                return parse_failure("queue update requires exactly one valid --id or --name selector");
            }
            continue;
        }

        auto const value = option_value(argument);
        if (!value) {
            return parse_failure("queue update has an invalid option");
        }
        if (argument.name() == "new-name" && !new_name_seen) {
            new_name      = std::string{*value};
            new_name_seen = true;
            continue;
        }
        if (argument.name() == "weight" && !weight_seen) {
            auto parsed = parse_unsigned(*value, 1, std::numeric_limits<std::uint32_t>::max());
            if (!parsed) {
                return parse_failure("--weight must be an integer from 1 through 4294967295");
            }
            weight      = static_cast<std::uint32_t>(*parsed);
            weight_seen = true;
            continue;
        }
        if (argument.name() == "concurrency-limit" && !concurrency_seen) {
            auto parsed = parse_unsigned(*value, 1, std::numeric_limits<std::uint32_t>::max());
            if (!parsed) {
                return parse_failure("--concurrency-limit must be an integer from 1 through 4294967295");
            }
            concurrency      = static_cast<std::uint32_t>(*parsed);
            concurrency_seen = true;
            continue;
        }
        return parse_failure("queue update has an unknown or duplicate option");
    }

    if (!selector) {
        return parse_failure("queue update requires exactly one --id or --name selector");
    }
    if (!new_name && !weight && !concurrency) {
        return parse_failure("queue update requires at least one mutable field");
    }

    auto request = UpdateQueueRequest{
        .queue             = std::move(*selector),
        .name              = std::move(new_name),
        .weight            = weight,
        .concurrency_limit = concurrency,
    };
    auto params = update_queue_request_to_json(request, registry);
    if (!params) {
        return parse_failure(params.error().message);
    }
    return {
        .command =
            Command{
                    .socket_path = std::move(socket_path),
                    .kind        = CommandKind::QueueUpdate,
                    .method      = "queue.update",
                    .params      = std::move(params).value(),
                    },
        .error = {},
    };
}

auto queue_state_text(QueueState state) noexcept -> std::string_view
{
    switch (state) {
        case QueueState::Active:
            return "active";
        case QueueState::Suspending:
            return "suspending";
        case QueueState::Suspended:
            return "suspended";
        case QueueState::Deleted:
            return "deleted";
    }
    return "unknown";
}

auto recovery_policy_text(RecoveryPolicy policy) noexcept -> std::string_view
{
    switch (policy) {
        case RecoveryPolicy::FailInterrupted:
            return "fail_interrupted";
        case RecoveryPolicy::RetryInterrupted:
            return "retry_interrupted";
    }
    return "unknown";
}

void print_queue(Queue const& queue)
{
    fmt::print(stdout,
               "Queue {}: name={}, state={}, weight={}, concurrency_limit={}, recovery_policy={}\n",
               queue.id.to_string(),
               queue.name,
               queue_state_text(queue.state),
               queue.weight,
               queue.concurrency_limit,
               recovery_policy_text(queue.recovery_policy));
}

void print_queue_page(QueuePage const& page)
{
    if (page.items.empty()) {
        fmt::print(stdout, "No queues\n");
    }
    for (auto const& queue : page.items) {
        print_queue(queue);
    }
    if (page.next_after_id) {
        fmt::print(stdout, "Next after: {}\n", page.next_after_id->to_string());
    }
}

void print_deleted_selector(QueueSelector const& selector)
{
    if (auto const* id = std::get_if<Uuid>(&selector)) {
        fmt::print(stdout, "Deleted queue id={}\n", id->to_string());
        return;
    }
    fmt::print(stdout, "Deleted queue name={}\n", std::get<std::string>(selector));
}

} // namespace

auto parse_queue_command(std::filesystem::path                socket_path,
                         std::string_view                     action,
                         std::span<CommandLineArgument const> arguments,
                         StandardAttributeRegistry const&     registry) -> CommandBuildResult
{
    if (action == "create") {
        return parse_queue_create(std::move(socket_path), arguments, registry);
    }
    if (action == "get") {
        return make_selector_command(std::move(socket_path), CommandKind::QueueGet, "queue.get", arguments);
    }
    if (action == "list") {
        return parse_queue_list(std::move(socket_path), arguments);
    }
    if (action == "update") {
        return parse_queue_update(std::move(socket_path), arguments, registry);
    }
    if (action == "suspend") {
        return make_selector_command(std::move(socket_path), CommandKind::QueueSuspend, "queue.suspend", arguments);
    }
    if (action == "resume") {
        return make_selector_command(std::move(socket_path), CommandKind::QueueResume, "queue.resume", arguments);
    }
    if (action == "delete") {
        return make_selector_command(std::move(socket_path), CommandKind::QueueDelete, "queue.delete", arguments);
    }
    return parse_failure("unknown queue action");
}

auto print_queue_result(Command const& command, JsonValue const& value, StandardAttributeRegistry const& registry)
    -> bool
{
    if (command.kind == CommandKind::QueueList) {
        auto page = queue_page_from_json(value, registry);
        if (!page) {
            log_error("Invalid {} response: {} ({})", command.method, page.error().message, page.error().code);
            return false;
        }
        print_queue_page(page.value());
        return true;
    }
    if (command.kind == CommandKind::QueueDelete) {
        if (!value.is_null() || !command.selector) {
            log_error("Invalid {} response", command.method);
            return false;
        }
        print_deleted_selector(*command.selector);
        return true;
    }

    auto queue = queue_from_json(value, registry);
    if (!queue) {
        log_error("Invalid {} response: {} ({})", command.method, queue.error().message, queue.error().code);
        return false;
    }
    print_queue(queue.value());
    return true;
}

} // namespace jb::jobuctl::detail
