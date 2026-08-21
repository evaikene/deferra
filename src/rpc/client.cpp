#include "client.hpp"

#include "client_priv.hpp"

#include <cassert>
#include <cstdint>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace jb::rpc {
namespace {

using jb::core::Error;
using jb::core::ErrorCategory;

auto invalid_method_error() -> Error
{
    return {
        .category = ErrorCategory::InvalidArgument,
        .code     = "rpc.invalid_argument",
        .message  = "RPC method must not be empty",
    };
}

auto invalid_params_error() -> Error
{
    return {
        .category = ErrorCategory::InvalidArgument,
        .code     = "rpc.invalid_argument",
        .message  = "RPC parameters must be an object or array",
    };
}

auto reentrant_write_error() -> Error
{
    return {
        .category = ErrorCategory::InvalidArgument,
        .code     = "rpc.invalid_argument",
        .message  = "RPC client does not permit reentrant writes",
    };
}

auto closed_operation_error() -> Error
{
    return {
        .category = ErrorCategory::Unavailable,
        .code     = "rpc.connection_closed",
        .message  = "RPC client is closed",
    };
}

auto explicit_close_error() -> Error
{
    return {
        .category = ErrorCategory::Cancelled,
        .code     = "rpc.connection_closed",
        .message  = "RPC client was closed",
    };
}

auto device_closed_error() -> Error
{
    return {
        .category = ErrorCategory::Unavailable,
        .code     = "rpc.connection_closed",
        .message  = "RPC connection closed",
    };
}

auto pending_limit_error() -> Error
{
    return {
        .category = ErrorCategory::ResourceExhausted,
        .code     = "rpc.pending_limit",
        .message  = "RPC client pending request limit reached",
    };
}

auto output_limit_error() -> Error
{
    return {
        .category = ErrorCategory::ResourceExhausted,
        .code     = "rpc.output_limit",
        .message  = "RPC client output limit reached",
    };
}

auto short_write_error() -> Error
{
    return {
        .category = ErrorCategory::Io,
        .code     = "rpc.short_write",
        .message  = "RPC client device did not accept the complete frame",
    };
}

auto response_protocol_error() -> Error
{
    return {
        .category = ErrorCategory::InvalidArgument,
        .code     = "rpc.protocol_error",
        .message  = "The peer sent an invalid JSON-RPC response",
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

} // anonymous namespace

Client::Private::Private(Client& client, jb::core::IODevice& client_device, ClientOptions client_options)
    : owner(client)
    , device(&client_device)
    , options(client_options)
    , framer(options.framing)
{
    assert(device->is_open());
    assert(owner.event_loop() != nullptr);
    assert(device->event_loop() == owner.event_loop());

    auto* private_data = this;
    ready_read_connection =
        device->ready_read.connect(&owner, [private_data]() -> void { private_data->process_readable(); });
    bytes_written_connection = device->bytes_written.connect(&owner, [private_data](std::size_t bytes) -> void {
        private_data->acknowledge_output(bytes);
    });
    error_connection =
        device->error_occurred.connect(&owner, [private_data](jb::core::IOError error, std::string const&) -> void {
            private_data->handle_device_error(error);
        });
    closed_connection = device->closed.connect(&owner, [private_data]() -> void {
        private_data->terminate(device_closed_error(), false);
    });
}

auto Client::Private::validate_request(std::string_view method, std::optional<JsonValue> const& params) const
    -> std::optional<Error>
{
    if (method.empty()) {
        return invalid_method_error();
    }
    if (params && !params->is_object() && !params->is_array()) {
        return invalid_params_error();
    }
    return std::nullopt;
}

auto Client::Private::allocate_request_id() const -> jb::core::Result<std::uint64_t, Error>
{
    using Result = jb::core::Result<std::uint64_t, Error>;

    auto candidate = next_request_id;
    auto attempts  = pending_ids.size() + reserved_response_ids.size() + 1U;
    while (attempts-- > 0U) {
        if (candidate != 0U && !pending_ids.contains(candidate) && !reserved_response_ids.contains(candidate)) {
            return Result::success(candidate);
        }
        candidate = candidate == std::numeric_limits<std::uint64_t>::max() ? 1U : candidate + 1U;
    }
    return Result::failure(pending_limit_error());
}

void Client::Private::advance_request_id(std::uint64_t id) noexcept
{
    next_request_id = id == std::numeric_limits<std::uint64_t>::max() ? 1U : id + 1U;
}

auto Client::Private::write_frame(std::string const& frame, std::optional<std::uint64_t> pending_id)
    -> jb::core::Result<void, Error>
{
    using Result = jb::core::Result<void, Error>;

    if (closed) {
        return Result::failure(closed_operation_error());
    }
    if (write_in_progress) {
        return Result::failure(reentrant_write_error());
    }

    auto const limit = options.max_queued_output_bytes;
    if (queued_output_bytes > limit || frame.size() > limit - queued_output_bytes) {
        auto error = output_limit_error();
        terminate(error, true);
        return Result::failure(std::move(error));
    }

    queued_output_bytes += frame.size();
    write_in_progress    = true;
    auto const written   = device->write(frame);
    write_in_progress    = false;

    if (closed) {
        return Result::failure(terminal_error.value_or(closed_operation_error()));
    }
    if (written != frame.size()) {
        auto error = short_write_error();
        terminate(error, true);
        return Result::failure(std::move(error));
    }

    if (pending_id) {
        pending_ids.insert(*pending_id);
        advance_request_id(*pending_id);
    }
    if (read_pending) {
        process_readable();
    }
    return Result::success();
}

void Client::Private::process_readable()
{
    if (closed) {
        return;
    }
    if (write_in_progress || processing_input) {
        read_pending = true;
        return;
    }

    processing_input = true;
    for (;;) {
        read_pending = false;
        auto bytes   = device->read_all();
        if (closed) {
            return;
        }

        if (!bytes.empty()) {
            auto bodies = framer.append(bytes);
            if (!bodies) {
                auto error = std::move(bodies).error();
                terminate(std::move(error), true);
                return;
            }

            for (auto const& body : std::move(bodies).value()) {
                process_body(body);
                if (closed) {
                    return;
                }
            }
        }

        if (!read_pending) {
            processing_input = false;
            return;
        }
    }
}

void Client::Private::process_body(std::string const& body)
{
    auto parsed = parse_json(body, options.json);
    if (!parsed) {
        auto error = std::move(parsed).error();
        terminate(std::move(error), true);
        return;
    }

    auto document = detail::decode_response_document(parsed.value(), options.max_batch_entries);
    if (!document) {
        auto error = std::move(document).error();
        terminate(std::move(error), true);
        return;
    }

    auto response_ids = preflight_responses(document.value());
    if (!response_ids) {
        auto error = std::move(response_ids).error();
        terminate(std::move(error), true);
        return;
    }

    reserved_response_ids.insert(response_ids->begin(), response_ids->end());
    auto const& entries = document->entries;
    for (auto index = std::size_t{0U}; index < entries.size(); ++index) {
        if (closed) {
            return;
        }

        auto const id = response_ids.value()[index];
        reserved_response_ids.erase(id);
        if (pending_ids.erase(id) == 0U) {
            continue;
        }

        deliver_response(entries[index], id);
        if (closed) {
            return;
        }
    }
    reserved_response_ids.clear();
}

auto Client::Private::preflight_responses(detail::ResponseDocument const& document) const
    -> jb::core::Result<std::vector<std::uint64_t>, Error>
{
    using Result = jb::core::Result<std::vector<std::uint64_t>, Error>;

    auto ids  = std::vector<std::uint64_t>{};
    auto seen = std::set<std::uint64_t>{};
    ids.reserve(document.entries.size());

    for (auto const& response : document.entries) {
        auto const* id = std::get_if<std::uint64_t>(&response.id);
        if (id == nullptr || *id == 0U || !pending_ids.contains(*id) || !seen.insert(*id).second) {
            return Result::failure(response_protocol_error());
        }
        ids.push_back(*id);
    }
    return Result::success(std::move(ids));
}

void Client::Private::deliver_response(detail::ResponseEnvelope const& response, std::uint64_t id)
{
    auto request_id = RequestId{id};
    if (std::holds_alternative<JsonValue>(response.payload)) {
        owner.emit(owner.result_received, request_id, std::get<JsonValue>(response.payload));
        return;
    }
    owner.emit(owner.error_received, request_id, std::get<RpcError>(response.payload));
}

void Client::Private::acknowledge_output(std::size_t bytes) noexcept
{
    if (closed) {
        return;
    }
    queued_output_bytes = bytes >= queued_output_bytes ? 0U : queued_output_bytes - bytes;
}

void Client::Private::handle_device_error(jb::core::IOError error)
{
    if (error == jb::core::IOError::NoError) {
        return;
    }
    terminate(device_error(error), false);
}

void Client::Private::terminate(Error error, bool emit_protocol_error)
{
    if (closed) {
        return;
    }

    closed         = true;
    terminal_error = std::move(error);
    disconnect_device();
    framer.reset();
    read_pending        = false;
    queued_output_bytes = 0U;
    reserved_response_ids.clear();

    auto failed_ids = std::vector<std::uint64_t>{pending_ids.begin(), pending_ids.end()};
    pending_ids.clear();

    auto const& terminal = *terminal_error;
    if (emit_protocol_error) {
        owner.emit(owner.protocol_error, terminal);
    }
    for (auto const id : failed_ids) {
        owner.emit(owner.request_failed, RequestId{id}, terminal);
    }
}

void Client::Private::disconnect_device() noexcept
{
    ready_read_connection.disconnect();
    bytes_written_connection.disconnect();
    error_connection.disconnect();
    closed_connection.disconnect();
}

Client::Client(jb::core::IODevice& device, ClientOptions options, jb::core::Object* parent)
    : Object(parent)
    , _data(std::make_unique<Private>(*this, device, options))
{}

Client::~Client()
{
    close();
}

auto Client::call(std::string_view method, std::optional<JsonValue> params)
    -> jb::core::Result<RequestId, jb::core::Error>
{
    using Result = jb::core::Result<RequestId, jb::core::Error>;

    if (_data->closed) {
        return Result::failure(closed_operation_error());
    }
    if (_data->write_in_progress) {
        return Result::failure(reentrant_write_error());
    }
    if (auto error = _data->validate_request(method, params)) {
        return Result::failure(std::move(*error));
    }

    auto allocated = _data->allocate_request_id();
    if (!allocated) {
        return Result::failure(std::move(allocated).error());
    }
    auto const id = allocated.value();

    auto serialized = serialize_json(detail::encode_request(RequestId{id}, method, std::move(params)));
    if (!serialized) {
        return Result::failure(std::move(serialized).error());
    }
    auto framed = frame_message(serialized.value(), _data->options.framing);
    if (!framed) {
        return Result::failure(std::move(framed).error());
    }
    if (_data->pending_ids.size() >= _data->options.max_pending_requests) {
        return Result::failure(pending_limit_error());
    }

    auto written = _data->write_frame(framed.value(), id);
    if (!written) {
        return Result::failure(std::move(written).error());
    }
    return Result::success(RequestId{id});
}

auto Client::notify(std::string_view method, std::optional<JsonValue> params) -> jb::core::Result<void, jb::core::Error>
{
    using Result = jb::core::Result<void, jb::core::Error>;

    if (_data->closed) {
        return Result::failure(closed_operation_error());
    }
    if (_data->write_in_progress) {
        return Result::failure(reentrant_write_error());
    }
    if (auto error = _data->validate_request(method, params)) {
        return Result::failure(std::move(*error));
    }

    auto serialized = serialize_json(detail::encode_notification(method, std::move(params)));
    if (!serialized) {
        return Result::failure(std::move(serialized).error());
    }
    auto framed = frame_message(serialized.value(), _data->options.framing);
    if (!framed) {
        return Result::failure(std::move(framed).error());
    }
    return _data->write_frame(framed.value(), std::nullopt);
}

void Client::cancel(RequestId const& id)
{
    if (_data->closed) {
        return;
    }
    if (auto const* unsigned_id = std::get_if<std::uint64_t>(&id)) {
        _data->pending_ids.erase(*unsigned_id);
    }
}

void Client::close()
{
    _data->terminate(explicit_close_error(), false);
}

auto Client::pending_request_count() const noexcept -> std::size_t
{
    return _data->pending_ids.size();
}

} // namespace jb::rpc
