#include "server.hpp"

#include "logging.hpp"
#include "server_priv.hpp"

#include <exception>
#include <limits>
#include <utility>

namespace jb::rpc {
namespace {

using jb::core::Error;
using jb::core::ErrorCategory;
using jb::core::JsonValue;
using jb::core::parse_json;
using jb::core::serialize_json;

auto invalid_connection_error() -> Error
{
    return {
        .category = ErrorCategory::InvalidArgument,
        .code     = "rpc.invalid_argument",
        .message  = "RPC connection requires an open device on the server event loop",
    };
}

auto connection_limit_error() -> Error
{
    return {
        .category = ErrorCategory::ResourceExhausted,
        .code     = "rpc.connection_limit",
        .message  = "RPC server connection limit reached",
    };
}

auto output_limit_error() -> Error
{
    return {
        .category = ErrorCategory::ResourceExhausted,
        .code     = "rpc.output_limit",
        .message  = "RPC connection output limit reached",
    };
}

auto short_write_error() -> Error
{
    return {
        .category = ErrorCategory::Io,
        .code     = "rpc.short_write",
        .message  = "RPC connection did not accept the complete frame",
    };
}

auto device_error(jb::core::IOError error) -> Error
{
    auto category = ErrorCategory::Io;
    switch (error) {
        case jb::core::IOError::InvalidArgument:
            category = ErrorCategory::InvalidArgument;
            break;
        case jb::core::IOError::Unsupported:
            category = ErrorCategory::Unsupported;
            break;
        case jb::core::IOError::ResourceError:
            category = ErrorCategory::ResourceExhausted;
            break;
        case jb::core::IOError::NoError:
        case jb::core::IOError::NotOpen:
        case jb::core::IOError::OpenError:
        case jb::core::IOError::ReadError:
        case jb::core::IOError::WriteError:
        case jb::core::IOError::CloseError:
        case jb::core::IOError::SeekError:
            break;
    }
    return {
        .category = category,
        .code     = "rpc.connection_closed",
        .message  = "RPC connection device failed",
    };
}

void log_handler_failure(std::string_view reason) noexcept
{
    try {
        jb::core::log_error("RPC method handler failed: {}", reason);
    }
    catch (...) {
        // Logging must not allow a secondary failure to escape this noexcept recovery path.
        return;
    }
}

auto internal_error_response(RequestId const& id) -> JsonValue
{
    return detail::encode_error_response(id, detail::make_standard_error(ErrorCode::InternalError));
}

} // anonymous namespace

Server::Private::ConnectionState::ConnectionState(ConnectionId         connection_id,
                                                  jb::core::IODevice*  connection_device,
                                                  OperationContext&&   connection_operation,
                                                  FramingLimits const& framing_limits)
    : id(connection_id)
    , device(connection_device)
    , operation(std::move(connection_operation))
    , framer(framing_limits)
{}

Server::Private::Private(ServerOptions server_options)
    : options(server_options)
{}

void Server::Private::bind_owner(Server& server)
{
    owner = &server;
}

auto Server::Private::allocate_connection_id() -> jb::core::Result<ConnectionId, jb::core::Error>
{
    using Result = jb::core::Result<ConnectionId, jb::core::Error>;

    if (connection_ids_exhausted) {
        return Result::failure(connection_limit_error());
    }

    auto const id = next_connection_id;
    if (id == std::numeric_limits<ConnectionId>::max()) {
        connection_ids_exhausted = true;
    }
    else {
        ++next_connection_id;
    }
    return Result::success(id);
}

void Server::Private::process_readable(ConnectionId id)
{
    auto iterator = connections.find(id);
    if (iterator == connections.end() || iterator->second.closing) {
        return;
    }
    if (iterator->second.processing_input) {
        iterator->second.read_pending = true;
        return;
    }

    iterator->second.processing_input = true;
    for (;;) {
        iterator = connections.find(id);
        if (iterator == connections.end() || iterator->second.closing) {
            return;
        }

        iterator->second.read_pending = false;
        auto* device                  = iterator->second.device;
        auto  bytes                   = device->read_all();

        iterator = connections.find(id);
        if (iterator == connections.end() || iterator->second.closing) {
            return;
        }

        if (!bytes.empty()) {
            auto bodies = iterator->second.framer.append(bytes);
            if (!bodies) {
                auto error = std::move(bodies).error();
                fail_connection(id, error);
                return;
            }

            for (auto const& body : std::move(bodies).value()) {
                process_body(id, body);
                iterator = connections.find(id);
                if (iterator == connections.end() || iterator->second.closing) {
                    return;
                }
            }
        }

        iterator = connections.find(id);
        if (iterator == connections.end() || iterator->second.closing) {
            return;
        }
        if (!iterator->second.read_pending) {
            iterator->second.processing_input = false;
            return;
        }
    }
}

void Server::Private::process_body(ConnectionId id, std::string const& body)
{
    auto iterator = connections.find(id);
    if (iterator == connections.end() || iterator->second.closing) {
        return;
    }

    auto parsed = parse_json(body, options.json);
    if (!parsed) {
        auto response =
            detail::encode_error_response(NullRequestId{}, detail::make_standard_error(ErrorCode::ParseError));
        static_cast<void>(write_response(id, response));
        return;
    }

    auto document = detail::decode_request_document(parsed.value(), options.max_batch_entries);
    auto response = dispatch_document(id, document);
    if (response && connections.contains(id)) {
        static_cast<void>(write_response(id, *response));
    }
}

auto Server::Private::dispatch_document(ConnectionId id, detail::RequestDocument const& document)
    -> std::optional<JsonValue>
{
    auto responses = std::vector<JsonValue>{};
    responses.reserve(document.entries.size());

    for (auto const& entry : document.entries) {
        auto response = dispatch_entry(id, entry);
        auto iterator = connections.find(id);
        if (iterator == connections.end() || iterator->second.closing) {
            return std::nullopt;
        }
        if (response) {
            responses.push_back(std::move(*response));
        }
    }

    if (document.kind == detail::RequestDocumentKind::Batch) {
        return detail::encode_batch(std::move(responses));
    }
    if (responses.empty()) {
        return std::nullopt;
    }
    return std::move(responses.front());
}

auto Server::Private::dispatch_entry(ConnectionId id, detail::RequestEntry const& entry) -> std::optional<JsonValue>
{
    if (std::holds_alternative<detail::InvalidRequest>(entry)) {
        return detail::encode_error_response(NullRequestId{}, detail::make_standard_error(ErrorCode::InvalidRequest));
    }

    auto const& request = std::get<detail::RequestEnvelope>(entry);
    auto const  method  = methods.find(request.method);
    if (method == methods.end()) {
        if (!request.id) {
            return std::nullopt;
        }
        return detail::encode_error_response(*request.id, detail::make_standard_error(ErrorCode::MethodNotFound));
    }

    auto iterator = connections.find(id);
    if (iterator == connections.end() || iterator->second.closing) {
        return std::nullopt;
    }

    auto handler = method->second;
    auto context = RequestContext{
        .connection_id = id,
        .operation     = iterator->second.operation,
    };

    try {
        auto result = handler(context, request.params);

        iterator = connections.find(id);
        if (iterator == connections.end() || iterator->second.closing) {
            return std::nullopt;
        }

        if (!request.id) {
            if (!result.is_initialized() || !result.has_value()) {
                log_handler_failure("notification handler returned a failure");
            }
            return std::nullopt;
        }

        auto response = JsonValue{};
        if (!result.is_initialized()) {
            log_handler_failure("handler returned an uninitialized result");
            response = internal_error_response(*request.id);
        }
        else if (result.has_value()) {
            response = detail::encode_success_response(*request.id, result.value());
        }
        else {
            response = detail::encode_error_response(*request.id, result.error());
        }

        if (!serialize_json(response)) {
            log_handler_failure("handler returned a value that cannot be serialized");
            return internal_error_response(*request.id);
        }
        return response;
    }
    catch (std::exception const& exception) {
        log_handler_failure(exception.what());
    }
    catch (...) {
        log_handler_failure("unknown exception");
    }

    if (!request.id || !connections.contains(id)) {
        return std::nullopt;
    }
    return internal_error_response(*request.id);
}

auto Server::Private::write_response(ConnectionId id, JsonValue const& response) -> bool
{
    auto serialized = serialize_json(response);
    if (!serialized) {
        auto error = std::move(serialized).error();
        fail_connection(id, error);
        return false;
    }

    auto framed = frame_message(serialized.value(), options.framing);
    if (!framed) {
        auto error = std::move(framed).error();
        fail_connection(id, error);
        return false;
    }

    auto frame    = std::move(framed).value();
    auto iterator = connections.find(id);
    if (iterator == connections.end() || iterator->second.closing) {
        return false;
    }

    auto const limit  = options.max_queued_output_bytes;
    auto const queued = iterator->second.queued_output_bytes;
    if (queued > limit || frame.size() > limit - queued) {
        fail_connection(id, output_limit_error());
        return false;
    }

    iterator->second.queued_output_bytes += frame.size();
    auto* device                          = iterator->second.device;
    auto  written                         = device->write(frame);

    iterator = connections.find(id);
    if (iterator == connections.end() || iterator->second.closing) {
        return false;
    }
    if (written != frame.size()) {
        fail_connection(id, short_write_error());
        return false;
    }
    return true;
}

void Server::Private::acknowledge_output(ConnectionId id, std::size_t bytes)
{
    auto iterator = connections.find(id);
    if (iterator == connections.end() || iterator->second.closing) {
        return;
    }

    auto& queued = iterator->second.queued_output_bytes;
    queued       = bytes >= queued ? 0U : queued - bytes;
}

void Server::Private::handle_device_error(ConnectionId id, jb::core::IOError error)
{
    if (error == jb::core::IOError::NoError) {
        return;
    }
    fail_connection(id, device_error(error));
}

void Server::Private::fail_connection(ConnectionId id, jb::core::Error const& error)
{
    auto iterator = connections.find(id);
    if (iterator == connections.end() || iterator->second.closing || iterator->second.error_reported) {
        return;
    }

    iterator->second.error_reported = true;
    owner->emit(owner->connection_error, id, error);

    iterator = connections.find(id);
    if (iterator != connections.end() && !iterator->second.closing) {
        close_connection(id);
    }
}

void Server::Private::close_connection(ConnectionId id)
{
    auto iterator = connections.find(id);
    if (iterator == connections.end() || iterator->second.closing) {
        return;
    }

    iterator->second.closing = true;
    iterator->second.device->close();
}

void Server::Private::retire_connection(ConnectionId id)
{
    auto iterator = connections.find(id);
    if (iterator == connections.end()) {
        return;
    }

    auto* device = iterator->second.device;
    iterator->second.ready_read_connection.disconnect();
    iterator->second.bytes_written_connection.disconnect();
    iterator->second.error_connection.disconnect();
    iterator->second.closed_connection.disconnect();
    connections.erase(iterator);

    owner->emit(owner->connection_closed, id);
    device->delete_later();
}

Server::Server(ServerOptions options, jb::core::Object* parent)
    : Object(*new Private{options}, parent)
{
    // Bind only after Object is fully constructed so Private never receives a partially constructed Server.
    d_ptr<Private>()->bind_owner(*this);
}

Server::~Server()
{
    // Disconnect device activity before Server's signal members are destroyed.
    close();
}

auto Server::register_method(std::string name, MethodHandler handler) -> bool
{
    if (name.empty() || !handler || detail::is_reserved_method(name)) {
        return false;
    }
    return d_ptr<Private>()->methods.emplace(std::move(name), std::move(handler)).second;
}

auto Server::unregister_method(std::string_view name) -> bool
{
    auto*      data     = d_ptr<Private>();
    auto const iterator = data->methods.find(name);
    if (iterator == data->methods.end()) {
        return false;
    }
    data->methods.erase(iterator);
    return true;
}

auto Server::has_method(std::string_view name) const noexcept -> bool
{
    return d_ptr<Private const>()->methods.contains(name);
}

auto Server::add_connection(std::unique_ptr<jb::core::IODevice> device, OperationContext operation)
    -> jb::core::Result<ConnectionId, jb::core::Error>
{
    using Result = jb::core::Result<ConnectionId, jb::core::Error>;

    auto* data = d_ptr<Private>();
    if (!device || !device->is_open() || !event_loop() || device->event_loop() != event_loop()) {
        return Result::failure(invalid_connection_error());
    }
    if (data->connections.size() >= data->options.max_connections) {
        return Result::failure(connection_limit_error());
    }

    auto allocated = data->allocate_connection_id();
    if (!allocated) {
        return Result::failure(std::move(allocated).error());
    }
    auto const id  = allocated.value();
    auto*      raw = device.get();

    auto connection_operation = std::move(operation);
    auto [iterator, inserted] =
        data->connections.try_emplace(id, id, raw, std::move(connection_operation), data->options.framing);
    if (!inserted) {
        return Result::failure(connection_limit_error());
    }

    // Receiver tracking prevents device activity from reaching the private block after Server destruction begins.
    iterator->second.ready_read_connection =
        raw->ready_read.connect(this, [data, id]() -> void { data->process_readable(id); });
    iterator->second.bytes_written_connection = raw->bytes_written.connect(this, [data, id](std::size_t bytes) -> void {
        data->acknowledge_output(id, bytes);
    });
    iterator->second.error_connection =
        raw->error_occurred.connect(this, [data, id](jb::core::IOError error, std::string const&) -> void {
            data->handle_device_error(id, error);
        });
    iterator->second.closed_connection =
        raw->closed.connect(this, [data, id]() -> void { data->retire_connection(id); });

    if (!raw->set_parent(this)) {
        iterator->second.ready_read_connection.disconnect();
        iterator->second.bytes_written_connection.disconnect();
        iterator->second.error_connection.disconnect();
        iterator->second.closed_connection.disconnect();
        data->connections.erase(iterator);
        return Result::failure(invalid_connection_error());
    }
    [[maybe_unused]] auto* released_device = device.release();

    emit(connection_opened, id);
    auto const current = data->connections.find(id);
    if (current != data->connections.end() && !current->second.closing) {
        data->process_readable(id);
    }
    return Result::success(id);
}

void Server::close_connection(ConnectionId id)
{
    d_ptr<Private>()->close_connection(id);
}

void Server::close()
{
    auto* data = d_ptr<Private>();
    auto  ids  = std::vector<ConnectionId>{};
    ids.reserve(data->connections.size());
    for (auto const& [id, connection] : data->connections) {
        static_cast<void>(connection);
        ids.push_back(id);
    }
    for (auto const id : ids) {
        data->close_connection(id);
    }
}

auto Server::connection_count() const noexcept -> std::size_t
{
    return d_ptr<Private const>()->connections.size();
}

} // namespace jb::rpc
