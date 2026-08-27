/**
 * @file system_info.hpp
 * @brief Defines the typed JobU daemon information exchanged by `system.info`.
 *
 * Daemons construct SystemInfo from their runtime version and implemented capabilities, then pass it to
 * system_info_to_json() from the RPC handler. Clients validate the returned JSON with system_info_from_json() before
 * displaying or acting on the result:
 *
 * @code{.cpp}
 * jb::jobu::SystemInfo info{
 *     .daemon_version = "0.1.0",
 *     .capabilities = {"system.info"},
 * };
 * auto wire_value = jb::jobu::system_info_to_json(info);
 * auto decoded = jb::jobu::system_info_from_json(wire_value);
 * @endcode
 */
#pragma once

#include "error.hpp"
#include "json.hpp"
#include "result.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace jb::jobu {

/// Identifies the version of the JobU RPC API implemented by a daemon.
struct ApiVersion {
    /// Incompatible API generation.
    std::uint32_t major{1};

    /// Backward-compatible feature revision within @ref major.
    std::uint32_t minor{0};
};

/// Describes one running JobU daemon and the operations it advertises.
struct SystemInfo {
    /// Daemon software version reported by the running executable.
    std::string daemon_version;

    /// RPC API version implemented by the daemon.
    ApiVersion api_version;

    /// Unique, lexicographically sorted method capabilities implemented by the daemon.
    std::vector<std::string> capabilities;
};

/** Encodes daemon information as the project-owned JSON tree used by JSON-RPC.
 *
 * Capability names are copied, sorted, and deduplicated so the wire result is deterministic.
 *
 * @param info Typed daemon information to encode.
 * @return Owning JSON object containing `daemon_version`, `api_version`, and `capabilities`.
 */
[[nodiscard]] auto system_info_to_json(SystemInfo const& info) -> jb::core::JsonValue;

/** Decodes and validates a `system.info` result.
 *
 * All known fields are mandatory and must have their documented JSON types. API version components must fit in
 * `std::uint32_t`, capability entries must be strings, and unknown object members are ignored for forward
 * compatibility. Decoded capability names are sorted and deduplicated.
 *
 * @param value JSON-RPC result value to decode without retaining references to it.
 * @return Typed daemon information, or `jobu.system_info.invalid_response` when the required shape is invalid.
 */
[[nodiscard]] auto system_info_from_json(jb::core::JsonValue const& value)
    -> jb::core::Result<SystemInfo, jb::core::Error>;

} // namespace jb::jobu
