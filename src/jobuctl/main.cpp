#include "jobu_version_priv.hpp"

#include "application.hpp"
#include "attribute_registry.hpp"
#include "client.hpp"
#include "command_line_parser.hpp"
#include "local_socket.hpp"
#include "logging.hpp"
#include "management_json.hpp"
#include "protocol.hpp"
#include "system_info.hpp"
#include "timer.hpp"
#include "uuid.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace {

using namespace jb::core;
using namespace jb::jobu;
using namespace jb::rpc;

enum class CommandKind : std::uint8_t {
    SystemInfo,
    QueueCreate,
    QueueGet,
    QueueList,
    QueueUpdate,
    QueueSuspend,
    QueueResume,
    QueueDelete,
};

struct Command {
    std::filesystem::path        socket_path;
    CommandKind                  kind{CommandKind::SystemInfo};
    std::string_view             method{"system.info"};
    std::optional<JsonValue>     params;
    std::optional<QueueSelector> selector;
};

struct ParseResult {
    std::optional<Command> command;
    std::string            error;
};

struct SelectorResult {
    std::optional<QueueSelector> selector;
    std::string                  error;
};

enum class SessionPhase : std::uint8_t {
    Connecting,
    SystemInfo,
    Command,
    Finished,
};

constexpr std::array command_line_options{
    CommandLineOption{.long_name = "socket", .value_mode = CommandLineValueMode::Required},
    CommandLineOption{.long_name = "weight", .value_mode = CommandLineValueMode::Required},
    CommandLineOption{.long_name = "concurrency-limit", .value_mode = CommandLineValueMode::Required},
    CommandLineOption{.long_name = "recovery-policy", .value_mode = CommandLineValueMode::Required},
    CommandLineOption{.long_name = "idempotency-key", .value_mode = CommandLineValueMode::Required},
    CommandLineOption{.long_name = "id", .value_mode = CommandLineValueMode::Required},
    CommandLineOption{.long_name = "name", .value_mode = CommandLineValueMode::Required},
    CommandLineOption{.long_name = "include-deleted"},
    CommandLineOption{.long_name = "limit", .value_mode = CommandLineValueMode::Required},
    CommandLineOption{.long_name = "after", .value_mode = CommandLineValueMode::Required},
    CommandLineOption{.long_name = "new-name", .value_mode = CommandLineValueMode::Required},
};

auto parse_failure(std::string message) -> ParseResult
{
    return {.command = std::nullopt, .error = std::move(message)};
}

auto selector_failure(std::string message) -> SelectorResult
{
    return {.selector = std::nullopt, .error = std::move(message)};
}

auto option_value(CommandLineArgument const& argument) -> std::optional<std::string_view>
{
    if (argument.kind() != CommandLineArgumentKind::Option || !argument.known() || argument.missing_value() ||
        !argument.value() || argument.value()->empty()) {
        return std::nullopt;
    }
    return argument.value();
}

auto parse_unsigned(std::string_view text, std::uint64_t minimum, std::uint64_t maximum) -> std::optional<std::uint64_t>
{
    auto value  = std::uint64_t{};
    auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || value < minimum || value > maximum) {
        return std::nullopt;
    }
    return value;
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
                           std::span<CommandLineArgument const> arguments) -> ParseResult
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
                        StandardAttributeRegistry const&     registry) -> ParseResult
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

auto parse_queue_list(std::filesystem::path socket_path, std::span<CommandLineArgument const> arguments) -> ParseResult
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
                        StandardAttributeRegistry const&     registry) -> ParseResult
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

