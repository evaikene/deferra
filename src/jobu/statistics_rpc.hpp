/// @file statistics_rpc.hpp
/// @brief Registration of bounded system and queue retained-history statistics.
///
#pragma once

#include <span>
#include <string_view>

namespace jb::rpc {
class Server;
}

namespace jb::jobu {

class ManagementService;
class StatisticsService;

/// Returns exactly the method names registered by register_statistics_methods().
[[nodiscard]] auto statistics_rpc_method_names() noexcept -> std::span<std::string_view const>;

/// Registers read-only statistics methods while borrowed services outlive the server.
/// Queue selectors are resolved through management before the initial scoped read.
/// Returns false on incomplete registration; callers must discard that server.
auto register_statistics_methods(jb::rpc::Server& server, StatisticsService& statistics, ManagementService& management)
    -> bool;

} // namespace jb::jobu
