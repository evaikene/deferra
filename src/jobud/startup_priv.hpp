#pragma once

#include "scheduler.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace jb::jobud::detail {

/// Fully validated command-line settings, prepared before opening daemon resources.
struct StartupOptions {
    std::filesystem::path                socket_path;
    std::filesystem::path                database_path;
    std::uint32_t                        cli_concurrency{4};
    std::uint32_t                        http_concurrency{16};
    bool                                 allow_root_cli{false};
    std::optional<std::string>           http_proxy;
    std::optional<std::filesystem::path> http_ca_bundle;
};

[[nodiscard]] auto parse_startup_options(int argc, char const* const argv[]) -> std::optional<StartupOptions>;
[[nodiscard]] auto scheduler_options(StartupOptions const& startup) -> jb::jobu::SchedulerOptions;

} // namespace jb::jobud::detail
