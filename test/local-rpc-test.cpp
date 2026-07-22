#include "application.hpp"
#include "client.hpp"
#include "event_loop_types.hpp"
#include "local_server.hpp"
#include "local_socket.hpp"
#include "server.hpp"
#include "support/temporary_directory.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <unistd.h>

using namespace jb::core;
using namespace jb::net;
using namespace jb::rpc;

// NOLINTBEGIN(readability-magic-numbers)

namespace {

template <typename T>
auto make_json(T value) -> JsonValue
{
    JsonValue result;
    result.data = std::move(value);
    return result;
}

template <typename Predicate>
auto wait_for(Application& app, Predicate&& predicate, std::chrono::milliseconds timeout = std::chrono::seconds{3})
    -> bool
{
    auto const deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        app.process_events(EventFlag::All, 20);
    }
    return predicate();
}

auto require_unsigned(RequestId const& id) -> std::uint64_t
{
    REQUIRE(std::holds_alternative<std::uint64_t>(id));
    return std::get<std::uint64_t>(id);
}

struct ClientEvents {
    std::vector<std::pair<RequestId, JsonValue>> results;
    std::vector<std::pair<RequestId, RpcError>>  remote_errors;
    std::vector<std::pair<RequestId, Error>>     request_failures;
    std::vector<Error>                           protocol_errors;
    std::vector<std::pair<IOError, std::string>> socket_errors;
    int                                          connected_count{0};
    int                                          disconnected_count{0};
    int                                          closed_count{0};
};

struct ClientPeer {
    ClientEvents                 events;
    std::unique_ptr<LocalSocket> socket;
    std::unique_ptr<Client>      client;
};

struct Invocation {
    RequestContext           context;
    std::optional<JsonValue> params;
};

class LocalRpcFixture {
public:
    explicit LocalRpcFixture(ServerOptions server_options = {}, LocalServerOptions local_options = {})
        : path{directory.path() / "local-rpc.sock"}
        , rpc_server{server_options}
    {
        local_server.new_connection.connect([this]() -> void { drain_connections(); });
        rpc_server.connection_opened.connect([this](ConnectionId id) -> void { opened_ids.push_back(id); });
        rpc_server.connection_error.connect([this](ConnectionId id, Error const& error) -> void {
            connection_errors.emplace_back(id, error);
            terminal_events.push_back("error:" + std::to_string(id));
        });
        rpc_server.connection_closed.connect([this](ConnectionId id) -> void {
            closed_ids.push_back(id);
            terminal_events.push_back("closed:" + std::to_string(id));
        });
        REQUIRE(rpc_server.register_method(
            "echo",
            [this](RequestContext const& context, std::optional<JsonValue> const& params) -> MethodResult {
                invocations.push_back({.context = context, .params = params});
                return MethodResult::success(params.value_or(make_json(JsonNull{})));
            }));
        REQUIRE(local_server.listen(path, local_options));
    }

    ~LocalRpcFixture() { shutdown(); }

    LocalRpcFixture(LocalRpcFixture const&)                    = delete;
    auto operator=(LocalRpcFixture const&) -> LocalRpcFixture& = delete;

    auto start_socket(std::size_t read_buffer_limit = 0U) -> ClientPeer&
    {
        auto peer    = std::make_unique<ClientPeer>();
        peer->socket = std::make_unique<LocalSocket>();
        peer->socket->set_read_buffer_limit(read_buffer_limit);

        auto* events = &peer->events;
        peer->socket->connected.connect([events]() -> void { ++events->connected_count; });
        peer->socket->disconnected.connect([events]() -> void { ++events->disconnected_count; });
        peer->socket->closed.connect([events]() -> void { ++events->closed_count; });
        peer->socket->error_occurred.connect([events](IOError error, std::string const& message) -> void {
            events->socket_errors.emplace_back(error, message);
        });

        auto& result = *peer;
        clients.push_back(std::move(peer));
        result.socket->connect_to_server(path);
        return result;
    }

