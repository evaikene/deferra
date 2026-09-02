#pragma once

#include "connection.hpp"
#include "object_priv.hpp"
#include "protocol_priv.hpp"
#include "server.hpp"

#include <map>
#include <optional>

namespace jb::rpc {

struct Server::Private : jb::core::priv::ObjectPrivate {
    struct ConnectionState {
        ConnectionState(ConnectionId         connection_id,
                        jb::core::IODevice*  connection_device,
                        OperationContext&&   connection_operation,
                        FramingLimits const& framing_limits);

        ConnectionId        id;
        jb::core::IODevice* device;
        OperationContext    operation;
        StreamFramer        framer;
        std::size_t         queued_output_bytes{0};

        jb::core::Connection ready_read_connection;
        jb::core::Connection bytes_written_connection;
        jb::core::Connection error_connection;
        jb::core::Connection closed_connection;

        bool processing_input{false};
        bool read_pending{false};
        bool closing{false};
        bool error_reported{false};
    };

    explicit Private(ServerOptions server_options);

    void bind_owner(Server& server);

    [[nodiscard]] auto allocate_connection_id() -> jb::core::Result<ConnectionId, jb::core::Error>;
    void               process_readable(ConnectionId id);
    void               process_body(ConnectionId id, std::string const& body);
    [[nodiscard]] auto dispatch_document(ConnectionId id, detail::RequestDocument const& document)
        -> std::optional<jb::core::JsonValue>;
    [[nodiscard]] auto dispatch_entry(ConnectionId id, detail::RequestEntry const& entry)
        -> std::optional<jb::core::JsonValue>;
    [[nodiscard]] auto write_response(ConnectionId id, jb::core::JsonValue const& response) -> bool;
    void               acknowledge_output(ConnectionId id, std::size_t bytes);
    void               handle_device_error(ConnectionId id, jb::core::IOError error);
    void               fail_connection(ConnectionId id, jb::core::Error const& error);
    void               close_connection(ConnectionId id);
    void               retire_connection(ConnectionId id);

    Server*                                           owner{nullptr};
    ServerOptions const                               options;
    std::map<std::string, MethodHandler, std::less<>> methods;
    std::map<ConnectionId, ConnectionState>           connections;
    ConnectionId                                      next_connection_id{1};
    bool                                              connection_ids_exhausted{false};
};

} // namespace jb::rpc
