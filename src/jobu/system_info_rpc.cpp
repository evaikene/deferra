#include "system_info_rpc.hpp"

#include "protocol.hpp"
#include "server.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace jb::jobu {

namespace {

constexpr std::string_view system_info_method{"system.info"};

} // anonymous namespace

auto system_info_rpc_method_name() noexcept -> std::string_view
{
    return system_info_method;
}

auto register_system_info_method(jb::rpc::Server& server, SystemInfo info) -> bool
{
    return server.register_method(
        std::string{system_info_method},
        [info = std::move(info)](jb::rpc::RequestContext const&, std::optional<jb::rpc::JsonValue> const&)
            -> jb::rpc::MethodResult { return jb::rpc::MethodResult::success(system_info_to_json(info)); });
}

} // namespace jb::jobu
