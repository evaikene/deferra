/** @file rpc.hpp
 * @brief Marks the JobU RPC module during its Phase 0 skeleton stage.
 */
#pragma once

#include <cstdint>

namespace jb::rpc {

/// Version of the public RPC module contract.
inline constexpr std::uint32_t module_version{1};

} // namespace jb::rpc