    void activate_client(ClientPeer& peer)
    {
        REQUIRE(peer.socket);
        REQUIRE(peer.socket->state() == LocalSocketState::Connected);
        REQUIRE_FALSE(peer.client);

        peer.client  = std::make_unique<Client>(*peer.socket);
        auto* events = &peer.events;
        peer.client->result_received.connect(
            [events](RequestId const& id, JsonValue const& value) -> void { events->results.emplace_back(id, value); });
        peer.client->error_received.connect([events](RequestId const& id, RpcError const& error) -> void {
            events->remote_errors.emplace_back(id, error);
        });
        peer.client->request_failed.connect([events](RequestId const& id, Error const& error) -> void {
            events->request_failures.emplace_back(id, error);
        });
        peer.client->protocol_error.connect(
            [events](Error const& error) -> void { events->protocol_errors.push_back(error); });
    }

    auto connect_client(std::size_t read_buffer_limit = 0U) -> ClientPeer&
    {
        auto const admitted_before = admitted_ids.size();
        auto const rejected_before = admission_errors.size();
        auto&      peer            = start_socket(read_buffer_limit);
        REQUIRE(wait_for(app, [&]() -> bool {
            return peer.socket->state() == LocalSocketState::Connected && admitted_ids.size() == admitted_before + 1U &&
                   admission_errors.size() == rejected_before;
        }));
        activate_client(peer);
        return peer;
    }

    void retire(ClientPeer& peer, std::size_t expected_connection_count)
    {
        if (peer.client) {
            peer.client->close();
            peer.client.reset();
        }
        if (peer.socket && peer.socket->state() != LocalSocketState::Unconnected) {
            peer.socket->disconnect_from_server();
        }
        REQUIRE(wait_for(app, [&]() -> bool {
            return peer.socket->state() == LocalSocketState::Unconnected &&
                   rpc_server.connection_count() == expected_connection_count;
        }));
    }

    void shutdown()
    {
        if (stopped) {
            return;
        }
        for (auto& peer : clients) {
            if (peer->client) {
                peer->client->close();
                peer->client.reset();
            }
            if (peer->socket) {
                peer->socket->abort();
            }
        }
        static_cast<void>(wait_for(app, [&]() -> bool { return rpc_server.connection_count() == 0U; }));
        rpc_server.close();
        local_server.close();
        stopped = true;
    }

    Application                                 app{0, nullptr};
    jb::test::TemporaryDirectory                directory;
    std::filesystem::path                       path;
    std::vector<ConnectionId>                   admitted_ids;
    std::vector<Error>                          admission_errors;
    std::vector<ConnectionId>                   opened_ids;
    std::vector<ConnectionId>                   closed_ids;
    std::vector<std::pair<ConnectionId, Error>> connection_errors;
    std::vector<std::string>                    terminal_events;
    std::vector<Invocation>                     invocations;
    LocalServer                                 local_server;
    Server                                      rpc_server;
    std::vector<std::unique_ptr<ClientPeer>>    clients;
    bool                                        stopped{false};

private:
    void drain_connections()
    {
        while (auto socket = local_server.take_next_connection()) {
            auto const credentials = socket->peer_credentials();

            OperationContext operation;
            operation.peer.process_id = credentials.process_id;
            operation.peer.user_id    = credentials.user_id;
            operation.peer.group_id   = credentials.group_id;

            auto added = rpc_server.add_connection(std::move(socket), std::move(operation));
            if (added) {
                admitted_ids.push_back(added.value());
            }
            else {
                admission_errors.push_back(std::move(added).error());
            }
        }
    }
};

auto find_result(ClientPeer const& peer, RequestId const& id) -> JsonValue const*
{
    auto const found =
        std::ranges::find_if(peer.events.results, [&](auto const& result) { return result.first == id; });
    return found == peer.events.results.end() ? nullptr : &found->second;
}

void check_no_rpc_errors(ClientPeer const& peer)
{
    CHECK(peer.events.remote_errors.empty());
    CHECK(peer.events.request_failures.empty());
    CHECK(peer.events.protocol_errors.empty());
    CHECK(peer.events.socket_errors.empty());
}

