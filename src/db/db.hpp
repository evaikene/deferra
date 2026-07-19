/** @file db.hpp
 * @brief Marks the JobU database module during its Phase 0 skeleton stage.
 */
#pragma once

#include <cstdint>

namespace jb::db {

/// Version of the public database module contract.
inline constexpr std::uint32_t module_version{1};

} // namespace jb::db
