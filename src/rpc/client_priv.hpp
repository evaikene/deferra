#pragma once

#include "client.hpp"
#include "connection.hpp"
#include "protocol_priv.hpp"

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace jb::rpc {

struct Client::Private {
    explicit Private(Client& client, jb::core::IODevice& client_device, ClientOptions client_options);

    [[nodiscard]] auto validate_request(std::string_view method, std::optional<JsonValue> const& params) const
        -> std::optional<jb::core::Error>;
    [[nodiscard]] auto allocate_request_id() const -> jb::core::Result<std::uint64_t, jb::core::Error>;
    void               advance_request_id(std::uint64_t id) noexcept;
    [[nodiscard]] auto write_frame(std::string const& frame, std::optional<std::uint64_t> pending_id)
        -> jb::core::Result<void, jb::core::Error>;

    void               process_readable();
    void               process_body(std::string const& body);
    [[nodiscard]] auto preflight_responses(detail::ResponseDocument const& document) const
        -> jb::core::Result<std::vector<std::uint64_t>, jb::core::Error>;
    void deliver_response(detail::ResponseEnvelope const& response, std::uint64_t id);

    void acknowledge_output(std::size_t bytes) noexcept;
    void handle_device_error(jb::core::IOError error);
    void terminate(jb::core::Error error, bool emit_protocol_error);
    void disconnect_device() noexcept;

    Client&                        owner;
    jb::core::IODevice*            device;
    ClientOptions const            options;
    StreamFramer                   framer;
    std::set<std::uint64_t>        pending_ids;
    std::set<std::uint64_t>        reserved_response_ids;
    std::uint64_t                  next_request_id{1U};
    std::size_t                    queued_output_bytes{0U};
    std::optional<jb::core::Error> terminal_error;

    jb::core::Connection ready_read_connection;
    jb::core::Connection bytes_written_connection;
    jb::core::Connection error_connection;
    jb::core::Connection closed_connection;

    bool processing_input{false};
    bool read_pending{false};
    bool write_in_progress{false};
    bool closed{false};
};

} // namespace jb::rpc