auto make_marker(std::uint64_t client, std::uint64_t round) -> JsonValue
{
    return make_json(JsonValue::Object{
        {"client", make_json(client)},
        {"round",  make_json(round) },
    });
}

} // anonymous namespace

TEST_CASE("Local RPC keeps one credentialed connection persistent", "[rpc][net][local]")
{
    LocalRpcFixture fixture;
    auto&           peer          = fixture.connect_client();
    auto const      first_params  = make_json(JsonValue::Object{
        {"message", make_json(std::string{"first"})}
    });
    auto const      second_params = make_json(JsonValue::Object{
        {"message", make_json(std::string{"second"})}
    });

    auto first  = peer.client->call("echo", first_params);
    auto second = peer.client->call("echo", second_params);
    REQUIRE(first);
    REQUIRE(second);
    CHECK(require_unsigned(first.value()) == 1U);
    CHECK(require_unsigned(second.value()) == 2U);
    REQUIRE(wait_for(fixture.app, [&]() -> bool { return peer.events.results.size() == 2U; }));

    auto const* first_result  = find_result(peer, first.value());
    auto const* second_result = find_result(peer, second.value());
    REQUIRE(first_result);
    REQUIRE(second_result);
    CHECK(*first_result == first_params);
    CHECK(*second_result == second_params);
    CHECK(peer.client->pending_request_count() == 0U);
    CHECK(peer.socket->state() == LocalSocketState::Connected);
    CHECK(fixture.rpc_server.connection_count() == 1U);
    REQUIRE(fixture.admitted_ids.size() == 1U);
    CHECK(fixture.admitted_ids == fixture.opened_ids);

    REQUIRE(fixture.invocations.size() == 2U);
    for (auto const& invocation : fixture.invocations) {
        CHECK(invocation.context.connection_id == fixture.admitted_ids.front());
#if defined(__APPLE__)
        CHECK_FALSE(invocation.context.operation.peer.process_id);
        REQUIRE(invocation.context.operation.peer.user_id);
        REQUIRE(invocation.context.operation.peer.group_id);
        CHECK(*invocation.context.operation.peer.user_id == static_cast<std::uint64_t>(::geteuid()));
        CHECK(*invocation.context.operation.peer.group_id == static_cast<std::uint64_t>(::getegid()));
#else
        REQUIRE(invocation.context.operation.peer.process_id);
        REQUIRE(invocation.context.operation.peer.user_id);
        REQUIRE(invocation.context.operation.peer.group_id);
        CHECK(*invocation.context.operation.peer.process_id == static_cast<std::uint64_t>(::getpid()));
        CHECK(*invocation.context.operation.peer.user_id == static_cast<std::uint64_t>(::getuid()));
        CHECK(*invocation.context.operation.peer.group_id == static_cast<std::uint64_t>(::getgid()));
#endif
        CHECK_FALSE(invocation.context.operation.authenticated_principal);
    }
    CHECK(fixture.invocations[0].context.operation.peer.process_id ==
          fixture.invocations[1].context.operation.peer.process_id);
    CHECK(fixture.invocations[0].context.operation.peer.user_id ==
          fixture.invocations[1].context.operation.peer.user_id);
    CHECK(fixture.invocations[0].context.operation.peer.group_id ==
          fixture.invocations[1].context.operation.peer.group_id);
    CHECK(fixture.connection_errors.empty());
    check_no_rpc_errors(peer);

    fixture.shutdown();
    CHECK(fixture.rpc_server.connection_count() == 0U);
    CHECK_FALSE(fixture.local_server.is_listening());
    CHECK_FALSE(std::filesystem::exists(fixture.path));
}

