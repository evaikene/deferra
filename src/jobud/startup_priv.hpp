#pragma once

#include "configuration_priv.hpp"
#include "scheduler.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace jb::jobud::detail {

enum class StartupAction : std::uint8_t {
    Run,
    Help,
    Version,
    CheckConfig
};

/// Only explicitly supplied flags live here. Optional booleans distinguish false from absence.
struct StartupArguments {
    StartupAction                        action{StartupAction::Run};
    std::optional<std::filesystem::path> config_path;
    bool                                 no_config{false};
    std::optional<std::filesystem::path> socket_path;
    std::optional<std::filesystem::path> database_path;
    std::optional<std::uint32_t>         cli_concurrency;
    std::optional<std::uint32_t>         http_concurrency;
    std::optional<bool>                  allow_root_cli;
    std::optional<bool>                  allow_root_daemon;
    std::optional<std::string>           run_as_user;
    std::optional<std::string>           run_as_group;
    std::optional<std::string>           http_proxy;
    std::optional<std::filesystem::path> http_ca_bundle;
};

/// Absolute paths compiled from the configured installation prefix or explicit cache overrides.
struct CompiledPaths {
    std::filesystem::path config;
    std::filesystem::path database;
    std::filesystem::path socket;
};

struct LoadedConfiguration {
    ConfigurationInput                   input;
    std::optional<std::filesystem::path> source_path;
};

/// Immutable resolved settings; consumers may be wired in later Phase 9 stages.
struct StartupOptions {
    std::filesystem::path                socket_path;
    std::filesystem::path                database_path;
    std::uint32_t                        cli_concurrency{4};
    std::uint32_t                        http_concurrency{16};
    bool                                 allow_root_cli{false};
    bool                                 allow_root_daemon{false};
    std::optional<std::string>           run_as_user;
    std::optional<std::string>           run_as_group;
    std::optional<std::string>           socket_owner;
    std::optional<std::string>           socket_group;
    std::uint16_t                        socket_mode{0600U};
    std::optional<std::string>           http_proxy;
    std::optional<std::filesystem::path> http_ca_bundle;
    std::string                          default_timezone{"UTC"};
    std::chrono::seconds                 default_retention{2'592'000};
    std::chrono::seconds                 history_sweep_interval{60};
    std::uint32_t                        history_batch_size{100};
    std::chrono::seconds                 telemetry_checkpoint_interval{30};
    std::size_t                          rpc_header_limit_bytes{16'384};
    std::size_t                          rpc_body_limit_bytes{1'048'576};
    std::size_t                          rpc_max_batch_entries{64};
    std::size_t                          rpc_max_connections{128};
    std::size_t                          rpc_queued_output_bytes{2'097'152};
    std::size_t                          rpc_read_buffer_capacity{1'064'960};
    jb::core::LogLevel                   logging_level{jb::core::LogLevel::Info};
    LoggingFormat                        logging_format{LoggingFormat::Json};
    jb::jobu::AttributeSet               daemon_defaults;
};

[[nodiscard]] auto compiled_paths() -> CompiledPaths;
[[nodiscard]] auto parse_startup_arguments(int argc, char const* const argv[])
    -> jb::core::Result<StartupArguments, StartupError>;
[[nodiscard]] auto load_configuration(StartupArguments const&      arguments,
                                      CompiledPaths const&         paths,
                                      std::filesystem::path const& invocation_directory)
    -> jb::core::Result<LoadedConfiguration, StartupError>;
[[nodiscard]] auto resolve_startup_options(StartupArguments const&      arguments,
                                           ConfigurationInput const&    input,
                                           CompiledPaths const&         paths,
                                           std::filesystem::path const& invocation_directory)
    -> jb::core::Result<StartupOptions, StartupError>;
/// Checks supplied account names without changing identity or opening daemon resources.
[[nodiscard]] auto validate_readonly_accounts(StartupOptions const& options) -> jb::core::Result<void, StartupError>;
[[nodiscard]] auto scheduler_options(StartupOptions const& startup) -> jb::jobu::SchedulerOptions;

} // namespace jb::jobud::detail
