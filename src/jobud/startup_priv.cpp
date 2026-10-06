#include "startup_priv.hpp"

#include "command_line_parser.hpp"
#include "configuration_file_priv.hpp"
#include "cron.hpp"
#include "http/url_validation.hpp"
#include "jobu_paths_priv.hpp"
#include "text_validation.hpp"

#include <array>
#include <charconv>
#include <fstream>
#include <grp.h>
#include <pwd.h>
#include <string_view>
#include <sys/un.h>
#include <system_error>
#include <utility>

namespace jb::jobud::detail {
namespace {

auto invalid_arguments() -> StartupError
{
    return {.code = "jobud.startup.invalid_arguments", .message = "Daemon command line is invalid"};
}

auto invalid_option(std::string_view key) -> StartupError
{
    return {.code = "jobud.config.invalid", .key = std::string{key}, .message = "Daemon startup setting is invalid"};
}

auto parse_positive_uint32(std::string_view value) -> std::optional<std::uint32_t>
{
    auto parsed = std::uint32_t{};
    auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || parsed == 0U) {
        return std::nullopt;
    }
    return parsed;
}

auto assign_path(std::optional<std::filesystem::path>& target, std::string_view value) -> bool
{
    if (target || value.empty() || jb::core::has_ascii_control(value)) {
        return false;
    }
    target = std::filesystem::path{value};
    return true;
}

auto assign_text(std::optional<std::string>& target, std::string_view value) -> bool
{
    if (target || value.empty() || jb::core::has_ascii_control(value)) {
        return false;
    }
    target = value;
    return true;
}

auto absolute_cli_path(std::filesystem::path const& path, std::filesystem::path const& directory)
    -> std::filesystem::path
{
    // Keep ".." components: removing them before traversal changes the target after a symlink.
    return path.is_absolute() ? path : directory / path;
}

auto valid_path(std::filesystem::path const& path) -> bool
{
    return path.is_absolute() && !path.empty() && !jb::core::has_ascii_control(path.native());
}

auto valid_socket_path(std::filesystem::path const& path) -> bool
{
    sockaddr_un address{};
    return valid_path(path) && path.native().size() < sizeof(address.sun_path);
}

auto readable_regular_file(std::filesystem::path const& path) -> bool
{
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error) {
        return false;
    }
    std::ifstream file{path};
    return file.good();
}

} // anonymous namespace

auto compiled_paths() -> CompiledPaths
{
    return {.config   = jb::jobu::detail::default_config_path,
            .database = jb::jobu::detail::default_database_path,
            .socket   = jb::jobu::detail::default_socket_path};
}

