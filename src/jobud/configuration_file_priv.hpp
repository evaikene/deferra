#pragma once

#include "configuration_priv.hpp"

#include <filesystem>
#include <optional>
#include <string>

namespace jb::jobud::detail {

/// Reads one bounded regular file without following its final pathname component.
/// Parent ownership and mode checks are added at the protected-path stage.
[[nodiscard]] auto read_configuration_file(std::filesystem::path const& path, bool allow_missing)
    -> jb::core::Result<std::optional<std::string>, StartupError>;

} // namespace jb::jobud::detail