TEST_CASE("Local RPC isolates simultaneous clients and preserves their contexts", "[rpc][net][local][multi-client]")
{
    LocalRpcFixture fixture;
    auto*           first  = &fixture.start_socket();
    auto*           second = &fixture.start_socket();
    auto*           third  = &fixture.start_socket();
    auto            peers  = std::vector<ClientPeer*>{first, second, third};

    REQUIRE(wait_for(fixture.app, [&]() -> bool {
        auto const all_connected = std::ranges::all_of(peers, [](ClientPeer const* peer) {
            return peer->socket->state() == LocalSocketState::Connected;
        });
        return all_connected && fixture.rpc_server.connection_count() == peers.size() &&
               fixture.admitted_ids.size() == peers.size();
    }));
    for (auto* peer : peers) {
        fixture.activate_client(*peer);
    }

    for (std::size_t index = 0; index < peers.size(); ++index) {
        auto call = peers[index]->client->call("echo", make_marker(index, 1U));
        REQUIRE(call);
        CHECK(call.value() == RequestId{std::uint64_t{1}});
    }
    REQUIRE(wait_for(fixture.app, [&]() -> bool {
        return std::ranges::all_of(peers, [](ClientPeer const* peer) { return peer->events.results.size() == 1U; });
    }));
    for (std::size_t index = 0; index < peers.size(); ++index) {
        REQUIRE(peers[index]->events.results.size() == 1U);
        CHECK(peers[index]->events.results.front().second == make_marker(index, 1U));
        check_no_rpc_errors(*peers[index]);
    }

    auto first_round_connections = std::map<std::uint64_t, ConnectionId>{};
    for (auto const& invocation : fixture.invocations) {
        REQUIRE(invocation.params);
        auto const& marker = invocation.params->as_object();
        first_round_connections.emplace(marker.at("client").as_uint(), invocation.context.connection_id);
    }
    REQUIRE(first_round_connections.size() == peers.size());
    auto distinct_ids = std::set<ConnectionId>{};
    for (auto const& [client, id] : first_round_connections) {
        static_cast<void>(client);
        CHECK(id != 0U);
        distinct_ids.insert(id);
    }
    CHECK(distinct_ids.size() == peers.size());

    auto const retired_id = first_round_connections.at(1U);
    fixture.retire(*second, 2U);
    CHECK(std::ranges::find(fixture.closed_ids, retired_id) != fixture.closed_ids.end());
    CHECK(fixture.local_server.is_listening());

    auto const invocations_before = fixture.invocations.size();
    for (auto index : {0U, 2U}) {
        auto call = peers[index]->client->call("echo", make_marker(index, 2U));
        REQUIRE(call);
        CHECK(call.value() == RequestId{std::uint64_t{2}});
    }
    REQUIRE(wait_for(fixture.app, [&]() -> bool {
        return first->events.results.size() == 2U && third->events.results.size() == 2U;
    }));
    REQUIRE(fixture.invocations.size() == invocations_before + 2U);
    for (auto index = invocations_before; index < fixture.invocations.size(); ++index) {
        auto const& invocation = fixture.invocations[index];
        REQUIRE(invocation.params);
        auto const& marker = invocation.params->as_object();
        auto const  client = marker.at("client").as_uint();
        CHECK(marker.at("round").as_uint() == 2U);
        CHECK(invocation.context.connection_id == first_round_connections.at(client));
    }
    CHECK(fixture.rpc_server.connection_count() == 2U);
    CHECK(fixture.connection_errors.empty());
    check_no_rpc_errors(*first);
    check_no_rpc_errors(*third);

    fixture.shutdown();
    CHECK(fixture.rpc_server.connection_count() == 0U);
    CHECK_FALSE(fixture.local_server.is_listening());
    CHECK_FALSE(std::filesystem::exists(fixture.path));
}