auto parse_startup_arguments(int argc, char const* const argv[]) -> jb::core::Result<StartupArguments, StartupError>
{
    using namespace jb::core;
    using ParseResult = Result<StartupArguments, StartupError>;

    constexpr std::array options{
        CommandLineOption{.long_name = "config",               .value_mode = CommandLineValueMode::Required},
        CommandLineOption{.long_name = "no-config",            .value_mode = CommandLineValueMode::None    },
        CommandLineOption{.long_name = "check-config",         .value_mode = CommandLineValueMode::None    },
        CommandLineOption{.long_name = "help",                 .value_mode = CommandLineValueMode::None    },
        CommandLineOption{.long_name = "version",              .value_mode = CommandLineValueMode::None    },
        CommandLineOption{.long_name = "socket",               .value_mode = CommandLineValueMode::Required},
        CommandLineOption{.long_name = "database",             .value_mode = CommandLineValueMode::Required},
        CommandLineOption{.long_name = "cli-concurrency",      .value_mode = CommandLineValueMode::Required},
        CommandLineOption{.long_name = "http-concurrency",     .value_mode = CommandLineValueMode::Required},
        CommandLineOption{.long_name = "http-proxy",           .value_mode = CommandLineValueMode::Required},
        CommandLineOption{.long_name = "http-ca-bundle",       .value_mode = CommandLineValueMode::Required},
        CommandLineOption{.long_name = "run-as-user",          .value_mode = CommandLineValueMode::Required},
        CommandLineOption{.long_name = "run-as-group",         .value_mode = CommandLineValueMode::Required},
        CommandLineOption{.long_name = "allow-root-cli",       .value_mode = CommandLineValueMode::None    },
        CommandLineOption{.long_name = "no-allow-root-cli",    .value_mode = CommandLineValueMode::None    },
        CommandLineOption{.long_name = "allow-root-daemon",    .value_mode = CommandLineValueMode::None    },
        CommandLineOption{.long_name = "no-allow-root-daemon", .value_mode = CommandLineValueMode::None    },
    };
    CommandLineParser parser{argc, argv, options};
    StartupArguments  parsed;

    for (auto const& argument : parser) {
        if (argument.kind() != CommandLineArgumentKind::Option || !argument.known() || argument.missing_value()) {
            return ParseResult::failure(invalid_arguments());
        }

        auto const name  = argument.name();
        auto const value = argument.value();
        if (name == "no-config" || name == "check-config" || name == "help" || name == "version" ||
            name == "allow-root-cli" || name == "no-allow-root-cli" || name == "allow-root-daemon" ||
            name == "no-allow-root-daemon") {
            if (value) {
                return ParseResult::failure(invalid_arguments());
            }
            if (name == "no-config") {
                if (parsed.no_config || parsed.config_path) {
                    return ParseResult::failure(invalid_arguments());
                }
                parsed.no_config = true;
            }
            else if (name == "check-config" || name == "help" || name == "version") {
                if (parsed.action != StartupAction::Run) {
                    return ParseResult::failure(invalid_arguments());
                }
                if (name == "check-config") {
                    parsed.action = StartupAction::CheckConfig;
                }
                else if (name == "help") {
                    parsed.action = StartupAction::Help;
                }
                else {
                    parsed.action = StartupAction::Version;
                }
            }
            else if (name == "allow-root-cli" || name == "no-allow-root-cli") {
                if (parsed.allow_root_cli) {
                    return ParseResult::failure(invalid_arguments());
                }
                parsed.allow_root_cli = name == "allow-root-cli";
            }
            else {
                if (parsed.allow_root_daemon) {
                    return ParseResult::failure(invalid_arguments());
                }
                parsed.allow_root_daemon = name == "allow-root-daemon";
            }
            continue;
        }

        if (!value || value->empty()) {
            return ParseResult::failure(invalid_arguments());
        }
        auto accepted = false;
        if (name == "config") {
            accepted = !parsed.no_config && assign_path(parsed.config_path, *value);
        }
        else if (name == "socket") {
            accepted = assign_path(parsed.socket_path, *value);
        }
        else if (name == "database") {
            accepted = assign_path(parsed.database_path, *value);
        }
        else if (name == "http-ca-bundle") {
            accepted = assign_path(parsed.http_ca_bundle, *value);
        }
        else if (name == "run-as-user") {
            accepted = assign_text(parsed.run_as_user, *value);
        }
        else if (name == "run-as-group") {
            accepted = assign_text(parsed.run_as_group, *value);
        }
        else if (name == "http-proxy") {
            accepted = assign_text(parsed.http_proxy, *value);
        }
        else if (name == "cli-concurrency" || name == "http-concurrency") {
            auto& target = name == "cli-concurrency" ? parsed.cli_concurrency : parsed.http_concurrency;
            if (!target) {
                target   = parse_positive_uint32(*value);
                accepted = target.has_value();
            }
        }
        if (!accepted) {
            return ParseResult::failure(invalid_arguments());
        }
    }
    return ParseResult::success(std::move(parsed));
}

auto load_configuration(StartupArguments const&      arguments,
                        CompiledPaths const&         paths,
                        std::filesystem::path const& invocation_directory)
    -> jb::core::Result<LoadedConfiguration, StartupError>
{
    using LoadResult = jb::core::Result<LoadedConfiguration, StartupError>;
    if (arguments.no_config) {
        return LoadResult::success(LoadedConfiguration{});
    }

    auto const explicit_path = arguments.config_path.has_value();
    auto const path = explicit_path ? absolute_cli_path(*arguments.config_path, invocation_directory) : paths.config;
    if (!valid_path(path)) {
        return LoadResult::failure(invalid_option("config"));
    }
    auto read = read_configuration_file(path, !explicit_path);
    if (!read) {
        return LoadResult::failure(std::move(read).error());
    }
    if (!*read) {
        return LoadResult::success(LoadedConfiguration{});
    }

    auto parsed = parse_configuration_text(**read);
    if (!parsed) {
        return LoadResult::failure(std::move(parsed).error());
    }
    return LoadResult::success({.input = std::move(parsed).value(), .source_path = path});
}

