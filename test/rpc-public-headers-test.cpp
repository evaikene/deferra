#include "protocol.hpp"

#include "framing.hpp"
#include "json.hpp"
#include "rpc.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

auto main() -> int
{
    static_assert(!std::is_copy_constructible_v<jb::rpc::StreamFramer>);
    static_assert(std::is_nothrow_move_constructible_v<jb::rpc::StreamFramer>);

    jb::rpc::StreamFramer framer;
    jb::rpc::StreamFramer moved{std::move(framer)};

    auto context                              = jb::rpc::RequestContext{};
    context.connection_id                     = jb::rpc::ConnectionId{7};
    context.operation.peer.process_id         = std::uint64_t{42};
    context.operation.authenticated_principal = std::string{"user"};
    auto handler                              = jb::rpc::MethodHandler{
        [](jb::rpc::RequestContext const& request, std::optional<jb::rpc::JsonValue> const& params) {
            auto value = params.value_or(jb::rpc::JsonValue{});
            if (request.connection_id == 0U) {
                return jb::rpc::MethodResult::failure({.message = "missing connection"});
            }
            return jb::rpc::MethodResult::success(std::move(value));
        }};
    auto result = handler(context, std::nullopt);

    return moved.buffered_bytes() == 0U && result ? 0 : 1;
}
