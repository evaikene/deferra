#include "protocol_priv.hpp"

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace jb::rpc {
namespace {

using jb::core::Error;
using jb::core::ErrorCategory;

template <typename T>
auto make_json(T value) -> JsonValue
{
    JsonValue result;
    result.data = std::move(value);
    return result;
}

auto category_name(ErrorCategory category) -> std::string
{
    switch (category) {
        case ErrorCategory::InvalidArgument:
            return "invalid_argument";
        case ErrorCategory::NotFound:
            return "not_found";
        case ErrorCategory::Conflict:
            return "conflict";
        case ErrorCategory::PermissionDenied:
            return "permission_denied";
        case ErrorCategory::Unavailable:
            return "unavailable";
        case ErrorCategory::ResourceExhausted:
            return "resource_exhausted";
        case ErrorCategory::Cancelled:
            return "cancelled";
        case ErrorCategory::Timeout:
            return "timeout";
        case ErrorCategory::Io:
            return "io";
        case ErrorCategory::Unsupported:
            return "unsupported";
        case ErrorCategory::Internal:
            return "internal";
    }
    return "internal";
}

auto find_member(JsonValue::Object const& object, std::string_view name) -> JsonValue const*
{
    auto const iterator = object.find(name);
    return iterator == object.end() ? nullptr : &iterator->second;
}

auto has_valid_version(JsonValue::Object const& object) -> bool
{
    auto const* version = find_member(object, "jsonrpc");
    return version != nullptr && version->is_string() && version->as_string() == "2.0";
}

auto decode_request_id(JsonValue const& value) -> std::optional<RequestId>
{
    if (value.is_null()) {
        return RequestId{NullRequestId{}};
    }
    if (value.is_int()) {
        return RequestId{value.as_int()};
    }
    if (value.is_uint()) {
        return RequestId{value.as_uint()};
    }
    if (value.is_string()) {
        return RequestId{value.as_string()};
    }
    return std::nullopt;
}

auto decode_request_entry(JsonValue const& value) -> detail::RequestEntry
{
    if (!value.is_object()) {
        return detail::InvalidRequest{};
    }

    auto const& object = value.as_object();
    if (!has_valid_version(object)) {
        return detail::InvalidRequest{};
    }

    auto const* method = find_member(object, "method");
    if (method == nullptr || !method->is_string()) {
        return detail::InvalidRequest{};
    }

    auto const* params = find_member(object, "params");
    if (params != nullptr && !params->is_array() && !params->is_object()) {
        return detail::InvalidRequest{};
    }

    auto id = std::optional<RequestId>{};
    if (auto const* id_value = find_member(object, "id")) {
        id = decode_request_id(*id_value);
        if (!id) {
            return detail::InvalidRequest{};
        }
    }

    return detail::RequestEnvelope{
        .id     = std::move(id),
        .method = method->as_string(),
        .params = params == nullptr ? std::optional<JsonValue>{} : std::optional<JsonValue>{*params},
    };
}

auto decode_rpc_error(JsonValue const& value) -> std::optional<RpcError>
{
    if (!value.is_object()) {
        return std::nullopt;
    }

    auto const& object  = value.as_object();
    auto const* code    = find_member(object, "code");
    auto const* message = find_member(object, "message");
    if (code == nullptr || message == nullptr || !message->is_string()) {
        return std::nullopt;
    }

    auto numeric_code = std::int64_t{};
    if (code->is_int()) {
        numeric_code = code->as_int();
    }
    else if (code->is_uint() &&
             code->as_uint() <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        numeric_code = static_cast<std::int64_t>(code->as_uint());
    }
    else {
        return std::nullopt;
    }

    auto const* data = find_member(object, "data");
    return RpcError{
        .code    = numeric_code,
        .message = message->as_string(),
        .data    = data == nullptr ? std::optional<JsonValue>{} : std::optional<JsonValue>{*data},
    };
}

auto decode_response_envelope(JsonValue const& value) -> std::optional<detail::ResponseEnvelope>
{
    if (!value.is_object()) {
        return std::nullopt;
    }

    auto const& object = value.as_object();
    if (!has_valid_version(object)) {
        return std::nullopt;
    }

    auto const* id_value = find_member(object, "id");
    if (id_value == nullptr) {
        return std::nullopt;
    }
    auto id = decode_request_id(*id_value);
    if (!id) {
        return std::nullopt;
    }

    auto const* result = find_member(object, "result");
    auto const* error  = find_member(object, "error");
    if ((result == nullptr) == (error == nullptr)) {
        return std::nullopt;
    }

    if (result != nullptr) {
        return detail::ResponseEnvelope{
            .id      = std::move(*id),
            .payload = detail::ResponsePayload{*result},
        };
    }

    auto decoded_error = decode_rpc_error(*error);
    if (!decoded_error) {
        return std::nullopt;
    }
    return detail::ResponseEnvelope{
        .id      = std::move(*id),
        .payload = detail::ResponsePayload{std::move(*decoded_error)},
    };
}

auto make_protocol_error() -> Error
{
    return {
        .category = ErrorCategory::InvalidArgument,
        .code     = "rpc.protocol_error",
        .message  = "The peer sent an invalid JSON-RPC response",
    };
}

auto encode_request_id(RequestId const& id) -> JsonValue
{
    return std::visit(
        [](auto const& value) {
            using Value = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Value, NullRequestId>) {
                return make_json(JsonNull{});
            }
            else {
                return make_json(value);
            }
        },
        id);
}

auto encode_request_object(std::optional<RequestId> const& id,
                           std::string_view                method,
                           std::optional<JsonValue> const& params) -> JsonValue
{
    auto object = JsonValue::Object{
        {"jsonrpc", make_json(std::string{"2.0"}) },
        {"method",  make_json(std::string{method})},
    };
    if (id) {
        object.emplace("id", encode_request_id(*id));
    }
    if (params) {
        object.emplace("params", *params);
    }
    return make_json(std::move(object));
}

auto encode_error_object(RpcError const& error) -> JsonValue
{
    auto object = JsonValue::Object{
        {"code",    make_json(error.code)   },
        {"message", make_json(error.message)},
    };
    if (error.data) {
        object.emplace("data", *error.data);
    }
    return make_json(std::move(object));
}

} // anonymous namespace

