#include "client.hpp"

#include "framing.hpp"
#include "json.hpp"
#include "protocol.hpp"
#include "rpc.hpp"
#include "server.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

namespace {

void compile_client_api(jb::core::IODevice& device, jb::core::Object* parent)
{
    auto client = jb::rpc::Client{device, {}, parent};
    static_cast<void>(client.call("method"));
    static_cast<void>(client.notify("notification"));
    client.cancel(jb::rpc::RequestId{std::uint64_t{1}});
    static_cast<void>(client.pending_request_count());

    auto result       = client.result_received.connect([](jb::rpc::RequestId const&, jb::rpc::JsonValue const&) {});
    auto remote_error = client.error_received.connect([](jb::rpc::RequestId const&, jb::rpc::RpcError const&) {});
    auto failed       = client.request_failed.connect([](jb::rpc::RequestId const&, jb::core::Error const&) {});
    auto protocol     = client.protocol_error.connect([](jb::core::Error const&) {});
    result.disconnect();
    remote_error.disconnect();
    failed.disconnect();
    protocol.disconnect();
    client.close();
}

} // anonymous namespace

auto main() -> int
{
    static_assert(std::is_base_of_v<jb::core::Object, jb::rpc::Server>);
    static_assert(std::is_base_of_v<jb::core::Object, jb::rpc::Client>);
    static_assert(!std::is_copy_constructible_v<jb::rpc::Client>);
    static_assert(!std::is_move_constructible_v<jb::rpc::Client>);
    static_assert(!std::is_copy_constructible_v<jb::rpc::Server>);
    static_assert(!std::is_move_constructible_v<jb::rpc::Server>);
    static_assert(!std::is_copy_constructible_v<jb::rpc::StreamFramer>);
    static_assert(std::is_nothrow_move_constructible_v<jb::rpc::StreamFramer>);

    auto options                    = jb::rpc::ServerOptions{};
    options.max_batch_entries       = 8U;
    options.max_connections         = 4U;
    options.max_queued_output_bytes = 4096U;

    auto client_options                    = jb::rpc::ClientOptions{};
    client_options.max_batch_entries       = 8U;
    client_options.max_pending_requests    = 4U;
    client_options.max_queued_output_bytes = 4096U;
    static_cast<void>(client_options);
    static_cast<void>(&compile_client_api);

    auto server = jb::rpc::Server{options};
    static_cast<void>(server.register_method("echo", [](auto const&, auto const&) {
        return jb::rpc::MethodResult::success(jb::rpc::JsonValue{});
    }));
    static_cast<void>(server.has_method("echo"));
    static_cast<void>(server.connection_count());
    server.close_connection(jb::rpc::ConnectionId{1});
    server.close();

    auto opened = server.connection_opened.connect([](jb::rpc::ConnectionId) {});
    auto closed = server.connection_closed.connect([](jb::rpc::ConnectionId) {});
    auto failed = server.connection_error.connect([](jb::rpc::ConnectionId, jb::core::Error const&) {});
    opened.disconnect();
    closed.disconnect();
    failed.disconnect();

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
