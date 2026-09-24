/// @file history_rpc.hpp
/// @brief Registration of bounded run, attempt, and output read methods.
///
#pragma once

#include <span>
#include <string_view>

namespace jb::rpc {
class Server;
}

namespace jb::jobu {

class AttributeRegistry;
class HistoryService;

/// Returns exactly the method names registered by register_history_methods().
[[nodiscard]] auto history_rpc_method_names() noexcept -> std::span<std::string_view const>;

/// Registers read-only methods while the borrowed service and registry outlive the server.
/// Returns false if any name could not be registered; callers must discard a partially configured server.
auto register_history_methods(jb::rpc::Server& server, HistoryService& service, AttributeRegistry const& attributes)
    -> bool;

} // namespace jb::jobu
