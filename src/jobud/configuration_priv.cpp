#include "configuration_priv.hpp"

#include "attribute_registry.hpp"
#include "cron.hpp"
#include "http_validation_priv.hpp"
#include "ini_file.hpp"
#include "json.hpp"
#include "text_validation_priv.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <limits>
#include <span>
#include <string_view>
#include <system_error>
#include <utility>

#if defined(__linux__) || defined(__APPLE__)
#  include <sys/un.h>
#endif

namespace jb::jobud::detail {
namespace {

using ConfigurationResult = jb::core::Result<ConfigurationInput, StartupError>;

constexpr std::size_t kMaximumConfigurationBytes{std::size_t{64} * 1024U};
constexpr std::size_t kDefaultHeaderBytes{std::size_t{16} * 1024U};
constexpr std::size_t kDefaultBodyBytes{std::size_t{1024} * 1024U};
constexpr std::size_t kDefaultQueuedOutputBytes{std::size_t{2} * 1024U * 1024U};

auto invalid(std::optional<std::string> key = std::nullopt) -> StartupError
{
    return {.code = "jobud.config.invalid", .key = std::move(key), .message = "Daemon configuration is invalid"};
}

auto safe_key(std::string_view key) -> std::optional<std::string>
{
    return std::string{key}; // Call only for a recognized literal or registered attribute name.
}

auto parse_unsigned(std::string_view value) -> std::optional<std::uint64_t>
{
    std::uint64_t parsed{};
    if (value.empty()) {
        return std::nullopt;
    }
    auto const result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size()) {
        return std::nullopt;
    }
    return parsed;
}

struct UnitMultiplier {
    char          suffix;
    std::uint64_t multiplier;
};

// Convert in the target unit before range checks, without overflowing even for large input text.
auto parse_quantity(std::string_view value, std::span<UnitMultiplier const> units, std::uint64_t maximum)
    -> std::optional<std::uint64_t>
{
    auto multiplier = std::uint64_t{1};
    if (!value.empty() && (value.back() < '0' || value.back() > '9')) {
        auto const suffix = value.back();
        auto const unit =
            std::ranges::find_if(units, [suffix](UnitMultiplier candidate) { return candidate.suffix == suffix; });
        if (unit == units.end()) {
            return std::nullopt;
        }
        multiplier = unit->multiplier;
        value.remove_suffix(1);
    }

    auto parsed = parse_unsigned(value);
    if (!parsed || *parsed > maximum / multiplier) {
        return std::nullopt;
    }
    return *parsed * multiplier;
}

template <typename T>
auto assign_integer(std::optional<T>& target, std::string_view value, std::uint64_t minimum, std::uint64_t maximum)
    -> bool
{
    auto parsed = parse_unsigned(value);
    if (!parsed || *parsed < minimum || *parsed > maximum || *parsed > std::numeric_limits<T>::max()) {
        return false;
    }
    target = static_cast<T>(*parsed);
    return true;
}

auto assign_seconds(std::optional<std::chrono::seconds>& target,
                    std::string_view                     value,
                    std::uint64_t                        minimum,
                    std::uint64_t                        maximum) -> bool
{
    constexpr std::array units{
        UnitMultiplier{.suffix = 's', .multiplier = 1U     },
        UnitMultiplier{.suffix = 'm', .multiplier = 60U    },
        UnitMultiplier{.suffix = 'h', .multiplier = 3'600U },
        UnitMultiplier{.suffix = 'd', .multiplier = 86'400U}
    };
    auto parsed = parse_quantity(value, units, maximum);
    if (!parsed || *parsed < minimum) {
        return false;
    }
    target = std::chrono::seconds{static_cast<std::chrono::seconds::rep>(*parsed)};
    return true;
}

auto assign_bytes(std::optional<std::size_t>& target,
                  std::string_view            value,
                  std::uint64_t               minimum,
                  std::uint64_t               maximum) -> bool
{
    constexpr std::array units{
        UnitMultiplier{.suffix = 'k', .multiplier = std::uint64_t{1} << 10U},
        UnitMultiplier{.suffix = 'm', .multiplier = std::uint64_t{1} << 20U},
        UnitMultiplier{.suffix = 'g', .multiplier = std::uint64_t{1} << 30U}
    };
    auto parsed = parse_quantity(value, units, maximum);
    if (!parsed || *parsed < minimum || *parsed > std::numeric_limits<std::size_t>::max()) {
        return false;
    }
    target = static_cast<std::size_t>(*parsed);
    return true;
}

auto ascii_lower(char value) -> char
{
    if (value >= 'A' && value <= 'Z') {
        return static_cast<char>(value + ('a' - 'A'));
    }
    return value;
}

auto ascii_equal_ignoring_case(std::string_view left, std::string_view right) -> bool
{
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (ascii_lower(left[index]) != ascii_lower(right[index])) {
            return false;
        }
    }
    return true;
}

auto assign_boolean(std::optional<bool>& target, std::string_view value) -> bool
{
    if (value == "1" || ascii_equal_ignoring_case(value, "true") || ascii_equal_ignoring_case(value, "on") ||
        ascii_equal_ignoring_case(value, "yes")) {
        target = true;
        return true;
    }
    if (value == "0" || ascii_equal_ignoring_case(value, "false") || ascii_equal_ignoring_case(value, "off") ||
        ascii_equal_ignoring_case(value, "no")) {
        target = false;
        return true;
    }
    return false;
}

auto valid_name(std::string_view value) -> bool
{
    return !value.empty() && !jb::jobu::detail::has_ascii_control(value);
}

auto absolute_path(std::string_view value) -> std::optional<std::filesystem::path>
{
    if (value.empty() || jb::jobu::detail::has_ascii_control(value)) {
        return std::nullopt;
    }
    auto path = std::filesystem::path{value};
    if (!path.is_absolute()) {
        return std::nullopt;
    }
    return path;
}

auto valid_socket_path(std::filesystem::path const& path) -> bool
{
#if defined(__linux__) || defined(__APPLE__)
    sockaddr_un address{};
    return path.native().size() < sizeof(address.sun_path);
#else
    return !path.empty();
#endif
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

auto parse_level(std::string_view value) -> std::optional<jb::core::LogLevel>
{
    constexpr std::array levels{
        std::pair{"fatal",   jb::core::LogLevel::Fatal  },
        std::pair{"error",   jb::core::LogLevel::Error  },
        std::pair{"warning", jb::core::LogLevel::Warning},
        std::pair{"info",    jb::core::LogLevel::Info   },
        std::pair{"debug1",  jb::core::LogLevel::Debug1 },
        std::pair{"debug2",  jb::core::LogLevel::Debug2 },
        std::pair{"debug3",  jb::core::LogLevel::Debug3 },
    };
    for (auto const& [name, level] : levels) {
        if (value == name) {
            return level;
        }
    }
    return std::nullopt;
}

enum class SettingStatus : std::uint8_t {
    Accepted,
    Invalid,
    Unknown
};

// Keep conversion beside each recognized key so the accepted key set is easy to audit.
auto parse_setting(ConfigurationInput& config, std::string_view key, std::string_view value) -> SettingStatus
{
    using Status = SettingStatus;

    if (key == "database.backend") {
        if (value != "sqlite") {
            return Status::Invalid;
        }
        config.database_backend = DatabaseBackend::Sqlite;
    }
    else if (key == "database.path" || key == "socket.path") {
        auto path = absolute_path(value);
        if (!path || (key == "socket.path" && !valid_socket_path(*path))) {
            return Status::Invalid;
        }
        if (key == "database.path") {
            config.database_path = std::move(*path);
        }
        else {
            config.socket_path = std::move(*path);
        }
    }
    else if (key == "socket.owner" || key == "socket.group" || key == "daemon.run_as_group") {
        if (!value.empty() && !valid_name(value)) {
            return Status::Invalid;
        }
        auto name = value.empty() ? std::optional<std::string>{} : std::optional<std::string>{value};
        if (key == "socket.owner") {
            config.socket_owner = std::move(name);
        }
        else if (key == "socket.group") {
            config.socket_group = std::move(name);
        }
        else {
            config.run_as_group = std::move(name);
        }
    }
    else if (key == "daemon.run_as_user") {
        if (!valid_name(value)) {
            return Status::Invalid;
        }
        config.run_as_user = value;
    }
    else if (key == "socket.mode") {
        if (value != "0600" && value != "0660") {
            return Status::Invalid;
        }
        config.socket_mode = value == "0600" ? 0600U : 0660U;
    }
    else if (key == "daemon.allow_root") {
        return assign_boolean(config.allow_root_daemon, value) ? Status::Accepted : Status::Invalid;
    }
    else if (key == "cli.allow_root") {
        return assign_boolean(config.allow_root_cli, value) ? Status::Accepted : Status::Invalid;
    }
    else if (key == "cli.concurrency") {
        return assign_integer(config.cli_concurrency, value, 1U, UINT32_MAX) ? Status::Accepted : Status::Invalid;
    }
    else if (key == "http.concurrency") {
        return assign_integer(config.http_concurrency, value, 1U, UINT32_MAX) ? Status::Accepted : Status::Invalid;
    }
    else if (key == "http.proxy") {
        if (value.empty()) {
            config.http_proxy.reset();
        }
        else if (!jb::net::detail::validate_http_url(value)) {
            return Status::Invalid;
        }
        else {
            config.http_proxy = value;
        }
    }
    else if (key == "http.ca_bundle") {
        if (value.empty()) {
            config.http_ca_bundle.reset();
        }
        else {
            auto path = absolute_path(value);
            if (!path || !readable_regular_file(*path)) {
                return Status::Invalid;
            }
            config.http_ca_bundle = std::move(*path);
        }
    }
    else if (key == "schedule.default_timezone") {
        if (value.empty()) {
            return Status::Invalid;
        }
        // CronEngine has no timezone-only validator; a fixed valid expression exercises its normal TZif lookup.
        jb::jobu::SystemCronEngine cron;
        if (!cron.validate({.expression = "* * * * *", .timezone = std::string{value}})) {
            return Status::Invalid;
        }
        config.default_timezone = value;
    }
    else if (key == "history.default_retention") {
        // Durable timestamps use signed microseconds. Reject a duration that cannot be represented in those units.
        constexpr auto maximum = static_cast<std::uint64_t>(INT64_MAX / 1'000'000);
        return assign_seconds(config.default_retention, value, 0U, maximum) ? Status::Accepted : Status::Invalid;
    }
    else if (key == "history.sweep_interval") {
        return assign_seconds(config.history_sweep_interval, value, 1U, 86'400U) ? Status::Accepted : Status::Invalid;
    }
    else if (key == "history.batch_size") {
        return assign_integer(config.history_batch_size, value, 1U, 1'000U) ? Status::Accepted : Status::Invalid;
    }
    else if (key == "telemetry.checkpoint_interval") {
        return assign_seconds(config.telemetry_checkpoint_interval, value, 1U, 86'400U) ? Status::Accepted
                                                                                        : Status::Invalid;
    }
    else if (key == "rpc.header_limit_bytes") {
        return assign_bytes(config.rpc_header_limit_bytes, value, 1'024U, 65'536U) ? Status::Accepted : Status::Invalid;
    }
    else if (key == "rpc.body_limit_bytes") {
        return assign_bytes(config.rpc_body_limit_bytes, value, 1'048'576U, 16'777'216U) ? Status::Accepted
                                                                                         : Status::Invalid;
    }
    else if (key == "rpc.max_batch_entries") {
        return assign_integer(config.rpc_max_batch_entries, value, 1U, 64U) ? Status::Accepted : Status::Invalid;
    }
    else if (key == "rpc.max_connections") {
        return assign_integer(config.rpc_max_connections, value, 1U, 4'096U) ? Status::Accepted : Status::Invalid;
    }
    else if (key == "rpc.queued_output_bytes") {
        return assign_bytes(config.rpc_queued_output_bytes, value, 1U, std::uint64_t{64} * 1024U * 1024U)
                 ? Status::Accepted
                 : Status::Invalid;
    }
    else if (key == "logging.level") {
        config.logging_level = parse_level(value);
        return config.logging_level ? Status::Accepted : Status::Invalid;
    }
    else if (key == "logging.format") {
        if (value != "json" && value != "text") {
            return Status::Invalid;
        }
        config.logging_format = value == "json" ? LoggingFormat::Json : LoggingFormat::Text;
    }
    else {
        return Status::Unknown;
    }
    return Status::Accepted;
}

auto lexical_error(std::string_view diagnostic) -> StartupError
{
    auto error = invalid();
    if (!diagnostic.starts_with("line ")) {
        return error;
    }
    diagnostic.remove_prefix(5);
    std::size_t line{};
    auto const  parsed = std::from_chars(diagnostic.data(), diagnostic.data() + diagnostic.size(), line);
    if (parsed.ec == std::errc{} && parsed.ptr != diagnostic.data() + diagnostic.size() && *parsed.ptr == ':') {
        error.line = line;
    }
    return error;
}

auto validate_rpc_budgets(ConfigurationInput& config) -> std::optional<StartupError>
{
    auto const header = config.rpc_header_limit_bytes.value_or(kDefaultHeaderBytes);
    auto const body   = config.rpc_body_limit_bytes.value_or(kDefaultBodyBytes);
    auto const queued = config.rpc_queued_output_bytes.value_or(kDefaultQueuedOutputBytes);

    if (header > std::numeric_limits<std::size_t>::max() - body) {
        return invalid(safe_key("rpc.body_limit_bytes"));
    }
    config.rpc_read_buffer_capacity = header + body;
    if (queued < config.rpc_read_buffer_capacity) {
        return invalid(safe_key("rpc.queued_output_bytes"));
    }
    return std::nullopt;
}

} // anonymous namespace

auto parse_configuration_text(std::string_view text) -> ConfigurationResult
{
    if (text.size() > kMaximumConfigurationBytes || text.find('\0') != std::string_view::npos ||
        !jb::jobu::detail::is_valid_utf8(text)) {
        return ConfigurationResult::failure(invalid());
    }

    auto ini = jb::core::IniFile::from_text(text);
    if (!ini.ok()) {
        return ConfigurationResult::failure(lexical_error(ini.error()));
    }

    ConfigurationInput                  config;
    jb::core::JsonValue::Object         default_json;
    jb::jobu::StandardAttributeRegistry registry;
    for (auto const& [key, values] : ini) {
        if (key.starts_with("defaults.")) {
            auto const  attribute_name = std::string_view{key}.substr(sizeof("defaults.") - 1U);
            auto const* definition     = registry.find(attribute_name);
            if (definition == nullptr) {
                return ConfigurationResult::failure(
                    {.code = "jobud.config.unknown_key", .message = "Daemon configuration key is unknown"});
            }
            if (!definition->scopes.test(jb::jobu::AttributeScope::DaemonDefault)) {
                return ConfigurationResult::failure(invalid(safe_key(key)));
            }
            if (values.size() != 1U) {
                return ConfigurationResult::failure({.code    = "jobud.config.duplicate_key",
                                                     .key     = safe_key(key),
                                                     .message = "Daemon configuration key is repeated"});
            }
            auto parsed = jb::core::parse_json(values.front());
            if (!parsed) {
                return ConfigurationResult::failure(invalid(safe_key(key)));
            }
            default_json.emplace(std::string{attribute_name}, std::move(parsed).value());
            continue;
        }

        switch (parse_setting(config, key, values.front())) {
            case SettingStatus::Accepted:
                break;
            case SettingStatus::Invalid:
                return ConfigurationResult::failure(invalid(safe_key(key)));
            case SettingStatus::Unknown:
                return ConfigurationResult::failure(
                    {.code = "jobud.config.unknown_key", .message = "Daemon configuration key is unknown"});
        }
        if (values.size() != 1U) {
            return ConfigurationResult::failure({.code    = "jobud.config.duplicate_key",
                                                 .key     = safe_key(key),
                                                 .message = "Daemon configuration key is repeated"});
        }
    }

    // Decode the entire partial layer before materializing it. This validates interactions among supplied defaults
    // without replacing the inherited built-ins or storing a complete job snapshot as daemon policy.
    auto decoded = jb::jobu::attribute_set_from_json(jb::core::JsonValue{.data = std::move(default_json)},
                                                     registry,
                                                     jb::jobu::AttributeScope::DaemonDefault);
    if (!decoded) {
        return ConfigurationResult::failure(
            {.code = "jobud.config.invalid", .message = "Daemon attribute defaults are invalid"});
    }
    config.daemon_defaults = std::move(decoded).value();
    if (!jb::jobu::materialize_attributes(registry, config.daemon_defaults, {}, {})) {
        return ConfigurationResult::failure(
            {.code = "jobud.config.invalid", .message = "Daemon attribute defaults are incompatible"});
    }
    if (auto budget_error = validate_rpc_budgets(config)) {
        return ConfigurationResult::failure(std::move(*budget_error));
    }

    return ConfigurationResult::success(std::move(config));
}

} // namespace jb::jobud::detail