auto application_error(jb::core::Error const& error) -> RpcError
{
    auto data = JsonValue::Object{};
    data.emplace("category", make_json(category_name(error.category)));
    data.emplace("code", make_json(error.code));

    return {
        .code    = static_cast<std::int64_t>(ErrorCode::ApplicationError),
        .message = error.message,
        .data    = make_json(std::move(data)),
    };
}

namespace detail {

auto decode_request_document(JsonValue const& value, std::size_t max_batch_entries) -> RequestDocument
{
    if (!value.is_array()) {
        return {
            .kind    = RequestDocumentKind::Single,
            .entries = {decode_request_entry(value)},
        };
    }

    auto const& array = value.as_array();
    if (array.empty() || array.size() > max_batch_entries) {
        return {
            .kind    = RequestDocumentKind::RejectedBatch,
            .entries = {InvalidRequest{}},
        };
    }

    auto entries = std::vector<RequestEntry>{};
    entries.reserve(array.size());
    for (auto const& entry : array) {
        entries.push_back(decode_request_entry(entry));
    }
    return {
        .kind    = RequestDocumentKind::Batch,
        .entries = std::move(entries),
    };
}

auto decode_response_document(JsonValue const& value, std::size_t max_batch_entries)
    -> jb::core::Result<ResponseDocument, jb::core::Error>
{
    using Result = jb::core::Result<ResponseDocument, jb::core::Error>;

    if (!value.is_array()) {
        auto response = decode_response_envelope(value);
        if (!response) {
            return Result::failure(make_protocol_error());
        }
        return Result::success({
            .kind    = ResponseDocumentKind::Single,
            .entries = {std::move(*response)},
        });
    }

    auto const& array = value.as_array();
    if (array.empty() || array.size() > max_batch_entries) {
        return Result::failure(make_protocol_error());
    }

    auto entries = std::vector<ResponseEnvelope>{};
    entries.reserve(array.size());
    for (auto const& value_entry : array) {
        auto entry = decode_response_envelope(value_entry);
        if (!entry) {
            return Result::failure(make_protocol_error());
        }
        entries.push_back(std::move(*entry));
    }
    return Result::success({
        .kind    = ResponseDocumentKind::Batch,
        .entries = std::move(entries),
    });
}

auto encode_request(RequestId const& id, std::string_view method, std::optional<JsonValue> const& params) -> JsonValue
{
    return encode_request_object(id, method, params);
}

auto encode_notification(std::string_view method, std::optional<JsonValue> const& params) -> JsonValue
{
    return encode_request_object(std::nullopt, method, params);
}

auto encode_success_response(RequestId const& id, JsonValue const& result) -> JsonValue
{
    return make_json(JsonValue::Object{
        {"id",      encode_request_id(id)        },
        {"jsonrpc", make_json(std::string{"2.0"})},
        {"result",  result                       },
    });
}

auto encode_error_response(RequestId const& id, RpcError const& error) -> JsonValue
{
    return make_json(JsonValue::Object{
        {"error",   encode_error_object(error)   },
        {"id",      encode_request_id(id)        },
        {"jsonrpc", make_json(std::string{"2.0"})},
    });
}

auto encode_batch(std::vector<JsonValue> entries) -> std::optional<JsonValue>
{
    if (entries.empty()) {
        return std::nullopt;
    }
    return make_json(std::move(entries));
}

auto make_standard_error(ErrorCode code) -> RpcError
{
    auto message = std::string{};
    switch (code) {
        case ErrorCode::ParseError:
            message = "Parse error";
            break;
        case ErrorCode::InvalidRequest:
            message = "Invalid Request";
            break;
        case ErrorCode::MethodNotFound:
            message = "Method not found";
            break;
        case ErrorCode::InvalidParams:
            message = "Invalid params";
            break;
        case ErrorCode::InternalError:
            message = "Internal error";
            break;
        case ErrorCode::ApplicationError:
            break;
    }
    return {
        .code    = static_cast<std::int64_t>(code),
        .message = std::move(message),
    };
}

auto is_reserved_method(std::string_view method) noexcept -> bool
{
    return method.starts_with("rpc.");
}

} // namespace detail
} // namespace jb::rpc