auto resolve_startup_options(StartupArguments const&      arguments,
                             ConfigurationInput const&    input,
                             CompiledPaths const&         paths,
                             std::filesystem::path const& invocation_directory)
    -> jb::core::Result<StartupOptions, StartupError>
{
    using ResolveResult = jb::core::Result<StartupOptions, StartupError>;

    // Materialize the complete configuration layer before applying explicit flags.
    StartupOptions options;
    options.socket_path   = input.socket_path.value_or(paths.socket);
    options.database_path = input.database_path.value_or(paths.database);
    options.socket_owner  = input.socket_owner;
    options.socket_group  = input.socket_group;
    options.socket_mode   = input.socket_mode.value_or(options.socket_mode);

    options.cli_concurrency   = input.cli_concurrency.value_or(options.cli_concurrency);
    options.http_concurrency  = input.http_concurrency.value_or(options.http_concurrency);
    options.allow_root_cli    = input.allow_root_cli.value_or(false);
    options.allow_root_daemon = input.allow_root_daemon.value_or(false);
    options.run_as_user       = input.run_as_user;
    options.run_as_group      = input.run_as_group;

    options.http_proxy     = input.http_proxy;
    options.http_ca_bundle = input.http_ca_bundle;

    options.default_timezone       = input.default_timezone.value_or(options.default_timezone);
    options.default_retention      = input.default_retention.value_or(options.default_retention);
    options.history_sweep_interval = input.history_sweep_interval.value_or(options.history_sweep_interval);
    options.history_batch_size     = input.history_batch_size.value_or(options.history_batch_size);
    options.telemetry_checkpoint_interval =
        input.telemetry_checkpoint_interval.value_or(options.telemetry_checkpoint_interval);

    options.rpc_header_limit_bytes  = input.rpc_header_limit_bytes.value_or(options.rpc_header_limit_bytes);
    options.rpc_body_limit_bytes    = input.rpc_body_limit_bytes.value_or(options.rpc_body_limit_bytes);
    options.rpc_max_batch_entries   = input.rpc_max_batch_entries.value_or(options.rpc_max_batch_entries);
    options.rpc_max_connections     = input.rpc_max_connections.value_or(options.rpc_max_connections);
    options.rpc_queued_output_bytes = input.rpc_queued_output_bytes.value_or(options.rpc_queued_output_bytes);
    options.rpc_read_buffer_capacity =
        input.rpc_read_buffer_capacity == 0U ? options.rpc_read_buffer_capacity : input.rpc_read_buffer_capacity;

    options.logging_level   = input.logging_level.value_or(options.logging_level);
    options.logging_format  = input.logging_format.value_or(options.logging_format);
    options.daemon_defaults = input.daemon_defaults;

    if (arguments.socket_path) {
        options.socket_path = absolute_cli_path(*arguments.socket_path, invocation_directory);
    }
    if (arguments.database_path) {
        options.database_path = absolute_cli_path(*arguments.database_path, invocation_directory);
    }
    if (arguments.cli_concurrency) {
        options.cli_concurrency = *arguments.cli_concurrency;
    }
    if (arguments.http_concurrency) {
        options.http_concurrency = *arguments.http_concurrency;
    }
    if (arguments.allow_root_cli) {
        options.allow_root_cli = *arguments.allow_root_cli;
    }
    if (arguments.allow_root_daemon) {
        options.allow_root_daemon = *arguments.allow_root_daemon;
    }
    if (arguments.run_as_user) {
        options.run_as_user = arguments.run_as_user;
    }
    if (arguments.run_as_group) {
        options.run_as_group = arguments.run_as_group;
    }
    if (arguments.http_proxy) {
        options.http_proxy = arguments.http_proxy;
    }
    if (arguments.http_ca_bundle) {
        options.http_ca_bundle = absolute_cli_path(*arguments.http_ca_bundle, invocation_directory);
    }

    // These checks use the merged values, so a CLI user can complete a config-supplied group setting.
    if (!valid_socket_path(options.socket_path)) {
        return ResolveResult::failure(invalid_option("socket.path"));
    }
    if (!valid_path(options.database_path)) {
        return ResolveResult::failure(invalid_option("database.path"));
    }
    if (options.run_as_group && !options.run_as_user) {
        return ResolveResult::failure(invalid_option("daemon.run_as_group"));
    }
    if (options.http_proxy && !jb::net::http::validate_url(*options.http_proxy)) {
        return ResolveResult::failure(invalid_option("http.proxy"));
    }
    if (options.http_ca_bundle &&
        (!valid_path(*options.http_ca_bundle) || !readable_regular_file(*options.http_ca_bundle))) {
        return ResolveResult::failure(invalid_option("http.ca_bundle"));
    }
    jb::jobu::SystemCronEngine cron;
    if (!cron.validate({.expression = "* * * * *", .timezone = options.default_timezone})) {
        return ResolveResult::failure(invalid_option("schedule.default_timezone"));
    }
    return ResolveResult::success(std::move(options));
}

auto validate_readonly_accounts(StartupOptions const& options) -> jb::core::Result<void, StartupError>
{
    using ValidationResult = jb::core::Result<void, StartupError>;
    // Local validation checks names only. The running daemon authorizes and verifies its final identity separately.
    if (options.run_as_user && ::getpwnam(options.run_as_user->c_str()) == nullptr) {
        return ValidationResult::failure(invalid_option("daemon.run_as_user"));
    }
    if (options.socket_owner && ::getpwnam(options.socket_owner->c_str()) == nullptr) {
        return ValidationResult::failure(invalid_option("socket.owner"));
    }
    if (options.run_as_group && ::getgrnam(options.run_as_group->c_str()) == nullptr) {
        return ValidationResult::failure(invalid_option("daemon.run_as_group"));
    }
    if (options.socket_group && ::getgrnam(options.socket_group->c_str()) == nullptr) {
        return ValidationResult::failure(invalid_option("socket.group"));
    }
    return ValidationResult::success();
}

auto scheduler_options(StartupOptions const& startup) -> jb::jobu::SchedulerOptions
{
    return {.cli_concurrency = startup.cli_concurrency, .http_concurrency = startup.http_concurrency};
}

} // namespace jb::jobud::detail
