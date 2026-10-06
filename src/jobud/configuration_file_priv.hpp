#pragma once

#include "configuration_priv.hpp"

#include <filesystem>
#include <optional>
#include <string>

namespace jb::jobud::detail {

/// Reads at most 64 KiB from the same inspected descriptor, without following a leaf symlink.
/// The invoking user or root must own the file and control its protected parent chain;
/// root invocations require root ownership. Group/other write access is refused.
/// Optional absence is accepted only through an otherwise trusted existing prefix.
[[nodiscard]] auto read_configuration_file(std::filesystem::path const& path, bool allow_missing)
    -> jb::core::Result<std::optional<std::string>, StartupError>;

} // namespace jb::jobud::detail