TEST_CASE("Local RPC survives fragmented and coalesced real-stream traffic", "[rpc][net][local][framing]")
{
    auto local_options                       = LocalServerOptions{};
    local_options.accepted_read_buffer_limit = 19U;
    LocalRpcFixture fixture{{}, local_options};
    auto&           peer     = fixture.connect_client(17U);
    auto            expected = std::vector<std::pair<RequestId, JsonValue>>{};

    for (std::uint64_t index = 1U; index <= 3U; ++index) {
        auto payload = make_json(JsonValue::Object{
            {"index", make_json(index)},
            {"payload", make_json(std::string(2048U, static_cast<char>('a' + index)))},
        });
        auto call    = peer.client->call("echo", payload);
        REQUIRE(call);
        expected.emplace_back(call.value(), std::move(payload));
    }

    REQUIRE(wait_for(fixture.app, [&]() -> bool { return peer.events.results.size() == expected.size(); }));
    for (auto const& [id, value] : expected) {
        auto const* result = find_result(peer, id);
        REQUIRE(result);
        CHECK(*result == value);
    }
    CHECK(peer.socket->read_buffer_limit() == 17U);
    CHECK(fixture.rpc_server.connection_count() == 1U);
    CHECK(peer.socket->state() == LocalSocketState::Connected);

    auto follow_up = peer.client->call("echo",
                                       make_json(JsonValue::Object{
                                           {"after", make_json(true)}
    }));
    REQUIRE(follow_up);
    REQUIRE(wait_for(fixture.app, [&]() -> bool { return peer.events.results.size() == expected.size() + 1U; }));
    CHECK(find_result(peer, follow_up.value()));
    CHECK(fixture.connection_errors.empty());
    check_no_rpc_errors(peer);

    fixture.shutdown();
    CHECK(fixture.rpc_server.connection_count() == 0U);
    CHECK_FALSE(fixture.local_server.is_listening());
    CHECK_FALSE(std::filesystem::exists(fixture.path));
}

TEST_CASE("Local RPC enforces admission limits without stopping the listener", "[rpc][net][local][admission]")
{
    auto server_options            = ServerOptions{};
    server_options.max_connections = 2U;
    LocalRpcFixture fixture{server_options};
    auto&           first    = fixture.connect_client();
    auto&           second   = fixture.connect_client();
    auto&           rejected = fixture.start_socket();

    REQUIRE(wait_for(fixture.app, [&]() -> bool {
        return fixture.admission_errors.size() == 1U && rejected.socket->state() == LocalSocketState::Unconnected;
    }));
    REQUIRE(fixture.admission_errors.size() == 1U);
    CHECK(fixture.admission_errors.front().category == ErrorCategory::ResourceExhausted);
    CHECK(fixture.admission_errors.front().code == "rpc.connection_limit");
    CHECK(rejected.events.connected_count == 1);
    CHECK(rejected.events.disconnected_count == 1);
    CHECK(rejected.events.closed_count == 1);
    CHECK(rejected.events.socket_errors.empty());
    CHECK(fixture.rpc_server.connection_count() == 2U);
    CHECK(fixture.local_server.is_listening());

    auto first_call  = first.client->call("echo", make_marker(0U, 1U));
    auto second_call = second.client->call("echo", make_marker(1U, 1U));
    REQUIRE(first_call);
    REQUIRE(second_call);
    REQUIRE(wait_for(fixture.app, [&]() -> bool {
        return first.events.results.size() == 1U && second.events.results.size() == 1U;
    }));

    auto const first_id = fixture.admitted_ids.front();
    fixture.retire(first, 1U);
    CHECK(std::ranges::find(fixture.closed_ids, first_id) != fixture.closed_ids.end());

    auto& later = fixture.connect_client();
    REQUIRE(fixture.admitted_ids.size() == 3U);
    CHECK(fixture.admitted_ids.back() > fixture.admitted_ids[1]);
    CHECK(fixture.rpc_server.connection_count() == 2U);
    auto later_call = later.client->call("echo", make_marker(2U, 1U));
    REQUIRE(later_call);
    REQUIRE(wait_for(fixture.app, [&]() -> bool { return later.events.results.size() == 1U; }));
    CHECK(later.events.results.front().second == make_marker(2U, 1U));
    CHECK(fixture.local_server.is_listening());
    CHECK(fixture.connection_errors.empty());
    check_no_rpc_errors(second);
    check_no_rpc_errors(later);

    fixture.shutdown();
    CHECK(fixture.rpc_server.connection_count() == 0U);
    CHECK_FALSE(fixture.local_server.is_listening());
    CHECK_FALSE(std::filesystem::exists(fixture.path));
}

