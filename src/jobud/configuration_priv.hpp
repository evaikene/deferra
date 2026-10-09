#pragma once

#include "attribute.hpp"
#include "error.hpp"
#include "logging.hpp"
#include "result.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace jb::jobud::detail {

/// Startup diagnostics own only fixed text and recognized key names, never input values.
struct StartupError {
    std::string                code;
    std::optional<std::string> key;
    std::optional<std::size_t> line;
    std::string                message;
    jb::core::ErrorCategory    category{jb::core::ErrorCategory::InvalidArgument};
};

enum class LoggingFormat : std::uint8_t {
    Json,
    Text
};
enum class DatabaseBackend : std::uint8_t {
    Sqlite
};

/// Validated settings supplied by one configuration file. Absence remains visible for startup precedence.
struct ConfigurationInput {
    std::optional<DatabaseBackend>       database_backend;
    std::optional<std::filesystem::path> database_path;
    std::optional<std::filesystem::path> socket_path;
    std::optional<std::string>           socket_owner;
    std::optional<std::uint16_t>         socket_mode;
    std::optional<std::string>           socket_group;
    std::optional<std::string>           run_as_user;
    /// A CLI user override can accompany this; their relationship is checked after precedence is resolved.
    std::optional<std::string>           run_as_group;
    std::optional<bool>                  allow_root_daemon;
    std::optional<bool>                  allow_root_cli;
    std::optional<std::uint32_t>         cli_concurrency;
    std::optional<std::uint32_t>         http_concurrency;
    std::optional<std::string>           http_proxy;
    std::optional<std::filesystem::path> http_ca_bundle;
    std::optional<std::string>           default_timezone;
    std::optional<std::chrono::seconds>  default_retention;
    std::optional<std::chrono::seconds>  history_sweep_interval;
    std::optional<std::uint32_t>         history_batch_size;
    std::optional<std::chrono::seconds>  telemetry_checkpoint_interval;
    std::optional<std::size_t>           rpc_header_limit_bytes;
    std::optional<std::size_t>           rpc_body_limit_bytes;
    std::optional<std::size_t>           rpc_max_batch_entries;
    std::optional<std::size_t>           rpc_max_connections;
    std::optional<std::size_t>           rpc_queued_output_bytes;
    std::optional<jb::core::LogLevel>    logging_level;
    std::optional<LoggingFormat>         logging_format;
    jb::jobu::AttributeSet               daemon_defaults;
    /// A full permitted input frame fits in the accepted socket's read buffer.
    std::size_t                          rpc_read_buffer_capacity{0};
};

/// Process-lifetime inventory of accepted fixed keys, excluding registry-driven defaults.*.
/// Parsing and sample completeness checks share this inventory; converters remain the value-policy authority.
[[nodiscard]] auto fixed_configuration_keys() -> std::span<std::string_view const>;

/// Parses and validates bounded daemon INI text without opening daemon resources.
/// Account ownership and path trust are checked after startup precedence is resolved.
[[nodiscard]] auto parse_configuration_text(std::string_view text)
    -> jb::core::Result<ConfigurationInput, StartupError>;

} // namespace jb::jobud::detail