auto parse_command_line(int argc, char* argv[], StandardAttributeRegistry const& registry) -> ParseResult
{
    CommandLineParser parser{argc, argv, command_line_options};
    auto const&       arguments = parser.arguments();
    if (arguments.size() < 3U) {
        return parse_failure("expected --socket PATH followed by a command");
    }

    auto const socket = option_value(arguments[0]);
    if (!socket || arguments[0].name() != "socket") {
        return parse_failure("--socket PATH must be the first argument");
    }
    auto socket_path = std::filesystem::path{std::string{*socket}};

    if (arguments[1].kind() != CommandLineArgumentKind::Positional ||
        arguments[2].kind() != CommandLineArgumentKind::Positional) {
        return parse_failure("expected a command family and action");
    }

    auto const family = arguments[1].token();
    auto const action = arguments[2].token();
    if (family == "system" && action == "info" && arguments.size() == 3U) {
        return {
            .command =
                Command{
                        .socket_path = std::move(socket_path),
                        .kind        = CommandKind::SystemInfo,
                        .method      = "system.info",
                        },
            .error = {},
        };
    }
    if (family != "queue") {
        return parse_failure("unknown command");
    }

    auto remaining = std::span<CommandLineArgument const>{arguments}.subspan(3);
    if (action == "create") {
        return parse_queue_create(std::move(socket_path), remaining, registry);
    }
    if (action == "get") {
        return make_selector_command(std::move(socket_path), CommandKind::QueueGet, "queue.get", remaining);
    }
    if (action == "list") {
        return parse_queue_list(std::move(socket_path), remaining);
    }
    if (action == "update") {
        return parse_queue_update(std::move(socket_path), remaining, registry);
    }
    if (action == "suspend") {
        return make_selector_command(std::move(socket_path), CommandKind::QueueSuspend, "queue.suspend", remaining);
    }
    if (action == "resume") {
        return make_selector_command(std::move(socket_path), CommandKind::QueueResume, "queue.resume", remaining);
    }
    if (action == "delete") {
        return make_selector_command(std::move(socket_path), CommandKind::QueueDelete, "queue.delete", remaining);
    }
    return parse_failure("unknown queue action");
}

void print_usage()
{
    fmt::print(stderr,
               "Usage:\n"
               "  jobuctl --socket PATH system info\n"
               "  jobuctl --socket PATH queue create NAME [--weight N] [--concurrency-limit N]\n"
               "      [--recovery-policy fail_interrupted|retry_interrupted] [--idempotency-key KEY]\n"
               "  jobuctl --socket PATH queue get (--id UUID | --name NAME)\n"
               "  jobuctl --socket PATH queue list [--include-deleted] [--limit N] [--after UUID]\n"
               "  jobuctl --socket PATH queue update (--id UUID | --name NAME)\n"
               "      [--new-name NAME] [--weight N] [--concurrency-limit N]\n"
               "  jobuctl --socket PATH queue suspend (--id UUID | --name NAME)\n"
               "  jobuctl --socket PATH queue resume (--id UUID | --name NAME)\n"
               "  jobuctl --socket PATH queue delete (--id UUID | --name NAME)\n");
}

void print_system_info(SystemInfo const& info)
{
    fmt::print(stdout, "Daemon version: {}\n", info.daemon_version);
    fmt::print(stdout, "API version: {}.{}\n", info.api_version.major, info.api_version.minor);
    fmt::print(stdout, "Capabilities:\n");
    for (auto const& capability : info.capabilities) {
        fmt::print(stdout, "  {}\n", capability);
    }
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

auto print_command_result(Command const& command, JsonValue const& value, StandardAttributeRegistry const& registry)
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

auto application_error_code(RpcError const& error) -> std::optional<std::string_view>
{
    if (error.code != static_cast<std::int64_t>(ErrorCode::ApplicationError) || !error.data ||
        !error.data->is_object()) {
        return std::nullopt;
    }
    auto const& object = error.data->as_object();
    auto const  member = object.find("code");
    if (member == object.end() || !member->second.is_string()) {
        return std::nullopt;
    }
    return member->second.as_string();
}

} // anonymous namespace

