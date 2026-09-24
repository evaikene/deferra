#pragma once

#include "command_line_priv.hpp"
#include "error.hpp"
#include "json.hpp"
#include "result.hpp"

namespace jb::jobuctl::detail {

/// Reads one bounded JSON object from a file or standard input (-) with fixed, input-safe errors.
[[nodiscard]] auto load_json_object(std::filesystem::path const& path)
    -> jb::core::Result<jb::core::JsonValue, jb::core::Error>;

/// Loads a selected request file after local help has been resolved, using the public method's strict request codec.
/// Input failures have fixed messages and do not expose the supplied document.
[[nodiscard]] auto load_request_file(Command& command, jb::jobu::StandardAttributeRegistry const& registry)
    -> jb::core::Result<void, jb::core::Error>;

} // namespace jb::jobuctl::detail
