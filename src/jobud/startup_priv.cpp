#include "startup_priv.hpp"

#include "command_line_parser.hpp"

#include <array>
#include <charconv>
#include <string_view>
#include <system_error>
#include <utility>

namespace jb::jobud::detail {
namespace {

auto parse_positive_uint32(std::string_view value) -> std::optional<std::uint32_t>
{
    auto parsed = std::uint32_t{};
    auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || parsed == 0U) {
        return std::nullopt;
    }
    return parsed;
}

} // anonymous namespace

auto parse_startup_options(int argc, char const* const argv[]) -> std::optional<StartupOptions>
{
    using namespace jb::core;

    constexpr std::array options{
        CommandLineOption{.long_name = "socket",           .value_mode = CommandLineValueMode::Required},
        CommandLineOption{.long_name = "database",         .value_mode = CommandLineValueMode::Required},
        CommandLineOption{.long_name = "cli-concurrency",  .value_mode = CommandLineValueMode::Required},
        CommandLineOption{.long_name = "http-concurrency", .value_mode = CommandLineValueMode::Required},
        CommandLineOption{.long_name = "allow-root-cli",   .value_mode = CommandLineValueMode::None    },
        CommandLineOption{.long_name = "http-proxy",       .value_mode = CommandLineValueMode::Required},
        CommandLineOption{.long_name = "http-ca-bundle",   .value_mode = CommandLineValueMode::Required},
    };
    CommandLineParser parser{argc, argv, options};

    auto socket_path      = std::optional<std::filesystem::path>{};
    auto database_path    = std::optional<std::filesystem::path>{};
    auto cli_concurrency  = std::optional<std::uint32_t>{};
    auto http_concurrency = std::optional<std::uint32_t>{};
    auto allow_root_cli   = false;
    auto http_proxy       = std::optional<std::string>{};
    auto http_ca_bundle   = std::optional<std::filesystem::path>{};
    for (auto const& argument : parser) {
        if (argument.kind() != CommandLineArgumentKind::Option || !argument.known() || argument.missing_value()) {
            return std::nullopt;
        }

        // The unsafe override is presence-only. Even an explicitly empty inline value must be rejected.
        if (argument.name() == "allow-root-cli") {
            if (allow_root_cli || argument.value()) {
                return std::nullopt;
            }
            allow_root_cli = true;
            continue;
        }
        if (!argument.value() || argument.value()->empty()) {
            return std::nullopt;
        }

        if (argument.name() == "socket" && !socket_path) {
            socket_path = std::filesystem::path{std::string{*argument.value()}};
        }
        else if (argument.name() == "database" && !database_path) {
            database_path = std::filesystem::path{std::string{*argument.value()}};
        }
        else if (argument.name() == "cli-concurrency" && !cli_concurrency) {
            cli_concurrency = parse_positive_uint32(*argument.value());
            if (!cli_concurrency) {
                return std::nullopt;
            }
        }
        else if (argument.name() == "http-concurrency" && !http_concurrency) {
            http_concurrency = parse_positive_uint32(*argument.value());
            if (!http_concurrency) {
                return std::nullopt;
            }
        }
        else if (argument.name() == "http-proxy" && !http_proxy) {
            http_proxy = std::string{*argument.value()};
        }
        else if (argument.name() == "http-ca-bundle" && !http_ca_bundle) {
            http_ca_bundle = std::filesystem::path{std::string{*argument.value()}};
        }
        else {
            return std::nullopt;
        }
    }

    if (!socket_path || !database_path) {
        return std::nullopt;
    }
    return StartupOptions{
        .socket_path      = std::move(*socket_path),
        .database_path    = std::move(*database_path),
        .cli_concurrency  = cli_concurrency.value_or(4U),
        .http_concurrency = http_concurrency.value_or(16U),
        .allow_root_cli   = allow_root_cli,
        .http_proxy       = std::move(http_proxy),
        .http_ca_bundle   = std::move(http_ca_bundle),
    };
}

auto scheduler_options(StartupOptions const& startup) -> jb::jobu::SchedulerOptions
{
    return {.cli_concurrency = startup.cli_concurrency, .http_concurrency = startup.http_concurrency};
}

} // namespace jb::jobud::detail