auto main(int argc, char* argv[]) -> int
{
    using namespace jb::core;
    using namespace jb::jobu;
    using namespace jb::net;
    using namespace jb::rpc;
    using namespace std::chrono_literals;

    if (argc == 2 && std::string_view{argv[1]} == "--version") {
        fmt::print(stdout, "jobuctl {}\n", jb::jobu::detail::project_version);
        return EXIT_SUCCESS;
    }

    StandardAttributeRegistry registry;
    auto                      parsed = parse_command_line(argc, argv, registry);
    if (!parsed.command) {
        fmt::print(stderr, "jobuctl: {}\n", parsed.error);
        print_usage();
        return EXIT_FAILURE;
    }
    auto command = std::move(*parsed.command);

    Application             app{0, nullptr};
    LocalSocket             socket;
    std::unique_ptr<Client> client;
    Timer                   timeout;
    auto                    finished = false;
    auto                    phase    = SessionPhase::Connecting;

    auto finish = [&](int code) -> void {
        if (finished) {
            return;
        }
        finished = true;
        phase    = SessionPhase::Finished;
        timeout.stop();
        static_cast<void>(app.quit(code));
    };
    auto finish_operator_error = [&finish, &finished](std::string_view message) -> void {
        if (finished) {
            return;
        }
        fmt::print(stderr, "jobuctl: {}\n", message);
        finish(EXIT_FAILURE);
    };
    auto finish_call_error = [&finish, &finished](std::string_view method, Error const& error) -> void {
        if (finished) {
            return;
        }
        log_error("Unable to send the {} request: {} ({})", method, error.message, error.code);
        finish(EXIT_FAILURE);
    };

    socket.set_read_buffer_limit(2U * 1024U * 1024U);
    socket.error_occurred.connect(
        [&finish_operator_error](IOError, std::string const& message) -> void { finish_operator_error(message); });
    socket.connected.connect([&]() -> void {
        client = std::make_unique<Client>(socket);
        client->result_received.connect([&](RequestId const&, JsonValue const& value) -> void {
            if (finished) {
                return;
            }
            if (phase == SessionPhase::SystemInfo) {
                auto info = system_info_from_json(value);
                if (!info) {
                    log_error("Invalid system.info response: {} ({})", info.error().message, info.error().code);
                    finish(EXIT_FAILURE);
                    return;
                }
                if (command.kind == CommandKind::SystemInfo) {
                    print_system_info(info.value());
                    finish(EXIT_SUCCESS);
                    return;
                }
                if (info->api_version.major != 1U) {
                    finish_operator_error("the daemon uses an incompatible API major version");
                    return;
                }
                if (std::find(info->capabilities.begin(), info->capabilities.end(), command.method) ==
                    info->capabilities.end()) {
                    auto message = fmt::format("the daemon does not advertise {}", command.method);
                    finish_operator_error(message);
                    return;
                }

                phase     = SessionPhase::Command;
                auto call = client->call(command.method, command.params);
                if (!call) {
                    finish_call_error(command.method, call.error());
                }
                return;
            }
            if (phase != SessionPhase::Command) {
                log_error("Received an RPC result in an invalid command-session phase");
                finish(EXIT_FAILURE);
                return;
            }
            if (!print_command_result(command, value, registry)) {
                finish(EXIT_FAILURE);
                return;
            }
            finish(EXIT_SUCCESS);
        });
        client->error_received.connect([&](RequestId const&, RpcError const& error) -> void {
            if (finished) {
                return;
            }
            if (auto const code = application_error_code(error)) {
                fmt::print(stderr, "jobuctl: {} ({})\n", error.message, *code);
            }
            else {
                fmt::print(stderr, "jobuctl: remote RPC error {}: {}\n", error.code, error.message);
            }
            finish(EXIT_FAILURE);
        });
        client->request_failed.connect([&finish_operator_error](RequestId const&, Error const& error) -> void {
            finish_operator_error(error.message);
        });
        client->protocol_error.connect([&finish, &finished](Error const& error) -> void {
            if (finished) {
                return;
            }
            log_error("RPC protocol error: {} ({})", error.message, error.code);
            finish(EXIT_FAILURE);
        });

        phase     = SessionPhase::SystemInfo;
        auto call = client->call("system.info");
        if (!call) {
            finish_call_error("system.info", call.error());
        }
    });
    timeout.timeout.connect([&]() -> void {
        auto const method  = phase == SessionPhase::Command ? command.method : std::string_view{"system.info"};
        auto       message = fmt::format("{} request timed out", method);
        finish_operator_error(message);
    });

    timeout.start(5s);
    socket.connect_to_server(command.socket_path);
    return app.exec();
}
