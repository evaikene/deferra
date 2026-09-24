#pragma once

#include "control_client.hpp"

#include "client.hpp"
#include "connection.hpp"
#include "event_loop_types.hpp"
#include "object_priv.hpp"
#include "timer.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace jb::jobu {

struct ControlClient::Private final : jb::core::priv::ObjectPrivate {
    enum class Phase : std::uint8_t {
        Uninitialized,
        Initializing,
        Ready,
        Closed,
        Failed
    };
    enum class Method : std::uint8_t {
        Info,
        CreateJob,
        ListRuns
    };

    struct Pending {
        Method                                                               method;
        jb::core::TimePoint                                                  deadline;
        std::optional<jb::rpc::RequestId>                                    wire_id;
        std::variant<std::monostate, jb::core::JsonValue, jb::rpc::RpcError> outcome;
        bool                                                                 possibly_sent{false};
    };

    struct EarlyResponse {
        jb::rpc::RequestId                                   id;
        std::variant<jb::core::JsonValue, jb::rpc::RpcError> outcome;
    };

    Private(jb::rpc::Client& raw_client, AttributeRegistry const& attribute_registry)
        : rpc{raw_client}
        , attributes{attribute_registry}
    {}

    void bind_owner(ControlClient& client);
    void disconnect_sources() noexcept;
    void on_result(jb::rpc::RequestId const& id, jb::core::JsonValue const& value);
    void on_remote_error(jb::rpc::RequestId const& id, jb::rpc::RpcError const& error);
    void on_terminated(jb::core::Error const& error);
    void record_response(jb::rpc::RequestId const& id, std::variant<jb::core::JsonValue, jb::rpc::RpcError> outcome);
    void bind_wire_id(ControlCallId local_id, jb::rpc::RequestId const& wire_id);
    void schedule_delivery();
    void deliver_ready_outcomes();
    void deliver_one(ControlCallId id, Pending call);
    void fail_handshake(jb::core::Error error);
    void on_timeout();
    void rearm_deadline();
    void close(bool emit_failures);

    [[nodiscard]] auto
    start_call(Method method, std::string_view name, jb::core::JsonValue params, ControlCallOptions options)
        -> jb::core::Result<ControlCallId, jb::core::Error>;

    jb::rpc::Client&                       rpc;
    AttributeRegistry const&               attributes;
    ControlClient*                         owner{nullptr};
    jb::core::Timer                        deadline_timer;
    Phase                                  phase{Phase::Uninitialized};
    std::set<std::string>                  capabilities;
    std::map<ControlCallId, Pending>       pending;
    std::map<std::uint64_t, ControlCallId> wire_to_local;
    std::optional<ControlCallId>           establishing;
    std::vector<EarlyResponse>             early_responses;
    std::vector<ControlCallId>             ready_outcomes;
    ControlCallId                          next_id{1};
    bool                                   delivery_scheduled{false};
    std::uint64_t                          generation{0};
    std::shared_ptr<int>                   lifetime_guard{std::make_shared<int>(0)};

    jb::core::Connection result_connection;
    jb::core::Connection error_connection;
    jb::core::Connection terminal_connection;
    jb::core::Connection timeout_connection;
};

} // namespace jb::jobu