TEST_CASE("A local RPC framing failure closes only the offending connection", "[rpc][net][local][isolation]")
{
    auto server_options                   = ServerOptions{};
    server_options.framing.max_body_bytes = 128U;
    LocalRpcFixture fixture{server_options};
    auto&           healthy = fixture.connect_client();

    auto initial = healthy.client->call("echo",
                                        make_json(JsonValue::Object{
                                            {"value", make_json(std::string{"ok"})}
    }));
    REQUIRE(initial);
    REQUIRE(wait_for(fixture.app, [&]() -> bool { return healthy.events.results.size() == 1U; }));

    auto const healthy_id         = fixture.admitted_ids.front();
    auto const invocations_before = fixture.invocations.size();
    auto&      offender           = fixture.start_socket();
    REQUIRE(wait_for(fixture.app, [&]() -> bool {
        return offender.socket->state() == LocalSocketState::Connected && fixture.admitted_ids.size() == 2U;
    }));
    auto const offender_id = fixture.admitted_ids.back();
    REQUIRE(offender_id != healthy_id);

    auto const valid_body  = std::string{R"({"id":99,"jsonrpc":"2.0","method":"echo"})"};
    auto       wire        = std::string{"Content-Length: 129\r\n\r\n"};
    wire                  += "Content-Length: " + std::to_string(valid_body.size()) + "\r\n\r\n" + valid_body;
    CHECK(offender.socket->write(wire) == wire.size());

    REQUIRE(wait_for(fixture.app, [&]() -> bool {
        return fixture.connection_errors.size() == 1U && fixture.closed_ids.size() == 1U &&
               offender.socket->state() == LocalSocketState::Unconnected;
    }));
    REQUIRE(fixture.connection_errors.size() == 1U);
    CHECK(fixture.connection_errors.front().first == offender_id);
    CHECK(fixture.connection_errors.front().second.code == "rpc.framing.body_too_large");
    CHECK(fixture.closed_ids == std::vector<ConnectionId>{offender_id});
    CHECK(fixture.terminal_events ==
          std::vector<std::string>{"error:" + std::to_string(offender_id), "closed:" + std::to_string(offender_id)});
    CHECK(fixture.invocations.size() == invocations_before);
    CHECK(fixture.rpc_server.connection_count() == 1U);
    CHECK(healthy.socket->state() == LocalSocketState::Connected);
    CHECK(fixture.local_server.is_listening());

    auto after_failure = healthy.client->call("echo",
                                              make_json(JsonValue::Object{
                                                  {"value", make_json(std::string{"still healthy"})}
    }));
    REQUIRE(after_failure);
    REQUIRE(wait_for(fixture.app, [&]() -> bool { return healthy.events.results.size() == 2U; }));

    auto& later      = fixture.connect_client();
    auto  later_call = later.client->call("echo",
                                          make_json(JsonValue::Object{
                                              {"value", make_json(std::string{"later"})}
    }));
    REQUIRE(later_call);
    REQUIRE(wait_for(fixture.app, [&]() -> bool { return later.events.results.size() == 1U; }));
    CHECK(fixture.rpc_server.connection_count() == 2U);
    CHECK(fixture.connection_errors.size() == 1U);
    CHECK(fixture.closed_ids.size() == 1U);
    CHECK(offender.events.socket_errors.empty());
    check_no_rpc_errors(healthy);
    check_no_rpc_errors(later);

    fixture.shutdown();
    CHECK(fixture.rpc_server.connection_count() == 0U);
    CHECK_FALSE(fixture.local_server.is_listening());
    CHECK_FALSE(std::filesystem::exists(fixture.path));
}

// NOLINTEND(readability-magic-numbers)
